package hkregion.hmcl;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** Reads actual instructions and cache dependencies; no archive hash or method-name allowlist. */
final class HmclClassAnalyzer {
    static final String TARGET = "org/jackhuang/hmcl/util/RegionUtils";
    record Member(String owner, String name, String descriptor) {}
    record Instruction(int opcode, Member member, int constant) {}
    record Plan(List<String> machine, List<String> network, String machineGetter, String networkGetter) {}
    record Result(byte[] bytes, Plan plan) {}
    private record Field(int flags, String descriptor) {}
    private static final class Method {
        int flags, attributeStart = -1, attributeEnd, attributeName;
        String name, descriptor;
        final List<Instruction> instructions = new ArrayList<>();
        final Set<String> strings = new LinkedHashSet<>();
        final Set<Member> calls = new LinkedHashSet<>(), reads = new LinkedHashSet<>(), writes = new LinkedHashSet<>();
        boolean predicate() { return (flags & (8 | 0x100 | 0x400)) == 8 && "()Z".equals(descriptor) && attributeStart >= 0; }
        boolean calls(String owner, String name) { return calls.stream().anyMatch(r -> r.owner.equals(owner) && r.name.equals(name)); }
    }

    static Result transform(byte[] bytes) throws IOException {
        Image image = new Image(bytes);
        Set<Method> machine = new LinkedHashSet<>(), network = new LinkedHashSet<>();
        for (Method method : image.methods) {
            if (!method.predicate()) continue;
            boolean networkCall = method.calls.stream().anyMatch(r ->
                r.owner.startsWith("org/jackhuang/hmcl/") && r.owner.endsWith("/NetworkUtils") && r.name.startsWith("doGet")
                || r.owner.startsWith("java/net/") && Set.of("send", "sendAsync", "openConnection", "openStream", "getInputStream").contains(r.name));
            boolean country = method.reads.stream().anyMatch(r -> countryName(r.name))
                || method.calls.stream().anyMatch(r -> countryName(r.name));
            boolean endpoint = method.strings.stream().anyMatch(s -> s.startsWith("https://ipwho.is") || s.startsWith("http://ipwho.is"));
            boolean remote = method.strings.contains("CN") && networkCall && (country || endpoint);
            boolean zone = method.calls("java/time/ZoneId", "systemDefault") || method.calls("java/util/TimeZone", "getDefault");
            boolean local = zone && method.strings.contains("Asia/Shanghai") &&
                (method.strings.contains("CN") || method.calls.stream().anyMatch(r -> r.name.equals("GetUserGeoID")));
            if (remote) network.add(method);
            else if (local) machine.add(method);
        }
        if (machine.size() != 1 || network.size() != 1)
            throw new IOException("Ambiguous or missing regional roots: machine=" + names(machine) + "; network=" + names(network));

        // Follow the actual <clinit> assignment, including optional Boolean boxing.
        Set<Member> machineFields = new LinkedHashSet<>(), networkFields = new LinkedHashSet<>();
        for (Method method : image.methods) {
            if (!method.name.equals("<clinit>")) continue;
            for (int i = 0; i < method.instructions.size(); ++i) {
                Instruction insn = method.instructions.get(i);
                if (insn.opcode != 184 || !selected(insn.member, machine)) continue;
                int next = i + 1;
                if (next < method.instructions.size() && boxing(method.instructions.get(next))) ++next;
                if (next < method.instructions.size()) {
                    Instruction store = method.instructions.get(next);
                    if (store.opcode == 179 && image.booleanField(store.member)) machineFields.add(store.member);
                }
            }
        }
        // A network memoizer must call the recognized network computation and
        // write a boolean cache. Later checks reject unrelated calls/side effects.
        for (Method method : image.methods) {
            if (method.predicate() && method.calls.stream().anyMatch(r -> selected(r, network)))
                for (Member field : method.writes) if (image.booleanField(field)) networkFields.add(field);
        }
        if (networkFields.size() > 1) throw new IOException("Ambiguous network caches: " + networkFields);
        boolean changed;
        do {
            changed = false;
            for (Method method : image.methods) {
                if (!method.predicate() || machine.contains(method) || network.contains(method)) continue;
                if (directGetter(method, machineFields, machine)) changed |= machine.add(method);
                else if (directGetter(method, networkFields, network) || memoizer(method, machineFields, networkFields, machine, network))
                    changed |= network.add(method);
            }
        } while (changed);
        String machineGetter = publicGetter(machine), networkGetter = publicGetter(network);
        if (machineGetter == null || networkGetter == null)
            throw new IOException("Recognized computations have no unambiguous public predicate accessors.");
        Set<Method> selected = new LinkedHashSet<>(machine);
        selected.addAll(network);
        ByteArrayOutputStream buffer = new ByteArrayOutputStream(bytes.length);
        DataOutputStream output = new DataOutputStream(buffer);
        int position = 0;
        for (Method method : image.methods) {
            if (!selected.contains(method)) continue;
            output.write(bytes, position, method.attributeStart - position);
            output.writeShort(method.attributeName);
            output.writeInt(14);
            output.writeShort(1); // max_stack
            output.writeShort(0); // static ()Z
            output.writeInt(2);
            output.writeByte(3); // iconst_0
            output.writeByte(0xac); // ireturn
            output.writeShort(0); // exception table
            output.writeShort(0); // nested Code attributes
            position = method.attributeEnd;
        }
        output.write(bytes, position, bytes.length - position);
        return new Result(buffer.toByteArray(), new Plan(names(machine), names(network), machineGetter, networkGetter));
    }

    private static boolean countryName(String name) {
        return Set.of("country", "countrycode", "country_code", "getcountry", "getcountrycode").contains(name.toLowerCase(Locale.ROOT));
    }
    private static List<String> names(Set<Method> methods) { return methods.stream().map(m -> m.name).sorted().toList(); }
    private static boolean selected(Member reference, Set<Method> methods) {
        return reference != null && reference.owner.equals(TARGET) && reference.descriptor.equals("()Z")
            && methods.stream().anyMatch(m -> m.name.equals(reference.name));
    }
    private static boolean boxing(Instruction instruction) {
        Member ref = instruction.member;
        return instruction.opcode == 184 && ref != null && ref.owner.equals("java/lang/Boolean") && ref.name.equals("valueOf");
    }
    private static boolean unboxing(Instruction instruction) {
        Member ref = instruction.member;
        return instruction.opcode == 182 && ref != null && ref.owner.equals("java/lang/Boolean") && ref.name.equals("booleanValue");
    }
    private static boolean directGetter(Method method, Set<Member> fields, Set<Method> methods) {
        List<Instruction> code = method.instructions.stream().filter(i -> i.opcode != 0).toList();
        if (code.size() < 2 || code.get(code.size() - 1).opcode != 172) return false;
        Instruction first = code.get(0);
        if (code.size() == 2 && first.opcode == 184 && selected(first.member, methods)) return true;
        return first.opcode == 178 && fields.contains(first.member) &&
            (code.size() == 2 && first.member.descriptor.equals("Z") || code.size() == 3 && unboxing(code.get(1)));
    }
    private static boolean memoizer(Method method, Set<Member> machineFields, Set<Member> networkFields,
                                    Set<Method> machine, Set<Method> network) {
        if (networkFields.isEmpty() || method.writes.isEmpty() || !networkFields.containsAll(method.writes)
            || method.calls.stream().noneMatch(r -> selected(r, network))) return false;
        for (Member read : method.reads)
            if (!machineFields.contains(read) && !networkFields.contains(read)) return false;
        for (Member call : method.calls) {
            if (selected(call, machine) || selected(call, network)) continue;
            if (!call.owner.equals("java/lang/Boolean") || !(call.name.equals("valueOf") || call.name.equals("booleanValue"))) return false;
        }
        // Cache wrappers may branch, synchronize and return false on a machine
        // guard; arithmetic, true constants, object creation and dynamic calls
        // would make the meaning ambiguous (e.g. an inverted business check).
        for (Instruction insn : method.instructions) {
            int op = insn.opcode;
            if (op == 0 || op == 3 || op >= 21 && op <= 45 || op >= 54 && op <= 78 || op == 89 ||
                op >= 153 && op <= 167 || op == 172 || op == 178 || op == 179 || op == 182 || op == 184 ||
                op == 191 || op == 194 || op == 195 || op == 198 || op == 199 || op == 200) continue;
            if ((op == 18 || op == 19) && insn.constant == 7) continue; // ldc class for synchronized
            return false;
        }
        return true;
    }
    private static String publicGetter(Set<Method> methods) {
        return methods.stream().filter(m -> (m.flags & 1) != 0).map(m -> m.name).sorted().findFirst().orElse(null);
    }

    private static final class Image {
        final byte[] bytes;
        final int[] tags, first, second;
        final String[] utf8;
        final Map<String, Field> fields = new HashMap<>();
        final List<Method> methods = new ArrayList<>();
        Image(byte[] bytes) throws IOException {
            this.bytes = bytes;
            Reader reader = new Reader(bytes);
            if (reader.u4() != 0xcafebabe) throw new IOException("Invalid class file.");
            reader.skip(4);
            int count = reader.u2();
            tags = new int[count]; first = new int[count]; second = new int[count]; utf8 = new String[count];
            for (int i = 1; i < count; ++i) {
                tags[i] = reader.u1();
                switch (tags[i]) {
                    case 1 -> {
                        int start = reader.position, size = reader.u2(); reader.skip(size);
                        utf8[i] = new DataInputStream(new ByteArrayInputStream(bytes, start, size + 2)).readUTF();
                    }
                    case 3, 4 -> first[i] = reader.u4();
                    case 5, 6 -> { reader.skip(8); ++i; }
                    case 7, 8, 16, 19, 20 -> first[i] = reader.u2();
                    case 9, 10, 11, 12, 17, 18 -> { first[i] = reader.u2(); second[i] = reader.u2(); }
                    case 15 -> { first[i] = reader.u1(); second[i] = reader.u2(); }
                    default -> throw new IOException("Unknown constant-pool tag.");
                }
            }
            reader.skip(2);
            int identity = reader.u2();
            if (!TARGET.equals(className(identity))) throw new IOException("Unexpected class identity.");
            reader.skip(2);
            reader.skip(2 * reader.u2());
            int fieldCount = reader.u2();
            for (int i = 0; i < fieldCount; ++i) {
                int flags = reader.u2(); String name = text(reader.u2()), descriptor = text(reader.u2());
                if (fields.put(name, new Field(flags, descriptor)) != null) throw new IOException("Duplicate field.");
                skipAttributes(reader);
            }
            int methodCount = reader.u2();
            Set<String> identities = new LinkedHashSet<>();
            for (int i = 0; i < methodCount; ++i) {
                Method method = new Method();
                method.flags = reader.u2(); method.name = text(reader.u2()); method.descriptor = text(reader.u2());
                if (!identities.add(method.name + method.descriptor)) throw new IOException("Duplicate method.");
                int attributes = reader.u2();
                for (int j = 0; j < attributes; ++j) {
                    int start = reader.position, name = reader.u2(), size = reader.u4(), body = reader.position;
                    reader.skip(size);
                    if (!text(name).equals("Code")) continue;
                    if (method.attributeStart >= 0) throw new IOException("Duplicate Code attribute.");
                    method.attributeStart = start; method.attributeEnd = reader.position; method.attributeName = name;
                    Reader code = new Reader(bytes); code.position = body; code.skip(4);
                    int length = code.u4(), offset = code.position; code.skip(length);
                    decode(method, offset, length);
                    code.skip(8 * code.u2()); skipAttributes(code);
                    if (code.position != reader.position) throw new IOException("Invalid Code attribute length.");
                }
                methods.add(method);
            }
            skipAttributes(reader);
            if (reader.position != bytes.length) throw new IOException("Trailing class file data.");
        }
        String text(int index) throws IOException {
            if (index <= 0 || index >= tags.length || tags[index] != 1) throw new IOException("Invalid UTF8 constant.");
            return utf8[index];
        }
        String className(int index) throws IOException {
            if (index <= 0 || index >= tags.length || tags[index] != 7) throw new IOException("Invalid class constant.");
            return text(first[index]);
        }
        Member member(int index) throws IOException {
            if (index <= 0 || index >= tags.length || tags[index] < 9 || tags[index] > 11)
                throw new IOException("Invalid member reference.");
            int type = second[index];
            if (type <= 0 || type >= tags.length || tags[type] != 12) throw new IOException("Invalid name-and-type constant.");
            return new Member(className(first[index]), text(first[type]), text(second[type]));
        }
        boolean booleanField(Member reference) {
            if (reference == null || !reference.owner.equals(TARGET)) return false;
            Field field = fields.get(reference.name);
            return field != null && (field.flags & 8) != 0 && field.descriptor.equals(reference.descriptor)
                && (field.descriptor.equals("Z") || field.descriptor.equals("Ljava/lang/Boolean;"));
        }
        void decode(Method method, int offset, int length) throws IOException {
            Reader code = new Reader(bytes);
            int end = offset + length;
            for (code.position = offset; code.position < end;) {
                int start = code.position, op = code.u1(), size = 1, constant = 0;
                Member reference = null;
                if (op > 201) throw new IOException("Invalid bytecode opcode.");
                if (op == 16 || op == 18 || op >= 21 && op <= 25 || op >= 54 && op <= 58 || op == 169 || op == 188) size = 2;
                else if (op == 17 || op == 19 || op == 20 || op == 132 || op >= 153 && op <= 168 ||
                         op >= 178 && op <= 184 || op == 187 || op == 189 || op == 192 || op == 193 || op == 198 || op == 199) size = 3;
                else if (op == 185 || op == 186 || op == 200 || op == 201) size = 5;
                else if (op == 197) size = 4;
                else if (op == 196) {
                    int extended = code.u1();
                    if (!(extended >= 21 && extended <= 25 || extended >= 54 && extended <= 58 || extended == 169 || extended == 132))
                        throw new IOException("Invalid wide instruction.");
                    size = extended == 132 ? 6 : 4;
                } else if (op == 170 || op == 171) {
                    code.position = offset + ((start - offset + 4) & ~3);
                    code.skip(4);
                    if (op == 170) {
                        int low = code.u4(), high = code.u4();
                        long entries = (long) high - low + 1;
                        if (entries < 0 || entries > (end - code.position) / 4) throw new IOException("Invalid tableswitch.");
                        code.skip((int) entries * 4);
                    } else {
                        int entries = code.u4();
                        if (entries < 0 || entries > (end - code.position) / 8) throw new IOException("Invalid lookupswitch.");
                        code.skip(entries * 8);
                    }
                    size = code.position - start;
                }
                if (size > end - start) throw new IOException("Truncated bytecode.");
                code.position = start + 1;
                if (op >= 178 && op <= 185) {
                    reference = member(code.u2());
                    if (op == 178 || op == 180) method.reads.add(reference);
                    else if (op == 179 || op == 181) method.writes.add(reference);
                    else method.calls.add(reference);
                } else if (op == 18 || op == 19 || op == 20) {
                    int index = op == 18 ? code.u1() : code.u2();
                    if (index <= 0 || index >= tags.length) throw new IOException("Invalid ldc constant.");
                    constant = tags[index];
                    if (constant == 8) method.strings.add(text(first[index]));
                }
                method.instructions.add(new Instruction(op, reference, constant));
                code.position = start + size;
            }
        }
    }
    private static void skipAttributes(Reader reader) throws IOException {
        int count = reader.u2();
        for (int i = 0; i < count; ++i) { reader.skip(2); reader.skip(reader.u4()); }
    }
    private static final class Reader {
        final byte[] bytes;
        int position;
        Reader(byte[] bytes) { this.bytes = bytes; }
        void skip(int size) throws IOException {
            if (size < 0 || size > bytes.length - position) throw new IOException("Truncated class file.");
            position += size;
        }
        int u1() throws IOException { int start = position; skip(1); return bytes[start] & 255; }
        int u2() throws IOException { return u1() << 8 | u1(); }
        int u4() throws IOException { return u2() << 16 | u2(); }
    }
}

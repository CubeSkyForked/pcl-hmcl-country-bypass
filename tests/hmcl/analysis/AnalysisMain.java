import java.lang.reflect.Method;
import org.jackhuang.hmcl.util.io.NetworkUtils;

public final class AnalysisMain {
    private static boolean call(Class<?> type, String name) throws Exception {
        return (boolean) type.getMethod(name).invoke(null);
    }
    public static void main(String[] args) throws Exception {
        Class<?> type = Class.forName("org.jackhuang.hmcl.util.RegionUtils");
        boolean late = args.length != 0 && args[0].equals("late");
        if (late) {
            if (!call(type, "machine") || !call(type, "network")) throw new AssertionError("Late baseline must be restricted.");
            System.out.println("READY"); System.out.flush();
            if (System.in.read() < 0) throw new AssertionError("Attach was not completed.");
        }
        if (call(type, "machine") || call(type, "network") || call(type, "networkAlias"))
            throw new AssertionError("Analyzed regional predicates were not converted.");
        if (!call(type, "unrelated") || !call(type, "inverse"))
            throw new AssertionError("Unrelated or inverted predicate was replaced.");
        Method switchValue = type.getMethod("switchValue", int.class);
        if (!switchValue.invoke(null, 1).equals(11) || !switchValue.invoke(null, -7).equals(99))
            throw new AssertionError("Unrelated switch method changed.");
        if (NetworkUtils.calls != (late ? 1 : 0)) throw new AssertionError("Unexpected network predicate side effect.");
        if (!"transformed".equals(System.getProperty("hkregion.hmcl.agent.state")))
            throw new AssertionError("Analysis was not exercised.");
        String methods = System.getProperty("hkregion.hmcl.agent.methods", "");
        if (!methods.contains("networkAlias") || methods.contains("unrelated") || methods.contains("inverse"))
            throw new AssertionError("Unexpected selection: " + methods);
        System.out.println("PASS: inferred roots, renamed caches, delegate and preserved business methods; late=" + late);
    }
}

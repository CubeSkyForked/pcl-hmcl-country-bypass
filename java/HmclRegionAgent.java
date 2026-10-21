package hkregion.hmcl;

import java.io.IOException;
import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.Instrumentation;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.ProtectionDomain;
import java.time.Instant;
import java.time.ZoneId;
import java.util.Base64;
import java.util.HexFormat;
import java.util.Locale;

/** Analyzes RegionUtils at load time; no HMCL classes are redistributed. */
public final class HmclRegionAgent {
    private static final String TARGET = "org/jackhuang/hmcl/util/RegionUtils";
    private static Path logFile;
    private static boolean armed;
    private record Observation(ClassLoader loader, HmclClassAnalyzer.Plan plan) {}
    private static volatile Observation observation;

    public static void premain(String options, Instrumentation instrumentation) {
        install(options, instrumentation, "premain");
    }

    public static void agentmain(String options, Instrumentation instrumentation) {
        install(options, instrumentation, "agentmain");
    }

    private static synchronized void install(String options, Instrumentation instrumentation, String mode) {
        try {
            if (armed) { log("Already armed."); return; }
            String logOption = options != null && options.startsWith("log64:")
                    ? new String(Base64.getDecoder().decode(options.substring(6)), StandardCharsets.UTF_8)
                    : options;
            logFile = logOption == null || logOption.isBlank()
                    ? Path.of(HmclRegionAgent.class.getProtectionDomain().getCodeSource().getLocation().toURI())
                        .getParent().resolve("HKRegion-hmcl-agent.log")
                    : Path.of(logOption).toAbsolutePath();
            if (logFile.getParent() != null) Files.createDirectories(logFile.getParent());
            log("START: mode=" + mode + "; pid=" + ProcessHandle.current().pid() + "; " + signals());
            if (!instrumentation.isRetransformClassesSupported())
                throw new UnsupportedOperationException("This JVM does not support class retransformation.");
            Transformer transformer = new Transformer();
            instrumentation.addTransformer(transformer, true);
            armed = true;
            System.setProperty("hkregion.hmcl.agent.state", "armed");
            for (Class<?> type : instrumentation.getAllLoadedClasses()) {
                if (type.getName().equals(TARGET.replace('/', '.'))) {
                    if (!instrumentation.isModifiableClass(type))
                        throw new UnsupportedOperationException("RegionUtils is not modifiable.");
                    instrumentation.retransformClasses(type);
                }
            }
            Thread observer = new Thread(() -> observe(instrumentation), "HKRegion-HMCL-observer");
            observer.setDaemon(true);
            observer.start();
        } catch (Exception error) {
            System.setProperty("hkregion.hmcl.agent.state", "failed");
            log("INSTALL FAILED: " + error);
        }
    }

    private static void observe(Instrumentation instrumentation) {
        try {
            for (int attempt = 0; attempt < 150; ++attempt) {
                Observation observed = observation;
                if (observed != null) {
                    HmclClassAnalyzer.Plan selected = observed.plan();
                    for (Class<?> type : instrumentation.getAllLoadedClasses()) {
                        if (type.getName().equals(TARGET.replace('/', '.')) && type.getClassLoader() == observed.loader()) {
                            log("VERIFIED: machine=" + type.getMethod(selected.machineGetter()).invoke(null)
                                    + "; network=" + type.getMethod(selected.networkGetter()).invoke(null)
                                    + "; " + signals());
                            return;
                        }
                    }
                }
                Thread.sleep(200);
            }
            log("No supported RegionUtils was observed within 30 seconds.");
        } catch (Exception error) { log("VERIFY FAILED: " + error); }
    }

    private static String signals() {
        return "timezone=" + ZoneId.systemDefault().getId() + "; locale=" + Locale.getDefault().toLanguageTag();
    }

    private static synchronized void log(String message) {
        if (logFile == null) return;
        try {
            Files.writeString(logFile, Instant.now() + " " + message + System.lineSeparator(),
                    StandardCharsets.UTF_8, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException ignored) { }
    }

    private static String sha(byte[] bytes) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }

    private static final class Transformer implements ClassFileTransformer {
        @Override
        public byte[] transform(ClassLoader loader, String name, Class<?> redefined,
                                ProtectionDomain domain, byte[] bytes) {
            if (!TARGET.equals(name)) return null;
            try {
                HmclClassAnalyzer.Result result = HmclClassAnalyzer.transform(bytes);
                HmclClassAnalyzer.Plan selected = result.plan();
                String source = domain == null || domain.getCodeSource() == null ? "unknown"
                        : domain.getCodeSource().getLocation().toString();
                log("ANALYZED: source=" + source + "; machine=" + selected.machine() + "; network=" + selected.network());
                observation = new Observation(loader, selected);
                System.setProperty("hkregion.hmcl.agent.state", "transformed");
                System.setProperty("hkregion.hmcl.agent.methods", String.join(",", selected.machine()) + "," + String.join(",", selected.network()));
                log("TRANSFORMED: methods=" + (selected.machine().size() + selected.network().size())
                        + "; retransform=" + (redefined != null) + "; inputSha256=" + sha(bytes));
                return result.bytes();
            } catch (Exception error) {
                System.setProperty("hkregion.hmcl.agent.state", "rejected");
                log("REJECTED: " + error);
                return null;
            }
        }
    }
}

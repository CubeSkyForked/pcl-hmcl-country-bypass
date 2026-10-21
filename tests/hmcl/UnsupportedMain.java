import org.jackhuang.hmcl.util.RegionUtils;

public final class UnsupportedMain {
    public static void main(String[] args) {
        if (!RegionUtils.isIsChinaMainlandMachine() || !RegionUtils.isIsChinaMainlandNetwork())
            throw new AssertionError("An unsupported application's class was modified.");
        if (!"rejected".equals(System.getProperty("hkregion.hmcl.agent.state")))
            throw new AssertionError("The class structure rejection was not exercised.");
        System.out.println("PASS: unsupported archive with the same class name is unchanged.");
    }
}

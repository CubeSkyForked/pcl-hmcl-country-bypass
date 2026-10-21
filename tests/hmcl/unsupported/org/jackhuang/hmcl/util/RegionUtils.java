package org.jackhuang.hmcl.util;

// An unrelated application with the same class name must remain unchanged.
public final class RegionUtils {
    private static boolean checkMachineIsChinaMainland() { return true; }
    private static boolean checkChinaMainlandNetwork() { return true; }
    public static boolean isIsChinaMainlandMachine() { return true; }
    public static boolean isIsChinaMainlandNetwork() { return true; }
    // Constants elsewhere in a class are insufficient: instructions must support the predicates.
    public static String decoy() { return "Asia/Shanghai CN https://ipwho.is"; }
}

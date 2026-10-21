package org.jackhuang.hmcl.util;

import java.time.ZoneId;
import java.util.Locale;
import org.jackhuang.hmcl.util.io.NetworkUtils;

public final class RegionUtils {
    private static final boolean MACHINE = calculateMachine();
    private static volatile Boolean cachedNetwork;
    private static boolean calculateMachine() {
        return "Asia/Shanghai".equals(ZoneId.systemDefault().getId()) || "CN".equals(Locale.getDefault().getCountry());
    }
    private static boolean calculateNetwork() {
        return "CN".equals(NetworkUtils.doGet("https://ipwho.is"));
    }
    public static boolean machine() { return MACHINE; }
    public static synchronized boolean network() {
        if (!MACHINE) return false;
        if (cachedNetwork == null) cachedNetwork = calculateNetwork();
        return cachedNetwork;
    }
    public static boolean networkAlias() { return network(); }
    public static boolean unrelated() { return true; }
    public static boolean inverse() { return !machine(); }
    public static int switchValue(int value) {
        return switch (value) { case 1 -> 11; case 2 -> 22; case 3 -> 33; default -> 99; };
    }
}

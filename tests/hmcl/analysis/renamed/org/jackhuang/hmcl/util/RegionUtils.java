package org.jackhuang.hmcl.util;

import java.time.ZoneId;
import java.util.Locale;
import java.util.Set;
import org.jackhuang.hmcl.util.io.NetworkUtils;

public final class RegionUtils {
    private static final boolean localFlag = hostAreaV2();
    private static volatile Boolean linkFlag;
    private record Response(String countryCode) {}
    private static boolean hostAreaV2() {
        String zone = ZoneId.systemDefault().getId();
        return zone.equals("Asia/Shanghai") || "CN".equals(Locale.getDefault().getCountry());
    }
    private static boolean remoteAreaV2() {
        // Different endpoint and extra timezone exclusions from a newer layout.
        if (Set.of("Asia/Hong_Kong", "Asia/Taipei").contains(ZoneId.systemDefault().getId())) return false;
        Response response = new Response(NetworkUtils.doGet("https://geo.invalid/current"));
        return "CN".equalsIgnoreCase(response.countryCode());
    }
    public static boolean machine() { return localFlag; }
    public static boolean network() {
        if (!localFlag) return false;
        Boolean value = linkFlag;
        if (value != null) return value;
        synchronized (RegionUtils.class) {
            value = linkFlag;
            if (value != null) return value;
            boolean result = remoteAreaV2();
            linkFlag = result;
            return result;
        }
    }
    public static boolean networkAlias() { return network(); }
    public static boolean unrelated() { return true; }
    public static boolean inverse() { return !machine(); }
    public static int switchValue(int value) {
        return switch (value) { case 1 -> 11; case 37 -> 22; case 999 -> 33; default -> 99; };
    }
}

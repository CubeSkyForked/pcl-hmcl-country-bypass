package org.jackhuang.hmcl.util;

import java.time.ZoneId;
import org.jackhuang.hmcl.util.io.NetworkUtils;

public final class RegionUtils {
    private static boolean firstArea() { return "Asia/Shanghai".equals(ZoneId.systemDefault().getId()) && "CN".equals("CN"); }
    private static boolean secondArea() { return "CN".equals("CN") && "Asia/Shanghai".equals(ZoneId.systemDefault().getId()); }
    private static boolean remoteArea() { return "CN".equals(NetworkUtils.doGet("https://ipwho.is")); }
    public static boolean isIsChinaMainlandMachine() { return firstArea() || secondArea(); }
    public static boolean isIsChinaMainlandNetwork() { return remoteArea(); }
}

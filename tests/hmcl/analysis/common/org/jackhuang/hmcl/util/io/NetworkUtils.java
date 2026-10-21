package org.jackhuang.hmcl.util.io;

// Offline fixture: no network is contacted. A transformed predicate must skip it.
public final class NetworkUtils {
    public static int calls;
    public static String doGet(String endpoint) { calls++; return "CN"; }
}

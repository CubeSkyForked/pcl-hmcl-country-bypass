// Compile with Framework csc /platform:x86 or /platform:x64; run each DLL in a fresh process.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

class ProxyArchitectureProbe {
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
    static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern int GetUserDefaultGeoName(StringBuilder value, int count);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate uint Wait(uint timeout);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate uint Status();
    [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode)]
    delegate uint VersionSize(string file, out uint handle);
    [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode)]
    delegate IntPtr HttpOpen(string agent, uint access, string proxy, string bypass, uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    delegate bool HttpTimeouts(IntPtr session, int resolve, int connect, int send, int receive);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate bool HttpClose(IntPtr session);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate uint Timer();
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate uint TimerCaps(IntPtr caps, uint size);
    [UnmanagedFunctionPointer(CallingConvention.Winapi, CharSet = CharSet.Unicode, SetLastError = true)]
    delegate void Extract(IntPtr window, IntPtr instance, StringBuilder command, int show);

    static T Bind<T>(IntPtr module, string name) where T : class {
        IntPtr address = GetProcAddress(module, name);
        if (address == IntPtr.Zero) throw new Exception("Missing export: " + name);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
    }
    static void Check(bool result, string name) {
        if (!result) throw new Exception(name);
        Console.WriteLine("PASS " + name);
    }
    static int Main(string[] args) {
        try {
            string path = Path.GetFullPath(args[0]);
            string profile = Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
            Console.WriteLine("Process pointer bits: " + (IntPtr.Size * 8));
            IntPtr proxy = LoadLibraryW(path);
            Check(proxy != IntPtr.Zero, "Load proxy in matching process architecture");
            Check(Bind<Wait>(proxy, "HKRegion_WaitForInitialization")(15000) == 0, "Native hooks initialized");
            StringBuilder geo = new StringBuilder(16);
            Check(GetUserDefaultGeoName(geo, geo.Capacity) == 3 && geo.ToString() == "HK", "Real GeoName API overridden");
            Status status = Bind<Status>(proxy, "HKRegion_GetManagedPatchStatus");
            for (int n = 0; n < 200 && status() == 997; n++) Thread.Sleep(10);
            Check(status() == 50, "Unrelated executable is rejected by managed restorer");
            IntPtr system = LoadLibraryW(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), profile + ".dll"));
            Check(system != IntPtr.Zero && system != proxy, "Separate matching system DLL loaded");
            if (profile == "version") {
                VersionSize forwarded = Bind<VersionSize>(proxy, "GetFileVersionInfoSizeW");
                VersionSize original = Bind<VersionSize>(system, "GetFileVersionInfoSizeW");
                string target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "kernel32.dll");
                uint ignored;
                uint expected = original(target, out ignored);
                Check(expected > 0, "Original version resource readable");
                for (int i = 0; i < 1000; i++) {
                    if (forwarded(target, out ignored) != expected) throw new Exception("Version forwarding or stack imbalance");
                }
                Check(true, "1000 stdcall version forwards preserve arguments and stack");
            } else if (profile == "winhttp") {
                IntPtr session = Bind<HttpOpen>(proxy, "WinHttpOpen")("HKRegion architecture probe", 1, null, null, 0);
                Check(session != IntPtr.Zero, "WinHttpOpen returns a valid session");
                Check(Bind<HttpTimeouts>(proxy, "WinHttpSetTimeouts")(session, 1000, 2000, 3000, 4000), "Five-argument WinHTTP forwarding works");
                Check(Bind<HttpClose>(proxy, "WinHttpCloseHandle")(session), "WinHTTP handle closed");
            } else if (profile == "winmm") {
                Timer forwarded = Bind<Timer>(proxy, "timeGetTime");
                Timer original = Bind<Timer>(system, "timeGetTime");
                Check(unchecked(forwarded() - original() + 100) <= 200, "WinMM timer forwarding works");
                IntPtr caps = Marshal.AllocHGlobal(8);
                try {
                    Check(Bind<TimerCaps>(proxy, "timeGetDevCaps")(caps, 8) == 0 && Marshal.ReadInt32(caps) > 0,
                        "WinMM output pointer forwarding works");
                } finally { Marshal.FreeHGlobal(caps); }
            } else throw new Exception("Unknown proxy profile");
            string directory = Path.Combine(Path.GetFullPath(args[1]), profile + "-" + Guid.NewGuid().ToString("N") + "-\u4e2d\u6587 \u7a7a\u683c");
            Directory.CreateDirectory(directory);
            string request = Guid.NewGuid().ToString();
            Extract extract = Bind<Extract>(proxy, "HKRegion_ExtractHmclAgentW");
            extract(IntPtr.Zero, IntPtr.Zero, new StringBuilder("\"" + directory + "\" " + request), 0);
            Check(Marshal.GetLastWin32Error() == 0, "Four-argument Unicode extraction callback succeeds");
            string manifest = File.ReadAllText(Path.Combine(directory, "hmcl-agent.json"));
            Check(manifest.Contains(request), "Fresh extraction request ID returned");
            string jar = Directory.GetFiles(directory, "*.jar")[0];
            using (SHA256 sha = SHA256.Create()) {
                string hash = BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(jar))).Replace("-", "").ToLowerInvariant();
                string expected = Regex.Match(manifest, "\"sha256\":\"([0-9a-f]{64})\"").Groups[1].Value;
                Check(hash == expected && hash.Length == 64, "Complete embedded agent matches published SHA256");
            }
            Console.WriteLine("ALL PASSED " + profile);
            return 0;
        } catch (Exception e) {
            Console.Error.WriteLine(e);
            return 1;
        }
    }
}

using System;
using System.IO;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

namespace HKRegion.Managed
{
    // Only the examined 2.99 image is supported. No JIT addresses are patched.
    public static class PclDownloadRestore
    {
        private const BindingFlags All = BindingFlags.Public | BindingFlags.NonPublic |
            BindingFlags.Static | BindingFlags.Instance;
        private static readonly Guid TargetMvid = new Guid("926d447e-daff-4169-8c64-adc3d37d5f78");
        private static readonly byte[] ExpectedIl = { 0x72, 0xeb, 0x45, 0x00, 0x70,
            0x73, 0xa6, 0x01, 0x00, 0x0a, 0x7a };
        private static readonly object LogLock = new object();
        private static string logPath;
        private static bool installed;
        private static object sourceLoader;
        private static FieldInfo inputField, outputField;
        private static MethodInfo waitMethod, cancellationMethod;
        private static FieldInfo forceField;

        public static int Run(string argument)
        {
            try
            {
                Assembly target = Assembly.GetEntryAssembly();
                if (target == null) return 997;
                if (target.ManifestModule.ModuleVersionId != TargetMvid) return 50;
                Application app = Application.Current;
                if (app == null || app.Dispatcher.HasShutdownStarted) return 997;
                logPath = argument;
                int result = 0;
                app.Dispatcher.Invoke(DispatcherPriority.Send, new Action(delegate
                {
                    try { Install(target); }
                    catch (Exception error) { Log("INSTALL FAILED: " + error); result = 13; }
                }));
                return result;
            }
            catch (Exception error)
            {
                Log("HOST FAILED: " + error);
                return 13;
            }
        }

        private static void Install(Assembly target)
        {
            if (installed) return;
            Type closure = target.GetType("PCL.PageDownloadInstall+_Closure$__", true);
            MethodInfo blocked = (MethodInfo)target.ManifestModule.ResolveMethod(0x06001468);
            byte[] actual = blocked.GetMethodBody().GetILAsByteArray();
            if (blocked.DeclaringType != closure || blocked.Name != "_Lambda$__3-0" ||
                actual.Length != ExpectedIl.Length)
                throw new InvalidOperationException("Unexpected disable callback.");
            for (int i = 0; i < actual.Length; ++i)
                if (actual[i] != ExpectedIl[i]) throw new InvalidOperationException("IL mismatch.");

            FieldInfo cache = Required(closure.GetField("$IR3-3", All));
            Type download = target.GetType("PCL.ModDownload", true);
            object source = Required(download.GetField("DlClientListLoader", All)).GetValue(null);
            if (source == null) throw new InvalidOperationException("Missing original loader.");
            Type taskType = source.GetType();
            FieldInfo taskDelegate = Required(taskType.GetField("LoadDelegate", All));
            if (cache.FieldType != taskDelegate.FieldType)
                throw new InvalidOperationException("Unexpected callback signature.");
            FieldInfo input = Required(taskType.GetField("Input", All));
            FieldInfo output = Required(taskType.GetField("Output", All));
            MethodInfo wait = Required(taskType.GetMethod("WaitForExit", All));
            MethodInfo cancel = Required(taskType.GetMethod("CreateCancellationToken", All));
            FieldInfo force = Required(taskType.GetField("IsForceRestarting", All));
            if (wait.GetParameters().Length != 5 || cancel.ReturnType != typeof(CancellationToken))
                throw new InvalidOperationException("Unexpected loader API.");
            Delegate replacement = Delegate.CreateDelegate(cache.FieldType,
                typeof(PclDownloadRestore).GetMethod("Forward", BindingFlags.NonPublic | BindingFlags.Static));

            object page = Required(target.GetType("PCL.ModMain", true)
                .GetField("FrmDownloadInstall", All)).GetValue(null);
            object existing = page == null ? null : Required(target.GetType("PCL.MyPageRight", true)
                .GetField("PageLoader", All)).GetValue(page);
            bool replaceExisting = false;
            if (existing != null && taskType.IsInstanceOfType(existing))
            {
                Delegate previous = taskDelegate.GetValue(existing) as Delegate;
                replaceExisting = previous != null && previous.Method.DeclaringType == closure &&
                    previous.Method.Name == "_Lambda$__R3-3";
            }

            sourceLoader = source;
            inputField = input;
            outputField = output;
            waitMethod = wait;
            cancellationMethod = cancel;
            forceField = force;
            cache.SetValue(null, replacement);
            if (replaceExisting) taskDelegate.SetValue(existing, replacement);
            installed = true;
            Log("INSTALLED: MVID=" + TargetMvid + "; IL verified; cached callback replaced; existing=" + replaceExisting);

            // An already failed page keeps its own delegate, independently of the cache.
            if (replaceExisting && Required(taskType.GetProperty("State", All)).GetValue(existing, null).ToString() == "Failed")
            {
                Required(page.GetType().GetMethod("PageLoaderRestart", All))
                    .Invoke(page, new object[] { null, true });
                Log("Restarted the existing failed download page.");
            }
        }

        private static void Forward(object task)
        {
            if (ReferenceEquals(task, sourceLoader))
                throw new InvalidOperationException("Recursive original loader.");
            Log("CALLBACK: forwarding to original DlClientListLoader.");
            try
            {
                CancellationToken token = (CancellationToken)cancellationMethod.Invoke(task, null);
                waitMethod.Invoke(sourceLoader, new object[] { inputField.GetValue(task), task,
                    forceField.GetValue(task), token, null });
                object output = outputField.GetValue(sourceLoader);
                outputField.SetValue(task, output);
                object value = Required(output.GetType().GetField("Value", All)).GetValue(output);
                if (value == null) throw new InvalidOperationException("Original loader returned no version list.");
                object versions = Required(value.GetType().GetProperty("Item", new Type[] { typeof(string) }))
                    .GetValue(value, new object[] { "versions" });
                object count = Required(versions.GetType().GetProperty("Count")).GetValue(versions, null);
                Log("COMPLETED: original version list; versions=" + count + "; source=" +
                    Required(output.GetType().GetField("SourceName", All)).GetValue(output));
            }
            catch (TargetInvocationException error)
            {
                Log("DOWNLOAD FAILED: " + error.InnerException);
                ExceptionDispatchInfo.Capture(error.InnerException ?? error).Throw();
                throw;
            }
        }

        private static T Required<T>(T member) where T : class
        {
            if (member == null) throw new MissingMemberException("The examined PCL member is absent.");
            return member;
        }

        private static void Log(string message)
        {
            if (String.IsNullOrEmpty(logPath)) return;
            try
            {
                lock (LogLock)
                    File.AppendAllText(logPath, DateTime.UtcNow.ToString("o") + " " + message + Environment.NewLine);
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}

#ifndef HK_REGION_VERSION_PROXY_H
#define HK_REGION_VERSION_PROXY_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#define HK_REGION_PROXY_NOEXCEPT noexcept
#else
#define HK_REGION_PROXY_NOEXCEPT
#endif

/*
 * State of the automatic initialization worker in the renameable proxy:
 *   ERROR_IO_PENDING (997): queued or running;
 *   ERROR_SUCCESS (0): the selected system DLL was loaded and hooks installed;
 *   any other value: a Win32 initialization error.
 * A separate call to HKRegion_Install does not update this worker status.
 * The query never waits and preserves the caller's LastError value.
 */
DWORD WINAPI HKRegion_GetInitializationStatus(void) HK_REGION_PROXY_NOEXCEPT;

/*
 * Wait for that worker from a normal application thread only. Never call this
 * from DllMain, a DLL/TLS initialization callback, or while holding loader lock.
 * Returns the worker result (0 on success), WAIT_TIMEOUT (258) on timeout, or a
 * Win32 wait/initialization error. INFINITE is accepted; zero polls readiness.
 * This function preserves the caller's LastError value.
 */
DWORD WINAPI HKRegion_WaitForInitialization(DWORD timeoutMs) HK_REGION_PROXY_NOEXCEPT;

typedef DWORD (WINAPI *HKRegionGetInitializationStatusFunction)(void);
typedef DWORD (WINAPI *HKRegionWaitForInitializationFunction)(DWORD timeoutMs);

/* Optional PCL 2.99 managed callback restoration. Independent from the native
 * hook status: 997 pending, 0 installed, 50 unsupported executable, otherwise
 * a Win32 error or CLR HRESULT. Preserves LastError and never waits. */
DWORD WINAPI HKRegion_GetManagedPatchStatus(void) HK_REGION_PROXY_NOEXCEPT;

/*
 * The loaded filename selects the system version.dll, winhttp.dll or winmm.dll.
 * A generated export map and assembly tail jumps retain native argument/return
 * conventions. Native ordinals dispatch against the selected system library;
 * version's 17 names retain ordinals 1..17. Diagnostic exports use 4096..4099;
 * 4100 is HKRegion_ExtractHmclAgentW, a rundll32 Unicode callback (four args).
 * Only call the selected library's APIs. An API absent from the system library
 * raises a noncontinuable resolution error, since a generic thunk cannot invent
 * a correctly typed failure return. Optional missing APIs do not block hooks.
 *
 * Initialization is asynchronous: loading this DLL is not a guarantee that
 * hooks are installed before the application's first region query. Ordinary
 * native forwarding does not wait for hook installation. A first forwarded
 * call from another DLL's initialization can still perform a lazy system DLL
 * load under that caller's loader lock; no private resolver lock is held.
 * The proxy, completion event, and retained real-DLL reference live until exit.
 */

#ifdef __cplusplus
}
#endif

#undef HK_REGION_PROXY_NOEXCEPT

#endif

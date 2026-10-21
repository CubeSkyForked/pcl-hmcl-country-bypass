#include "hk_region_hook.h"
#include "version_proxy.h"
#include "proxy_exports.h"
#include "pcl_restore.h"
#include "hmcl_agent.h"

#include <cstring>
#include <cwchar>

namespace {

HMODULE g_proxy = nullptr;
// DllMain is called for every new thread, including threads with small stacks.
// A large local path buffer would reserve stack even before its reason check.
WCHAR g_proxyPath[32768]{};
PVOID volatile g_system = nullptr;
const WCHAR* g_systemName = L"version.dll";
PVOID volatile g_targets[kForwardCount]{};
HANDLE g_completion = nullptr;
volatile LONG g_status = ERROR_IO_PENDING;

DWORD LastErrorOr(DWORD fallback) noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? fallback : error;
}

DWORD Status() noexcept {
    return static_cast<DWORD>(InterlockedCompareExchange(&g_status, 0, 0));
}

void Complete(DWORD error) noexcept {
    InterlockedExchange(&g_status, static_cast<LONG>(error));
    if (g_completion) SetEvent(g_completion);
}

HMODULE SystemModule(DWORD& error) noexcept {
    auto module = static_cast<HMODULE>(InterlockedCompareExchangePointer(&g_system, nullptr, nullptr));
    if (module) return module;
    WCHAR path[MAX_PATH + 32]{};
    constexpr auto capacity = sizeof(path) / sizeof(path[0]);
    const UINT length = GetSystemDirectoryW(path, static_cast<UINT>(capacity));
    if (!length || length >= capacity) {
        error = LastErrorOr(ERROR_FILENAME_EXCED_RANGE);
        return nullptr;
    }
    const auto suffixLength = std::wcslen(g_systemName);
    if (length + 1 + suffixLength + 1 > capacity) {
        error = ERROR_FILENAME_EXCED_RANGE;
        return nullptr;
    }
    path[length] = L'\\';
    std::memcpy(path + length + 1, g_systemName, (suffixLength + 1) * sizeof(WCHAR));
    module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        error = LastErrorOr(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    if (module == g_proxy) {
        FreeLibrary(module);
        error = ERROR_INVALID_DLL;
        return nullptr;
    }
    const auto previous = static_cast<HMODULE>(InterlockedCompareExchangePointer(&g_system, module, nullptr));
    if (previous) {
        FreeLibrary(module);
        return previous;
    }
    return module;
}

PVOID Resolve(unsigned index, DWORD& error) noexcept {
    PVOID address = InterlockedCompareExchangePointer(&g_targets[index], nullptr, nullptr);
    if (address) return address;
    const HMODULE module = SystemModule(error);
    if (!module) return nullptr;
    const auto& spec = kForwardSpecs[index];
    const FARPROC function = GetProcAddress(module, spec.name ? spec.name : MAKEINTRESOURCEA(spec.ordinal));
    if (!function) {
        error = LastErrorOr(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    static_assert(sizeof(address) == sizeof(function), "Windows function pointer size");
    std::memcpy(&address, &function, sizeof(address));
    const PVOID previous = InterlockedCompareExchangePointer(&g_targets[index], address, nullptr);
    return previous ? previous : address;
}

DWORD WINAPI Initialize(LPVOID) noexcept {
    DWORD error = ERROR_SUCCESS;
    if (!SystemModule(error)) {
        Complete(error);
        return error;
    }
    // Newer optional WinHTTP/WinMM exports may not exist on older Windows.
    // Their absence must not prevent initialization of the regional overrides.
    for (unsigned index = 0; index < kForwardCount; ++index) {
        DWORD optionalError = ERROR_SUCCESS;
        Resolve(index, optionalError);
    }
    error = HKRegion_Install(nullptr);
    Complete(error);
    // The optional, fingerprinted .NET Framework restoration has its own
    // status. Native forwarding and regional hooks never wait for the CLR.
    HKR_InitializeManagedRestore(g_proxy);
    return error;
}

} // namespace

// The assembly thunks retain argument registers, stack arguments and the
// original return address, then tail-jump using the system function's own ABI.
extern "C" PVOID __cdecl HKR_ResolveForwarder(unsigned index) noexcept {
    const DWORD entryError = GetLastError();
    DWORD error = ERROR_PROC_NOT_FOUND;
    const PVOID result = index < kForwardCount ? Resolve(index, error) : nullptr;
    if (!result) {
        // A generic thunk cannot invent a correctly typed failure return.
        // Fail explicitly if an API absent from this Windows DLL is invoked.
        SetLastError(error);
        RaiseException(error, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        TerminateProcess(GetCurrentProcess(), error);
        return nullptr;
    }
    SetLastError(entryError);
    return result;
}

extern "C" DWORD WINAPI HKRegion_GetInitializationStatus(void) noexcept {
    const DWORD entryError = GetLastError();
    const DWORD result = Status();
    SetLastError(entryError);
    return result;
}

extern "C" DWORD WINAPI HKRegion_WaitForInitialization(DWORD timeoutMs) noexcept {
    const DWORD entryError = GetLastError();
    DWORD result = Status();
    if (result == ERROR_IO_PENDING) {
        if (!g_completion) result = ERROR_INVALID_HANDLE;
        else {
            const DWORD wait = WaitForSingleObject(g_completion, timeoutMs);
            result = wait == WAIT_OBJECT_0 ? Status()
                : wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : LastErrorOr(ERROR_GEN_FAILURE);
        }
    }
    SetLastError(entryError);
    return result;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) noexcept {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_proxy = instance;
    const DWORD capacity = static_cast<DWORD>(sizeof(g_proxyPath) / sizeof(g_proxyPath[0]));
    const DWORD length = GetModuleFileNameW(instance, g_proxyPath, capacity);
    if (!length || length >= capacity) {
        Complete(LastErrorOr(ERROR_FILENAME_EXCED_RANGE));
        return TRUE;
    }
    const WCHAR* name = std::wcsrchr(g_proxyPath, L'\\');
    name = name ? name + 1 : g_proxyPath;
    if (_wcsicmp(name, L"winhttp.dll") == 0) g_systemName = L"winhttp.dll";
    else if (_wcsicmp(name, L"winmm.dll") == 0) g_systemName = L"winmm.dll";
    // Other names retain the original explicitly-loaded version profile.

    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(instance), &pinned)) {
        Complete(LastErrorOr(ERROR_DLL_INIT_FAILED));
        return TRUE;
    }
    // The known HMCL wrapper's import is armed before its main thread can
    // create Java. All extraction and command-line work happens on that call.
    HKR_ArmHmclLauncher();
    // No real-DLL load, MinHook call, wait or private lock under loader lock.
    g_completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_completion) {
        Complete(LastErrorOr(ERROR_NOT_ENOUGH_MEMORY));
        return TRUE;
    }
    const HANDLE worker = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    if (!worker) Complete(LastErrorOr(ERROR_NOT_ENOUGH_MEMORY));
    else CloseHandle(worker);
    return TRUE;
}

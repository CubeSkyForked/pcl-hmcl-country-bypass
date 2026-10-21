#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef HK_REGION_HOOK_BUILD_DLL
#define HK_REGION_HOOK_BUILD_DLL
#endif

#include "hk_region_hook.h"
#include "MinHook.h"

#include <cstddef>
#include <cstring>
#include <cwchar>

#if !defined(_M_IX86) && !defined(_M_X64) && !defined(__i386__) && !defined(__x86_64__)
#error This MinHook build supports x86 and x64 processes only.
#endif

namespace {

constexpr DWORD kHongKongGeoId = 104;
constexpr DWORD kDwordBytes = sizeof(DWORD);
constexpr ULONG kKeyNameInformation = 3;
constexpr DWORD kWow64ViewFlags = RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY;
constexpr WCHAR kHongKongLocale[] = L"zh-HK";
// Each MULTI_SZ contains one string terminator plus the final list terminator.
constexpr WCHAR kHongKongUiNames[] = L"zh-HK\0";
constexpr WCHAR kHongKongUiIds[] = L"0C04\0";

constexpr wchar_t kDeviceKey[] =
    L"\\REGISTRY\\MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
    L"\\Control Panel\\DeviceRegion";
constexpr wchar_t kDeviceKeyWow32[] =
    L"\\REGISTRY\\MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion"
    L"\\Control Panel\\DeviceRegion";
constexpr char kDeviceValue[] = "DeviceRegion";

using GeoNameFunction = int (WINAPI *)(LPWSTR, int);
using LocaleNameFunction = int (WINAPI *)(LPWSTR, int);
using PreferredUiLanguagesFunction = BOOL (WINAPI *)(DWORD, PULONG, PZZWSTR, PULONG);
using QueryValueWFunction = LSTATUS (WINAPI *)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using QueryValueAFunction = LSTATUS (WINAPI *)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using GetValueWFunction = LSTATUS (WINAPI *)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using GetValueAFunction = LSTATUS (WINAPI *)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using NtQueryKeyFunction = LONG (NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);

GeoNameFunction g_geoName = nullptr;
LocaleNameFunction g_localeName = nullptr;
PreferredUiLanguagesFunction g_preferredUiLanguages = nullptr;
QueryValueWFunction g_queryValueW = nullptr;
QueryValueAFunction g_queryValueA = nullptr;
GetValueWFunction g_getValueW = nullptr;
GetValueAFunction g_getValueA = nullptr;
NtQueryKeyFunction g_ntQueryKey = nullptr;

SRWLOCK g_installLock = SRWLOCK_INIT;
bool g_installed = false;
bool g_rollbackFailed = false;

// Native wrappers may themselves call one of the other hooked exports.
// Internal probes and native parameter validation must see original signals.
thread_local unsigned int g_internalDepth = 0;

class InternalCall final {
public:
    InternalCall() noexcept { ++g_internalDepth; }
    ~InternalCall() noexcept {
        // Some MinGW TLS runtimes use TlsGetValue, which can clear LastError.
        const DWORD error = GetLastError();
        --g_internalDepth;
        SetLastError(error);
    }
    InternalCall(const InternalCall&) = delete;
    InternalCall& operator=(const InternalCall&) = delete;
};

class InstallLock final {
public:
    InstallLock() noexcept { AcquireSRWLockExclusive(&g_installLock); }
    ~InstallLock() noexcept { ReleaseSRWLockExclusive(&g_installLock); }
    InstallLock(const InstallLock&) = delete;
    InstallLock& operator=(const InstallLock&) = delete;
};

constexpr unsigned int FoldAscii(unsigned int ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

template <typename Char>
bool IsDeviceValue(const Char* value) noexcept {
    if (value == nullptr) return false;
    for (std::size_t i = 0; i < sizeof(kDeviceValue); ++i) {
        if (FoldAscii(static_cast<unsigned int>(value[i])) !=
            FoldAscii(static_cast<unsigned int>(kDeviceValue[i]))) return false;
        if (kDeviceValue[i] == '\0') return true;
    }
    return false;
}

template <std::size_t N>
bool SameNativePath(const wchar_t* text, std::size_t length,
                    const wchar_t (&expected)[N]) noexcept {
    if (length != N - 1) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (FoldAscii(static_cast<unsigned int>(text[i])) !=
            FoldAscii(static_cast<unsigned int>(expected[i]))) return false;
    }
    return true;
}

bool IsDeviceKey(HKEY key) noexcept {
    if (key == nullptr || g_ntQueryKey == nullptr) return false;

    // KEY_NAME_INFORMATION has a byte count followed by non-NUL-terminated
    // WCHARs. A deliberately bounded stack buffer is ample for our two exact
    // target paths; longer/unrecognized names always pass through unchanged.
    struct KeyNameBuffer {
        ULONG nameBytes;
        WCHAR name[1024];
    } buffer{};
    ULONG returnedBytes = 0;
    const LONG status = g_ntQueryKey(key, kKeyNameInformation, &buffer,
                                   static_cast<ULONG>(sizeof(buffer)), &returnedBytes);
    if (status < 0 || returnedBytes < sizeof(ULONG) ||
        buffer.nameBytes > sizeof(buffer.name) ||
        buffer.nameBytes % sizeof(WCHAR) != 0 ||
        buffer.nameBytes > returnedBytes - sizeof(ULONG)) return false;

    const std::size_t count = buffer.nameBytes / sizeof(WCHAR);
    return SameNativePath(buffer.name, count, kDeviceKey) ||
           SameNativePath(buffer.name, count, kDeviceKeyWow32);
}

template <typename Char, typename OpenFunction>
bool IsDeviceSubKey(HKEY parent, const Char* subKey, DWORD flags,
                    OpenFunction openKey) noexcept {
    if (subKey == nullptr || subKey[0] == 0) return IsDeviceKey(parent);

    REGSAM access = KEY_QUERY_VALUE;
    if ((flags & RRF_SUBKEY_WOW6464KEY) != 0) access |= KEY_WOW64_64KEY;
    if ((flags & RRF_SUBKEY_WOW6432KEY) != 0) access |= KEY_WOW64_32KEY;

    // Opening the real subkey also handles predefined HKLM, relative paths,
    // registry redirection and case-insensitive matching without tracking
    // handles. A recycled handle can never retain stale path metadata.
    HKEY opened = nullptr;
    if (openKey(parent, subKey, 0, access, &opened) != ERROR_SUCCESS) return false;
    const bool matches = IsDeviceKey(opened);
    RegCloseKey(opened);
    return matches;
}

LSTATUS WriteDwordResult(LPDWORD type, PVOID data, LPDWORD bytes,
                        DWORD capacity, bool allowed, bool zeroOnFailure) noexcept {
    if (type != nullptr) *type = REG_DWORD;
    if (bytes != nullptr) *bytes = kDwordBytes;

    LSTATUS status = ERROR_SUCCESS;
    if (!allowed) {
        status = ERROR_UNSUPPORTED_TYPE;
    } else if (data != nullptr && capacity < kDwordBytes) {
        status = ERROR_MORE_DATA;
    } else if (data != nullptr) {
        // memcpy supports unaligned caller buffers and leaves their tail alone.
        const DWORD value = kHongKongGeoId;
        std::memcpy(data, &value, sizeof(value));
    }

    if (status != ERROR_SUCCESS && zeroOnFailure && data != nullptr && capacity != 0) {
        std::memset(data, 0, capacity);
    }
    return status;
}

template <typename Char, typename QueryFunction>
LSTATUS QueryValueImpl(QueryFunction original, HKEY key, const Char* value,
                       LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD bytes,
                       DWORD incomingLastError) noexcept {
    if (reserved != nullptr || (data != nullptr && bytes == nullptr) ||
        !IsDeviceValue(value) || !IsDeviceKey(key)) {
        SetLastError(incomingLastError);
        return original(key, value, reserved, type, data, bytes);
    }

    DWORD actualType = 0;
    DWORD actualBytes = 0;
    const LSTATUS probe = original(key, value, nullptr, &actualType, nullptr, &actualBytes);
    if (probe != ERROR_SUCCESS) {
        // Preserve the real API's error and output-parameter behavior for a
        // missing/inaccessible value. No keys or values are fabricated.
        SetLastError(incomingLastError);
        return original(key, value, reserved, type, data, bytes);
    }

    const DWORD capacity = data != nullptr ? *bytes : 0;
    const LSTATUS status = WriteDwordResult(type, data, bytes, capacity, true, false);
    // Registry APIs report LSTATUS directly; our private probes must not leak a
    // different thread LastError into otherwise successful caller code.
    SetLastError(incomingLastError);
    return status;
}

template <typename Char, typename GetFunction, typename OpenFunction>
LSTATUS GetValueImpl(GetFunction original, OpenFunction openKey,
                     HKEY key, const Char* subKey, const Char* value, DWORD flags,
                     LPDWORD type, PVOID data, LPDWORD bytes,
                     DWORD incomingLastError) noexcept {
    if ((data != nullptr && bytes == nullptr) ||
        (flags & kWow64ViewFlags) == kWow64ViewFlags ||
        !IsDeviceValue(value) || !IsDeviceSubKey(key, subKey, flags, openKey)) {
        SetLastError(incomingLastError);
        return original(key, subKey, value, flags, type, data, bytes);
    }

    DWORD actualType = 0;
    DWORD actualBytes = 0;
    // Let Windows validate the original flags before we emulate a DWORD.
    // A native unsupported-type/size result can refer to the stored type,
    // which is deliberately replaced by the virtual REG_DWORD result.
    LSTATUS probe = original(key, subKey, value, flags,
                            &actualType, nullptr, &actualBytes);
    if (probe == ERROR_UNSUPPORTED_TYPE || probe == ERROR_DATATYPE_MISMATCH) {
        const DWORD probeFlags = (flags & kWow64ViewFlags) | RRF_RT_ANY | RRF_NOEXPAND;
        probe = original(key, subKey, value, probeFlags,
                         &actualType, nullptr, &actualBytes);
    }
    if (probe != ERROR_SUCCESS) {
        SetLastError(incomingLastError);
        return original(key, subKey, value, flags, type, data, bytes);
    }

    // A zero type mask allows no types. RRF_RT_ANY, not flags=0, means no
    // restriction; RRF_RT_DWORD also contains the RRF_RT_REG_DWORD bit.
    const bool acceptsDword = (flags & RRF_RT_REG_DWORD) != 0;
    const DWORD capacity = data != nullptr ? *bytes : 0;
    const LSTATUS status = WriteDwordResult(type, data, bytes, capacity, acceptsDword,
                                           (flags & RRF_ZEROONFAILURE) != 0);
    SetLastError(incomingLastError);
    return status;
}

int WINAPI HookGeoName(LPWSTR name, int count) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_geoName(name, count);
    }
    if (count < 0 || (count > 0 && name == nullptr)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (count == 0) {
        SetLastError(incomingLastError);
        return 3;  // H, K, NUL; do not touch even a non-NULL buffer.
    }
    if (count < 3) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    name[0] = L'H';
    name[1] = L'K';
    name[2] = L'\0';
    SetLastError(incomingLastError);
    return 3;
}

int WINAPI HookLocaleName(LPWSTR name, int count) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_localeName(name, count);
    }
    constexpr int required = static_cast<int>(sizeof(kHongKongLocale) / sizeof(WCHAR));
    if (count <= 0 || name == nullptr) {
        // Keep the platform's invalid-parameter behavior. If the native API
        // accepts a zero-length size query, report the virtual locale's size.
        // The guard also prevents native validation from seeing our other hooks.
        InternalCall internal;
        SetLastError(incomingLastError);
        const int native = g_localeName(name, count);
        if (count == 0 && native != 0) {
            SetLastError(incomingLastError);
            return required;
        }
        return native;
    }
    if (count < required) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(name, kHongKongLocale, sizeof(kHongKongLocale));
    SetLastError(incomingLastError);
    return required;
}

BOOL WINAPI HookPreferredUiLanguages(DWORD flags, PULONG languageCount,
                                     PZZWSTR languages, PULONG characters) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_preferredUiLanguages(flags, languageCount, languages, characters);
    }
    // GetUserPreferredUILanguages documents only these two mutually exclusive
    // formats; zero defaults to names. Flags belonging to the thread/system
    // MUI APIs must not be silently accepted here. Let Windows validate them.
    const bool supportedFormat = flags == 0 || flags == MUI_LANGUAGE_NAME ||
                                 flags == MUI_LANGUAGE_ID;
    if (!supportedFormat || languageCount == nullptr || characters == nullptr ||
        (languages == nullptr && *characters != 0)) {
        InternalCall internal;
        SetLastError(incomingLastError);
        return g_preferredUiLanguages(flags, languageCount, languages, characters);
    }

    const bool identifiers = flags == MUI_LANGUAGE_ID;
    const WCHAR* result = identifiers ? kHongKongUiIds : kHongKongUiNames;
    const ULONG required = static_cast<ULONG>(identifiers
        ? sizeof(kHongKongUiIds) / sizeof(WCHAR)
        : sizeof(kHongKongUiNames) / sizeof(WCHAR));
    const ULONG capacity = *characters;
    *characters = required;
    if (languages != nullptr && capacity < required) {
        // Report the required MULTI_SZ length, including both trailing NULs;
        // leave the caller's small buffer and language count untouched.
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (languages != nullptr) {
        std::memcpy(languages, result, static_cast<std::size_t>(required) * sizeof(WCHAR));
    }
    *languageCount = 1;
    SetLastError(incomingLastError);
    return TRUE;
}

LSTATUS WINAPI HookQueryValueW(HKEY key, LPCWSTR value, LPDWORD reserved,
                               LPDWORD type, LPBYTE data, LPDWORD bytes) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_queryValueW(key, value, reserved, type, data, bytes);
    }
    InternalCall internal;
    return QueryValueImpl(g_queryValueW, key, value, reserved, type, data, bytes, incomingLastError);
}

LSTATUS WINAPI HookQueryValueA(HKEY key, LPCSTR value, LPDWORD reserved,
                               LPDWORD type, LPBYTE data, LPDWORD bytes) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_queryValueA(key, value, reserved, type, data, bytes);
    }
    InternalCall internal;
    return QueryValueImpl(g_queryValueA, key, value, reserved, type, data, bytes, incomingLastError);
}

LSTATUS WINAPI HookGetValueW(HKEY key, LPCWSTR subKey, LPCWSTR value, DWORD flags,
                             LPDWORD type, PVOID data, LPDWORD bytes) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_getValueW(key, subKey, value, flags, type, data, bytes);
    }
    InternalCall internal;
    return GetValueImpl(g_getValueW, &RegOpenKeyExW, key, subKey, value, flags,
                        type, data, bytes, incomingLastError);
}

LSTATUS WINAPI HookGetValueA(HKEY key, LPCSTR subKey, LPCSTR value, DWORD flags,
                             LPDWORD type, PVOID data, LPDWORD bytes) noexcept {
    const DWORD incomingLastError = GetLastError();
    if (g_internalDepth != 0) {
        SetLastError(incomingLastError);
        return g_getValueA(key, subKey, value, flags, type, data, bytes);
    }
    InternalCall internal;
    return GetValueImpl(g_getValueA, &RegOpenKeyExA, key, subKey, value, flags,
                        type, data, bytes, incomingLastError);
}

struct HookSlot {
    LPCWSTR module;
    const char* name;
    LPVOID detour;
    LPVOID target;
    LPVOID trampoline;
    bool created;
};

HookSlot g_hooks[] = {
    {L"kernel32.dll", "GetUserDefaultGeoName", reinterpret_cast<LPVOID>(&HookGeoName), nullptr, nullptr, false},
    {L"advapi32.dll", "RegQueryValueExW", reinterpret_cast<LPVOID>(&HookQueryValueW), nullptr, nullptr, false},
    {L"advapi32.dll", "RegQueryValueExA", reinterpret_cast<LPVOID>(&HookQueryValueA), nullptr, nullptr, false},
    {L"advapi32.dll", "RegGetValueW", reinterpret_cast<LPVOID>(&HookGetValueW), nullptr, nullptr, false},
    {L"advapi32.dll", "RegGetValueA", reinterpret_cast<LPVOID>(&HookGetValueA), nullptr, nullptr, false},
    {L"kernel32.dll", "GetUserDefaultLocaleName", reinterpret_cast<LPVOID>(&HookLocaleName), nullptr, nullptr, false},
    {L"kernel32.dll", "GetUserPreferredUILanguages", reinterpret_cast<LPVOID>(&HookPreferredUiLanguages), nullptr, nullptr, false},
};

void DebugStatus(const wchar_t* operation, int status) noexcept {
    wchar_t message[160]{};
    const int written = std::swprintf(message, sizeof(message) / sizeof(message[0]),
                                      L"[HKRegion] %ls failed, status=%d.\n", operation, status);
    if (written > 0) OutputDebugStringW(message);
}

DWORD PinSelf() noexcept {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&HKRegion_Install), &module)) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return error != ERROR_SUCCESS ? error : ERROR_INVALID_HANDLE;
}

void RollBack(bool ownMinHook) noexcept {
    bool clean = true;
    // Disable every owned entry point before releasing any trampoline: one
    // original Win32 wrapper can call another entry point in this same set.
    for (auto& hook : g_hooks) {
        if (!hook.created) continue;
        const MH_STATUS disabled = MH_DisableHook(hook.target);
        if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) {
            DebugStatus(L"rollback MH_DisableHook", static_cast<int>(disabled));
            clean = false;
        }
    }
    if (clean) {
        for (auto& hook : g_hooks) {
            if (!hook.created) continue;
            const MH_STATUS removed = MH_RemoveHook(hook.target);
            if (removed != MH_OK) {
                DebugStatus(L"rollback MH_RemoveHook", static_cast<int>(removed));
                clean = false;
                continue;
            }
            hook.created = false;
            hook.trampoline = nullptr;
        }
    }
    if (clean && ownMinHook) {
        const MH_STATUS result = MH_Uninitialize();
        if (result != MH_OK && result != MH_ERROR_NOT_INITIALIZED) {
            DebugStatus(L"rollback MH_Uninitialize", static_cast<int>(result));
            clean = false;
        }
    }
    if (!clean) {
        g_rollbackFailed = true;
        // Preserve code/trampolines that could still be referenced. Never let
        // a caller mistake a partially failed rollback for a fresh state.
        const DWORD pinned = PinSelf();
        if (pinned != ERROR_SUCCESS) DebugStatus(L"rollback pin", static_cast<int>(pinned));
        OutputDebugStringW(L"[HKRegion] Rollback incomplete; terminate this process before retrying.\n");
    }
}

DWORD InstallImpl() noexcept {
    InstallLock lock;
    if (g_installed) return ERROR_SUCCESS;
    if (g_rollbackFailed) return ERROR_BUSY;

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ntQueryKey = ntdll != nullptr
        ? reinterpret_cast<NtQueryKeyFunction>(GetProcAddress(ntdll, "NtQueryKey")) : nullptr;
    if (g_ntQueryKey == nullptr) return ERROR_PROC_NOT_FOUND;

    for (auto& hook : g_hooks) {
        const HMODULE module = GetModuleHandleW(hook.module);
        hook.target = module != nullptr
            ? reinterpret_cast<LPVOID>(GetProcAddress(module, hook.name)) : nullptr;
        if (hook.target == nullptr) {
            OutputDebugStringW(L"[HKRegion] A required Windows API export is unavailable.\n");
            return ERROR_PROC_NOT_FOUND;
        }
        // GetProcAddress resolves export forwarders to their actual code target.
    }

    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        DebugStatus(L"MH_Initialize", static_cast<int>(initialized));
        return ERROR_GEN_FAILURE;
    }
    const bool ownMinHook = initialized == MH_OK;

    for (auto& hook : g_hooks) {
        const MH_STATUS created = MH_CreateHook(hook.target, hook.detour, &hook.trampoline);
        if (created != MH_OK) {
            DebugStatus(L"MH_CreateHook", static_cast<int>(created));
            RollBack(ownMinHook);
            return ERROR_GEN_FAILURE;
        }
        hook.created = true;
    }

    g_geoName = reinterpret_cast<GeoNameFunction>(g_hooks[0].trampoline);
    g_queryValueW = reinterpret_cast<QueryValueWFunction>(g_hooks[1].trampoline);
    g_queryValueA = reinterpret_cast<QueryValueAFunction>(g_hooks[2].trampoline);
    g_getValueW = reinterpret_cast<GetValueWFunction>(g_hooks[3].trampoline);
    g_getValueA = reinterpret_cast<GetValueAFunction>(g_hooks[4].trampoline);
    g_localeName = reinterpret_cast<LocaleNameFunction>(g_hooks[5].trampoline);
    g_preferredUiLanguages = reinterpret_cast<PreferredUiLanguagesFunction>(g_hooks[6].trampoline);

    for (auto& hook : g_hooks) {
        const MH_STATUS queued = MH_QueueEnableHook(hook.target);
        if (queued != MH_OK) {
            DebugStatus(L"MH_QueueEnableHook", static_cast<int>(queued));
            RollBack(ownMinHook);
            return ERROR_GEN_FAILURE;
        }
    }
    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        DebugStatus(L"MH_ApplyQueued", static_cast<int>(applied));
        RollBack(ownMinHook);
        return ERROR_GEN_FAILURE;
    }

    const DWORD pinned = PinSelf();
    if (pinned != ERROR_SUCCESS) {
        DebugStatus(L"GetModuleHandleExW(PIN)", static_cast<int>(pinned));
        RollBack(ownMinHook);
        return pinned;
    }
    g_installed = true;
    OutputDebugStringW(L"[HKRegion] Installed: GeoName=HK; user Locale/UI=zh-HK; existing DeviceRegion reads=104.\n");
    return ERROR_SUCCESS;
}

}  // namespace

extern "C" HK_REGION_API DWORD WINAPI HKRegion_Install(LPVOID reserved) noexcept {
    if (reserved != nullptr) return ERROR_INVALID_PARAMETER;
    // No work is performed from DllMain; the caller must invoke this explicitly
    // after LoadLibrary has returned. No C++ allocations/exceptions are used.
    return InstallImpl();
}

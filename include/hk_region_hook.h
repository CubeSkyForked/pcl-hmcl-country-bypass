#ifndef HK_REGION_HOOK_H
#define HK_REGION_HOOK_H

#include <windows.h>

#if defined(HK_REGION_HOOK_BUILD_DLL)
#define HK_REGION_API __declspec(dllexport)
#else
#define HK_REGION_API
#endif

#ifdef __cplusplus
extern "C" {
#define HK_REGION_NOEXCEPT noexcept
#else
#define HK_REGION_NOEXCEPT
#endif

/*
 * Install the process-local overrides. Pass NULL for reserved.
 * Call from normal application/thread context, never from DllMain or while
 * holding the Windows loader lock. Keep the LoadLibrary reference until this
 * function returns. Successful installation pins the DLL until process exit.
 * Returns ERROR_SUCCESS (also when already installed), or a Win32 error code.
 * MinHook failures return ERROR_GEN_FAILURE; details use OutputDebugStringW.
 * There is deliberately no runtime uninstall operation.
 *
 * The process-local profile covers the signals used by PCL-CE RegionPolicy:
 *   GetUserDefaultGeoName -> HK;
 *   GetUserDefaultLocaleName -> zh-HK;
 *   GetUserPreferredUILanguages -> one zh-HK entry (or language ID 0C04).
 *   existing, readable HKLM DeviceRegion values -> REG_DWORD 104.
 * UI language results use a double-NUL-terminated MULTI_SZ and character counts.
 * Unsupported/invalid MUI forms are delegated to the native API for validation.
 * No system settings or registry contents are written. Time zone is unchanged.
 */
HK_REGION_API DWORD WINAPI HKRegion_Install(LPVOID reserved) HK_REGION_NOEXCEPT;

typedef DWORD (WINAPI *HKRegionInstallFunction)(LPVOID reserved);

#ifdef __cplusplus
}
#endif

#undef HK_REGION_NOEXCEPT

#endif

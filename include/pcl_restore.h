#pragma once
#include <windows.h>

// Called only on the proxy's initialization thread, after native initialization.
void HKR_InitializeManagedRestore(HMODULE proxy) noexcept;
extern "C" DWORD WINAPI HKRegion_GetManagedPatchStatus(void) noexcept;

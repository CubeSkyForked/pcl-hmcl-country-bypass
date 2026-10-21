#pragma once
#include <windows.h>
#include <string>

// Publishes only the verified JAR; unlike the rundll32 entry, writes no manifest.
DWORD HKR_ExtractHmclAgentPayload(const std::wstring& directory, std::wstring& path) noexcept;

// Loader-lock-safe: arm an HMCL candidate's CreateProcessW import.
// Archive recognition, extraction, argument parsing and logging wait for that call.
void HKR_ArmHmclLauncher() noexcept;

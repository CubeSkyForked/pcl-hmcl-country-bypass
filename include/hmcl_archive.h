#pragma once
#include <windows.h>

// ZIP central-directory recognition only. JVM class semantics are checked by the agent.
bool HKR_IsHmclArchiveW(LPCWSTR path) noexcept;

#include "hmcl_archive.h"
#include <cwchar>

int wmain(int count, wchar_t** args) {
    if (count != 3) return 2;
    const bool expected = std::wcscmp(args[2], L"1") == 0;
    return HKR_IsHmclArchiveW(args[1]) == expected ? 0 : 1;
}

#include <windows.h>
#include <shellapi.h>
#include "hmcl_agent.h"
#include "hmcl_agent_payload.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

class File final {
public:
    HANDLE h;
    explicit File(HANDLE handle) : h(handle) {}
    ~File() { Close(); }
    void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
};

bool Matches(const std::wstring& path) {
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (file.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.h, &size) ||
        size.QuadPart != sizeof(kHmclAgentPayload)) return false;
    BYTE bytes[4096];
    size_t offset = 0;
    while (offset < sizeof(kHmclAgentPayload)) {
        DWORD remaining = static_cast<DWORD>(sizeof(kHmclAgentPayload) - offset);
        DWORD wanted = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        DWORD count = 0;
        if (!ReadFile(file.h, bytes, wanted, &count, nullptr) || count != wanted ||
            std::memcmp(bytes, kHmclAgentPayload + offset, count)) return false;
        offset += count;
    }
    return true;
}

DWORD ExtractJar(const std::wstring& directory, std::wstring& path) {
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return GetLastError();
    const std::wstring name = std::wstring(L"HKRegion.HMCL.Agent.") + kHmclAgentHash + L".jar";
    path = directory + L"\\" + name;
    if (!Matches(path)) {
        WCHAR temporary[MAX_PATH]{};
        if (!GetTempFileNameW(directory.c_str(), L"HKJ", 0, temporary)) return GetLastError();
        File file(CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0, error = ERROR_SUCCESS;
        if (file.h == INVALID_HANDLE_VALUE) error = GetLastError();
        else if (!WriteFile(file.h, kHmclAgentPayload, sizeof(kHmclAgentPayload), &written, nullptr) ||
                 written != sizeof(kHmclAgentPayload) || !FlushFileBuffers(file.h))
            error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        file.Close();
        if (!error && !MoveFileExW(temporary, path.c_str(), 0)) error = GetLastError();
        DeleteFileW(temporary);
        if (!Matches(path)) return error ? error : ERROR_FILE_CORRUPT;
    }
    return ERROR_SUCCESS;
}

DWORD Extract(const std::wstring& directory, const WCHAR* requestId) {
    std::wstring path;
    const DWORD result = ExtractJar(directory, path);
    if (result) return result;
    // A caller-provided nonce prevents a failed extraction from looking like
    // success merely because a manifest from an earlier invocation exists.
    char request[80]{};
    size_t length = 0;
    for (; requestId[length]; ++length) {
        const WCHAR c = requestId[length];
        if (length >= sizeof(request) - 1 ||
            !((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
              (c >= L'A' && c <= L'F') || c == L'-')) return ERROR_INVALID_PARAMETER;
        request[length] = static_cast<char>(c);
    }
    char manifest[512]{};
    const int count = std::snprintf(manifest, sizeof(manifest),
        "{\"file\":\"HKRegion.HMCL.Agent.%s.jar\",\"sha256\":\"%s\",\"requestId\":\"%s\"}\n",
        kHmclAgentHashA, kHmclAgentHashA, request);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(manifest)) return ERROR_INSUFFICIENT_BUFFER;
    File file(CreateFileW((directory + L"\\hmcl-agent.json").c_str(), GENERIC_WRITE,
                          FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    if (!WriteFile(file.h, manifest, static_cast<DWORD>(count), &written, nullptr) ||
        written != static_cast<DWORD>(count) || !FlushFileBuffers(file.h)) return ERROR_WRITE_FAULT;
    return ERROR_SUCCESS;
}

} // namespace

DWORD HKR_ExtractHmclAgentPayload(const std::wstring& directory, std::wstring& path) noexcept {
    try { return ExtractJar(directory, path); }
    catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}

// rundll32 <proxy>,HKRegion_ExtractHmclAgent "<cache directory>" <request GUID>
// This executes in the short-lived helper process, outside the launcher's JVM.
extern "C" void CALLBACK HKRegion_ExtractHmclAgentW(HWND, HINSTANCE, LPWSTR commandLine, int) noexcept {
    DWORD result = ERROR_INVALID_PARAMETER;
    try {
        const std::wstring command = std::wstring(L"hkr ") + commandLine;
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(command.c_str(), &count);
        if (args) {
            if (count == 3) result = Extract(args[1], args[2]);
            LocalFree(args);
        }
    } catch (...) { result = ERROR_NOT_ENOUGH_MEMORY; }
    if (result) OutputDebugStringW(L"[HKRegion] Java agent extraction failed; no valid fresh manifest was published.\n");
    SetLastError(result);
}

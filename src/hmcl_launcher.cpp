#include "hmcl_agent.h"
#include "hmcl_archive.h"

#if defined(_M_IX86) || defined(__i386__)
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {
using CreateProcessFunction = decltype(&CreateProcessW);
CreateProcessFunction g_createProcess = nullptr;
INIT_ONCE g_prepareOnce = INIT_ONCE_STATIC_INIT;
DWORD g_prepareStatus = ERROR_IO_PENDING;
std::wstring g_nativeLog, g_image, g_agent, g_option;
WCHAR g_launcherPath[32768]{};
struct File {
    HANDLE h;
    explicit File(const WCHAR* path) : h(CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
struct Arguments {
    int count = 0;
    LPWSTR* values;
    explicit Arguments(LPCWSTR command) : values(CommandLineToArgvW(command, &count)) {}
    ~Arguments() { if (values) LocalFree(values); }
};

void Log(const char* message, DWORD error = 0, DWORD child = 0) noexcept {
    if (g_nativeLog.empty()) return;
    const HANDLE file = CreateFileW(g_nativeLog.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[256];
    const int count = std::snprintf(line, sizeof(line), "[pid=%lu] %s; error=%lu; child=%lu\r\n",
        GetCurrentProcessId(), message, error, child);
    DWORD written = 0;
    if (count > 0 && static_cast<size_t>(count) < sizeof(line))
        WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
    CloseHandle(file);
}

std::wstring FullPath(const std::wstring& path) {
    std::vector<WCHAR> buffer(32768);
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return length && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring();
}

std::wstring Base64Utf8(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::vector<char> bytes(static_cast<size_t>(size));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
        static_cast<int>(value.size()), bytes.data(), size, nullptr, nullptr) != size) return {};
    constexpr WCHAR alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring result;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned bits = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) bits |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) bits |= static_cast<unsigned char>(bytes[i + 2]);
        result += alphabet[(bits >> 18) & 63];
        result += alphabet[(bits >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(bits >> 6) & 63] : L'=';
        result += i + 2 < bytes.size() ? alphabet[bits & 63] : L'=';
    }
    return result;
}

BOOL CALLBACK Prepare(PINIT_ONCE, PVOID, PVOID*) noexcept {
    try {
        std::vector<WCHAR> image(32768);
        const DWORD length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (!length || length >= image.size()) { g_prepareStatus = ERROR_FILENAME_EXCED_RANGE; return TRUE; }
        g_image = FullPath(std::wstring(image.data(), length));
        const size_t slash = g_image.find_last_of(L"\\/");
        if (slash == std::wstring::npos) { g_prepareStatus = ERROR_BAD_PATHNAME; return TRUE; }
        const std::wstring directory = g_image.substr(0, slash + 1);
        g_nativeLog = directory + L"HKRegion-hmcl-launcher.log";
        if (!HKR_IsHmclArchiveW(g_image.c_str())) {
            g_prepareStatus = ERROR_NOT_SUPPORTED;
            Log("hmcl-archive-structure-rejected", g_prepareStatus);
            return TRUE;
        }
        g_prepareStatus = HKR_ExtractHmclAgentPayload(directory + L"HKRegionCache", g_agent);
        if (!g_prepareStatus) {
            const std::wstring encoded = Base64Utf8(directory + L"HKRegion-hmcl-agent.log");
            if (encoded.empty()) g_prepareStatus = ERROR_NO_UNICODE_TRANSLATION;
            else g_option = L"-javaagent:" + g_agent + L"=log64:" + encoded;
        }
        Log("launcher-prepared", g_prepareStatus);
    } catch (...) {
        g_prepareStatus = ERROR_NOT_ENOUGH_MEMORY;
        Log("launcher-prepare-failed", g_prepareStatus);
    }
    return TRUE; // Cache failure as well as success; never repeatedly modify startup.
}

std::wstring Quote(const WCHAR* argument) {
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (const WCHAR* p = argument; *p; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        result.append(slashes * (*p == L'"' ? 2 : 1), L'\\');
        if (*p == L'"') result += L'\\';
        slashes = 0;
        result += *p;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

bool SameImage(const std::wstring& candidate) {
    File a(candidate.c_str()), b(g_image.c_str());
    BY_HANDLE_FILE_INFORMATION left{}, right{};
    return a.h != INVALID_HANDLE_VALUE && b.h != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a.h, &left) && GetFileInformationByHandle(b.h, &right) &&
        left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
        left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
}

bool BuildCommand(LPCWSTR application, LPCWSTR command, LPCWSTR workingDirectory, std::wstring& result) {
    if (!command) return false;
    Arguments args(command);
    if (!args.values || args.count < 3) return false;
    const WCHAR* executable = application && *application ? application : args.values[0];
    const WCHAR* name = std::wcsrchr(executable, L'\\');
    name = name ? name + 1 : executable;
    if (_wcsicmp(name, L"java.exe") && _wcsicmp(name, L"javaw.exe")) return false;
    int jarIndex = 0;
    for (int i = 1; i + 1 < args.count; ++i) {
        if (std::wcscmp(args.values[i], L"-jar") == 0) { jarIndex = i; break; }
    }
    if (!jarIndex) return false; // JVM discovery commands and unrelated children stay unchanged.
    if (!InitOnceExecuteOnce(&g_prepareOnce, Prepare, nullptr, nullptr) || g_prepareStatus) return false;
    std::wstring archive(args.values[jarIndex + 1]);
    // Resolve relative -jar paths in the child's working directory, not the caller's.
    if (archive.size() < 2 || (archive[0] != L'\\' && archive[1] != L':')) {
        const std::wstring directory = workingDirectory && *workingDirectory ? FullPath(workingDirectory) : FullPath(L".");
        archive = directory + L"\\" + archive;
    }
    if (!SameImage(archive)) return false;
    for (int i = 1; i < jarIndex; ++i) {
        if (std::wcsncmp(args.values[i], L"-javaagent:", 11) == 0 &&
            std::wcsstr(args.values[i], L"HKRegion.HMCL.Agent.")) {
            Log("agent-already-present");
            return false;
        }
    }
    for (int i = 0; i < args.count; ++i) {
        if (i) result += L' ';
        if (i == jarIndex) result += Quote(g_option.c_str()) + L' ';
        result += Quote(args.values[i]);
    }
    if (result.size() >= 32767) { Log("command-line-too-long", ERROR_FILENAME_EXCED_RANGE); return false; }
    return true;
}

BOOL WINAPI CreateProcessWithAgent(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES processAttributes,
    LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) noexcept {
    const DWORD entryError = GetLastError();
    DWORD error;
    BOOL success;
    {
        std::vector<WCHAR> buffer;
        LPWSTR forwarded = command;
        bool added = false;
        try {
            std::wstring modified;
            if (BuildCommand(application, command, directory, modified)) {
                buffer.assign(modified.begin(), modified.end());
                buffer.push_back(L'\0');
                forwarded = buffer.data();
                added = true;
            }
        } catch (...) { Log("command-preparation-failed", ERROR_NOT_ENOUGH_MEMORY); }
        SetLastError(entryError);
        success = g_createProcess(application, forwarded, processAttributes, threadAttributes,
            inheritHandles, flags, environment, directory, startup, process);
        error = GetLastError();
        if (added) Log("child-created-with-agent", success ? ERROR_SUCCESS : error,
            success && process ? process->dwProcessId : 0);
    } // Destroy temporary buffers before restoring CreateProcessW's LastError.
    SetLastError(error);
    return success;
}
} // namespace

void HKR_ArmHmclLauncher() noexcept {
    // Only mapped PE metadata / the launcher name are read under loader lock.
    // The JAR structure and actual class semantics are validated after startup.
    const auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (!base) return;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    bool candidate = false;
    const auto& exports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exports.VirtualAddress < size && exports.Size >= sizeof(IMAGE_EXPORT_DIRECTORY) && exports.Size <= size - exports.VirtualAddress) {
        const auto table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exports.VirtualAddress);
        if (table->Name < size && std::memchr(base + table->Name, 0, size - table->Name))
            candidate = _stricmp(reinterpret_cast<const char*>(base + table->Name), "HMCLauncher.exe") == 0;
    }
    if (!candidate) {
        const DWORD capacity = static_cast<DWORD>(sizeof(g_launcherPath) / sizeof(g_launcherPath[0]));
        const DWORD length = GetModuleFileNameW(nullptr, g_launcherPath, capacity);
        if (length && length < capacity) {
            const WCHAR* name = std::wcsrchr(g_launcherPath, L'\\');
            name = name ? name + 1 : g_launcherPath;
            candidate = _wcsnicmp(name, L"HMCL", 4) == 0 || _wcsnicmp(name, L"HMCLauncher", 11) == 0;
        }
    }
    if (!candidate) return;
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= size || imports.Size > size - imports.VirtualAddress) return;
    const auto descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (size_t i = 0; i < imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) && descriptors[i].Name; ++i) {
        const auto& descriptor = descriptors[i];
        if (descriptor.Name >= size || !descriptor.OriginalFirstThunk || descriptor.OriginalFirstThunk >= size ||
            !descriptor.FirstThunk || descriptor.FirstThunk >= size) continue;
        if (!std::memchr(base + descriptor.Name, 0, size - descriptor.Name)) continue;
        const char* library = reinterpret_cast<const char*>(base + descriptor.Name);
        if (_stricmp(library, "kernel32.dll") && _stricmp(library, "kernelbase.dll") &&
            _strnicmp(library, "api-ms-win-core-processthreads-", 30)) continue;
        const auto names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + descriptor.OriginalFirstThunk);
        const auto slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor.FirstThunk);
        const size_t maximum = (size - (descriptor.FirstThunk > descriptor.OriginalFirstThunk
            ? descriptor.FirstThunk : descriptor.OriginalFirstThunk)) / sizeof(IMAGE_THUNK_DATA32);
        for (size_t j = 0; j < maximum && names[j].u1.AddressOfData; ++j) {
            if (IMAGE_SNAP_BY_ORDINAL32(names[j].u1.Ordinal) || names[j].u1.AddressOfData >= size - sizeof(IMAGE_IMPORT_BY_NAME)) continue;
            const auto entry = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[j].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(entry->Name), "CreateProcessW")) continue;
            auto slot = reinterpret_cast<PVOID volatile*>(&slots[j].u1.Function);
            const auto original = reinterpret_cast<CreateProcessFunction>(*slot);
            if (!original || original == CreateProcessWithAgent) return;
            DWORD protection = 0;
            if (!VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), PAGE_READWRITE, &protection)) return;
            g_createProcess = original;
            InterlockedExchangePointer(slot, reinterpret_cast<PVOID>(&CreateProcessWithAgent));
            DWORD ignored = 0;
            VirtualProtect(const_cast<PVOID*>(slot), sizeof(PVOID), protection, &ignored);
            OutputDebugStringW(L"[HKRegion] HMCL CreateProcessW import armed.\n");
            return;
        }
    }
}
#else
void HKR_ArmHmclLauncher() noexcept {}
#endif

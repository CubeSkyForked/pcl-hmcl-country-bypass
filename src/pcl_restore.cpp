#include "pcl_restore.h"
#include "managed_payload.h"

#include <mscoree.h>
#include <oleauto.h>
#include <wincrypt.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

volatile LONG g_managedStatus = ERROR_IO_PENDING;
constexpr BYTE kTargetHash[] = {
    0x6b,0x6e,0x12,0x60,0xcb,0x5a,0x1d,0x6a,0x30,0x38,0x55,0xf1,0xc6,0x0c,0xab,0x3d,
    0x0b,0xc1,0xae,0xbb,0xa7,0xe4,0x0a,0x96,0x41,0x80,0x18,0x8b,0x27,0x1e,0x73,0x86
};

} // namespace

// COM implementations live outside this translation unit. These interface
// types need external linkage: GCC may devirtualize an anonymous-namespace
// abstract interface to __cxa_pure_virtual when it sees no local subclasses.
namespace HKRClrHosting {

// ABI prefixes from the public CLR hosting interfaces in MetaHost.h. Keeping
// the declarations here also supports older MinGW headers without MetaHost.h.
struct MetaHost : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetRuntime(LPCWSTR, REFIID, LPVOID*) = 0;
};
struct RuntimeInfo : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVersionString(LPWSTR, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeDirectory(LPWSTR, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLoaded(HANDLE, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadErrorString(UINT, LPWSTR, DWORD*, LONG) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadLibrary(LPCWSTR, HMODULE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcAddress(LPCSTR, LPVOID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFCLSID, REFIID, LPVOID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLoadable(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultStartupFlags(DWORD, LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultStartupFlags(DWORD*, LPWSTR, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BindAsLegacyV2Runtime() = 0;
    virtual HRESULT STDMETHODCALLTYPE IsStarted(BOOL*, DWORD*) = 0;
};

} // namespace HKRClrHosting

namespace {
using HKRClrHosting::MetaHost;
using HKRClrHosting::RuntimeInfo;
constexpr GUID kMetaClass = {0x9280188d,0x0e8e,0x4867,{0xb3,0x0c,0x7f,0xa8,0x38,0x84,0xe8,0xde}};
constexpr GUID kMetaInterface = {0xd332db9e,0xb9b3,0x4125,{0x82,0x07,0xa1,0x48,0x84,0xf5,0x32,0x16}};
constexpr GUID kInfoInterface = {0xbd39d1d2,0xba2f,0x486a,{0x89,0xb0,0xb4,0xb0,0xcb,0x46,0x68,0x91}};
constexpr GUID kRuntimeClass = {0x90f1a06e,0x7712,0x4762,{0x86,0xb5,0x7a,0x5e,0xba,0x6b,0xdb,0x02}};
constexpr GUID kRuntimeInterface = {0x90f1a06c,0x7712,0x4762,{0x86,0xb5,0x7a,0x5e,0xba,0x6b,0xdb,0x02}};

template<class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
};
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit File(HANDLE value) : h(value) {}
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    void Close() { if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
};

bool MatchesImage() {
    std::vector<WCHAR> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    File file(CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (file.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.h, &size) || size.QuadPart != 5980160)
        return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool match = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            BYTE buffer[16384];
            DWORD read = 0;
            bool success = true;
            while (success) {
                if (!ReadFile(file.h, buffer, sizeof(buffer), &read, nullptr)) { success = false; break; }
                if (!read) break;
                success = CryptHashData(hash, buffer, read, 0) != FALSE;
            }
            BYTE digest[32];
            DWORD count = sizeof(digest);
            match = success && CryptGetHashParam(hash, HP_HASHVAL, digest, &count, 0) &&
                count == sizeof(kTargetHash) && std::memcmp(digest, kTargetHash, count) == 0;
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(provider, 0);
    }
    return match;
}

bool PayloadMatches(const std::wstring& path) {
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (file.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.h, &size) ||
        size.QuadPart != sizeof(kManagedPayload)) return false;
    BYTE bytes[4096];
    size_t offset = 0;
    while (offset < sizeof(kManagedPayload)) {
        const DWORD remaining = static_cast<DWORD>(sizeof(kManagedPayload) - offset);
        const DWORD wanted = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        DWORD read = 0;
        if (!ReadFile(file.h, bytes, wanted, &read, nullptr) || read != wanted ||
            std::memcmp(bytes, kManagedPayload + offset, read) != 0) return false;
        offset += read;
    }
    return true;
}

DWORD ExtractPayload(const std::wstring& directory, std::wstring& path) {
    const std::wstring cache = directory + L"HKRegionCache";
    if (!CreateDirectoryW(cache.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return GetLastError();
    path = cache + L"\\HKRegion.Managed." + kManagedPayloadHash + L".dll";
    if (PayloadMatches(path)) return ERROR_SUCCESS;
    // Publish a complete, verified helper atomically; concurrent PCL processes
    // cannot observe a partially written assembly. Never execute a stale file.
    WCHAR temporary[MAX_PATH]{};
    if (!GetTempFileNameW(cache.c_str(), L"HKR", 0, temporary)) return GetLastError();
    File file(CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD count = 0;
    DWORD error = ERROR_SUCCESS;
    if (file.h == INVALID_HANDLE_VALUE) error = GetLastError();
    else if (!WriteFile(file.h, kManagedPayload, sizeof(kManagedPayload), &count, nullptr) ||
             count != sizeof(kManagedPayload) || !FlushFileBuffers(file.h))
        error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    file.Close();
    if (!error && !MoveFileExW(temporary, path.c_str(), 0)) error = GetLastError();
    DeleteFileW(temporary);
    if (PayloadMatches(path)) return ERROR_SUCCESS;
    return error ? error : ERROR_FILE_CORRUPT;
}

void LogStatus(const std::wstring& path, DWORD status) {
    File file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.h == INVALID_HANDLE_VALUE) return;
    char message[128];
    const int count = wsprintfA(message, "[native] managed restore status=%lu (0x%08lX)\r\n", status, status);
    DWORD written = 0;
    WriteFile(file.h, message, static_cast<DWORD>(count), &written, nullptr);
}

void LogClrError(const std::wstring& path) {
    IErrorInfo* info = nullptr;
    if (GetErrorInfo(0, &info) != S_OK || !info) return;
    BSTR description = nullptr;
    if (SUCCEEDED(info->GetDescription(&description)) && description) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            std::vector<char> bytes(static_cast<size_t>(size));
            WideCharToMultiByte(CP_UTF8, 0, description, -1, bytes.data(), size, nullptr, nullptr);
            File file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            DWORD written = 0;
            if (file.h != INVALID_HANDLE_VALUE) {
                WriteFile(file.h, bytes.data(), size - 1, &written, nullptr);
                WriteFile(file.h, "\r\n", 2, &written, nullptr);
            }
        }
        SysFreeString(description);
    }
    info->Release();
}

DWORD Execute(const std::wstring& assembly, const std::wstring& log) {
    // Do not start a second CLR, or change the host's CLR configuration.
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        const HMODULE shim = GetModuleHandleW(L"mscoree.dll");
        if (shim && GetModuleHandleW(L"clr.dll")) {
            using Create = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
            const auto create = reinterpret_cast<Create>(GetProcAddress(shim, "CLRCreateInstance"));
            if (!create) return ERROR_PROC_NOT_FOUND;
            ComPtr<MetaHost> meta;
            HRESULT hr = create(kMetaClass, kMetaInterface, reinterpret_cast<void**>(&meta.p));
            if (FAILED(hr)) return static_cast<DWORD>(hr);
            ComPtr<RuntimeInfo> info;
            hr = meta.p->GetRuntime(L"v4.0.30319", kInfoInterface, reinterpret_cast<void**>(&info.p));
            if (FAILED(hr)) return static_cast<DWORD>(hr);
            BOOL loaded = FALSE, started = FALSE;
            DWORD flags = 0;
            hr = info.p->IsLoaded(GetCurrentProcess(), &loaded);
            if (SUCCEEDED(hr) && loaded) hr = info.p->IsStarted(&started, &flags);
            if (FAILED(hr)) return static_cast<DWORD>(hr);
            if (loaded && started) {
                ComPtr<ICLRRuntimeHost> host;
                hr = info.p->GetInterface(kRuntimeClass, kRuntimeInterface, reinterpret_cast<void**>(&host.p));
                if (FAILED(hr)) return static_cast<DWORD>(hr);
                DWORD result = ERROR_GEN_FAILURE;
                hr = host.p->ExecuteInDefaultAppDomain(assembly.c_str(),
                    L"HKRegion.Managed.PclDownloadRestore", L"Run", log.c_str(), &result);
                if (FAILED(hr)) { LogClrError(log); return static_cast<DWORD>(hr); }
                if (result != ERROR_IO_PENDING) return result;
            }
        }
        Sleep(100);
    }
    return WAIT_TIMEOUT;
}

} // namespace

void HKR_InitializeManagedRestore(HMODULE proxy) noexcept {
    DWORD result = ERROR_NOT_SUPPORTED;
    std::wstring log;
    try {
        if (MatchesImage()) {
            std::vector<WCHAR> path(32768);
            const DWORD length = GetModuleFileNameW(proxy, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) result = ERROR_FILENAME_EXCED_RANGE;
            else {
                std::wstring directory(path.data(), length);
                // The explicit loader uses extended Win32 paths. Framework
                // assembly loading rejects that prefix even for short paths.
                if (directory.compare(0, 8, L"\\\\?\\UNC\\") == 0)
                    directory = L"\\\\" + directory.substr(8);
                else if (directory.compare(0, 4, L"\\\\?\\") == 0 &&
                         directory.size() > 6 && directory[5] == L':')
                    directory.erase(0, 4);
                directory.erase(directory.find_last_of(L"\\/") + 1);
                log = directory + L"HKRegion-managed.log";
                std::wstring assembly;
                result = ExtractPayload(directory, assembly);
                if (!result) result = Execute(assembly, log);
            }
        }
    }
    catch (...) { result = ERROR_NOT_ENOUGH_MEMORY; }
    InterlockedExchange(&g_managedStatus, static_cast<LONG>(result));
    if (!log.empty()) LogStatus(log, result);
}

extern "C" DWORD WINAPI HKRegion_GetManagedPatchStatus(void) noexcept {
    const DWORD entry = GetLastError();
    const DWORD result = static_cast<DWORD>(InterlockedCompareExchange(&g_managedStatus, 0, 0));
    SetLastError(entry);
    return result;
}

#include "hmcl_archive.h"
#include <cstring>
#include <vector>

namespace {
struct File {
    HANDLE h;
    explicit File(const WCHAR* path) : h(CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
unsigned U16(const BYTE* p) noexcept { return p[0] | (static_cast<unsigned>(p[1]) << 8); }
DWORD U32(const BYTE* p) noexcept { return U16(p) | (static_cast<DWORD>(U16(p + 2)) << 16); }
bool ReadAt(HANDLE file, LONGLONG offset, BYTE* buffer, DWORD size) noexcept {
    LARGE_INTEGER location{};
    location.QuadPart = offset;
    DWORD read = 0;
    return SetFilePointerEx(file, location, nullptr, FILE_BEGIN) &&
        ReadFile(file, buffer, size, &read, nullptr) && read == size;
}

// Validate the appended JAR's actual central directory. No version/size/hash
// allowlist: class semantics are checked by the agent before transformation.
bool MatchesHmclArchive(LPCWSTR path) {
    File file(path);
    LARGE_INTEGER size{};
    if (file.h == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.h, &size) || size.QuadPart < 22)
        return false;
    const DWORD tailSize = static_cast<DWORD>(size.QuadPart > 65557 ? 65557 : size.QuadPart);
    std::vector<BYTE> tail(tailSize);
    if (!ReadAt(file.h, size.QuadPart - tailSize, tail.data(), tailSize)) return false;
    for (size_t end = tail.size() - 22 + 1; end-- > 0;) {
        const BYTE* record = tail.data() + end;
        if (U32(record) != 0x06054b50 || end + 22 + U16(record + 20) != tail.size()) continue;
        const unsigned entries = U16(record + 10);
        const DWORD directorySize = U32(record + 12), directoryOffset = U32(record + 16);
        const LONGLONG endOffset = size.QuadPart - tailSize + static_cast<LONGLONG>(end);
        if (U16(record + 4) || U16(record + 6) || !entries || entries == 0xffff || U16(record + 8) != entries ||
            directorySize < 46 || directorySize > 32 * 1024 * 1024 || directorySize > endOffset) return false;
        const LONGLONG start = endOffset - directorySize;
        if (directoryOffset > start) return false;
        const LONGLONG prefix = start - directoryOffset;
        std::vector<BYTE> directory(directorySize);
        if (!ReadAt(file.h, start, directory.data(), directorySize)) return false;
        size_t offset = 0;
        bool launcher = false, region = false;
        for (unsigned i = 0; i < entries; ++i) {
            if (directory.size() - offset < 46) return false;
            const BYTE* entry = directory.data() + offset;
            if (U32(entry) != 0x02014b50) return false;
            const unsigned nameSize = U16(entry + 28);
            const size_t length = 46 + nameSize + U16(entry + 30) + U16(entry + 32);
            if (length > directory.size() - offset || prefix + U32(entry + 42) >= start) return false;
            const char* name = reinterpret_cast<const char*>(entry + 46);
            constexpr char launcherName[] = "org/jackhuang/hmcl/Launcher.class";
            constexpr char regionName[] = "org/jackhuang/hmcl/util/RegionUtils.class";
            if (!(U16(entry + 8) & 1)) {
                launcher |= nameSize == sizeof(launcherName) - 1 && std::memcmp(name, launcherName, nameSize) == 0;
                region |= nameSize == sizeof(regionName) - 1 && std::memcmp(name, regionName, nameSize) == 0;
            }
            offset += length;
        }
        return offset == directory.size() && launcher && region;
    }
    return false;
}

} // namespace

bool HKR_IsHmclArchiveW(LPCWSTR path) noexcept {
    try { return MatchesHmclArchive(path); }
    catch (...) { return false; }
}

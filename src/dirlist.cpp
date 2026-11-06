#include "dirlist.h"

#include <shlwapi.h>
#include <winternl.h>

#include <algorithm>
#include <cwchar>

namespace {

// FILE_FULL_DIR_INFORMATION: name + size + dates + attributes in a single pass.
struct FullDirInfo {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    WCHAR FileName[1];
};
constexpr ULONG kFileFullDirectoryInformation = 2;
constexpr LONG kStatusNoMoreFiles = static_cast<LONG>(0x80000006);
constexpr LONG kStatusNoSuchFile = static_cast<LONG>(0xC000000F);

using PFN_NtQueryDirectoryFile = LONG(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK*, PVOID, ULONG,
                                              ULONG, BOOLEAN, UNICODE_STRING*, BOOLEAN);
using PFN_RtlNtStatusToDosError = ULONG(NTAPI*)(LONG);

struct NtApi {
    PFN_NtQueryDirectoryFile query = nullptr;
    PFN_RtlNtStatusToDosError toDos = nullptr;
    NtApi() {
        HMODULE h = GetModuleHandleW(L"ntdll.dll");
        query = reinterpret_cast<PFN_NtQueryDirectoryFile>(GetProcAddress(h, "NtQueryDirectoryFile"));
        toDos = reinterpret_cast<PFN_RtlNtStatusToDosError>(GetProcAddress(h, "RtlNtStatusToDosError"));
    }
};
const NtApi& Nt() {
    static NtApi api;
    return api;
}

bool IsDots(const wchar_t* n, size_t len) {
    return (len == 1 && n[0] == L'.') || (len == 2 && n[0] == L'.' && n[1] == L'.');
}

void Add(Listing& l, const wchar_t* name, size_t len, uint32_t attrs, uint64_t size, int64_t mtime) {
    Entry e;
    e.nameOff = static_cast<uint32_t>(l.names.size());
    e.nameLen = static_cast<uint32_t>(len);
    e.attrs = attrs;
    e.size = size;
    e.mtime = mtime;
    l.names.insert(l.names.end(), name, name + len);
    l.names.push_back(0);
    l.entries.push_back(e);
}

bool EnumNt(const std::wstring& path, Listing& out) {
    std::wstring ext = ToExtendedPath(path);
    HANDLE h = CreateFileW(ext.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out.error = GetLastError();
        return false;
    }
    // 64 KiB batches: each kernel call brings back hundreds of entries.
    alignas(8) static thread_local unsigned char buf[64 * 1024];
    IO_STATUS_BLOCK iosb{};
    BOOLEAN restart = TRUE;
    for (;;) {
        LONG st = Nt().query(h, nullptr, nullptr, nullptr, &iosb, buf, sizeof(buf), kFileFullDirectoryInformation,
                             FALSE, nullptr, restart);
        if (st == kStatusNoMoreFiles || (restart && st == kStatusNoSuchFile)) break;
        restart = FALSE;
        if (st < 0) {
            out.error = Nt().toDos(st);
            CloseHandle(h);
            return false;
        }
        unsigned char* p = buf;
        for (;;) {
            auto* fi = reinterpret_cast<FullDirInfo*>(p);
            size_t len = fi->FileNameLength / sizeof(WCHAR);
            if (!IsDots(fi->FileName, len))
                Add(out, fi->FileName, len, fi->FileAttributes, static_cast<uint64_t>(fi->EndOfFile.QuadPart),
                    fi->LastWriteTime.QuadPart);
            if (fi->NextEntryOffset == 0) break;
            p += fi->NextEntryOffset;
        }
    }
    CloseHandle(h);
    return true;
}

bool EnumWin32(const std::wstring& path, Listing& out) {
    std::wstring pattern = ToExtendedPath(path);
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return true;
        out.error = e;
        return false;
    }
    do {
        size_t len = wcslen(fd.cFileName);
        if (IsDots(fd.cFileName, len)) continue;
        uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        int64_t mtime = static_cast<int64_t>((static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                             fd.ftLastWriteTime.dwLowDateTime);
        Add(out, fd.cFileName, len, fd.dwFileAttributes, size, mtime);
    } while (FindNextFileW(h, &fd));
    DWORD e = GetLastError();
    FindClose(h);
    if (e != ERROR_NO_MORE_FILES) {
        out.error = e;
        return false;
    }
    return true;
}

bool EnumDrives(Listing& out) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t n[3] = {static_cast<wchar_t>(L'A' + i), L':', 0};
        Add(out, n, 2, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE, 0, 0);
    }
    return true;
}

}  // namespace

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

bool EnumerateDir(const std::wstring& path, EnumMethod method, Listing& out) {
    out.path = path;
    out.entries.clear();
    out.names.clear();
    out.error = 0;
    if (path.empty()) return EnumDrives(out);
    return method == EnumMethod::Nt ? EnumNt(path, out) : EnumWin32(path, out);
}

bool g_sortWithKeys = true;

void NameKeys::Build(const Listing& l) {
    constexpr DWORD flags = LCMAP_SORTKEY | NORM_IGNORECASE | SORT_DIGITSASNUMBERS;
    bytes.resize(l.entries.size() * 48 + 4096);
    at.resize(l.entries.size());
    size_t used = 0;
    for (size_t i = 0; i < l.entries.size(); ++i) {
        const Entry& e = l.entries[i];
        const int len = static_cast<int>(e.nameLen);
        int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, l.name(e), len, reinterpret_cast<LPWSTR>(bytes.data() + used),
                              static_cast<int>(bytes.size() - used), nullptr, nullptr, 0);
        if (n == 0) {
            n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, l.name(e), len, nullptr, 0, nullptr, nullptr, 0);
            bytes.resize(bytes.size() * 2 + static_cast<size_t>(n));
            n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, l.name(e), len, reinterpret_cast<LPWSTR>(bytes.data() + used),
                              static_cast<int>(bytes.size() - used), nullptr, nullptr, 0);
        }
        at[i] = static_cast<uint32_t>(used);
        used += static_cast<size_t>(n);
    }
}

void SortListing(Listing& l, SortBy by, bool asc, bool foldersFirst) {
    l.sortBy = by;
    l.sortAsc = asc;
    l.foldersFirst = foldersFirst;
    if (l.path.empty()) return;  // drives already come in order
    auto order = [&](const Entry& a, const Entry& b, auto byName) {
        bool da = a.isDir(), db = b.isDir();
        if (foldersFirst && da != db) return da;
        int c = 0;
        if (by == SortBy::Date)
            c = a.mtime < b.mtime ? -1 : (a.mtime > b.mtime ? 1 : 0);
        else if (by == SortBy::Size) {
            // mixed by size: folders (which have no size) go last
            if (da != db) return asc ? da : db;
            if (!da) c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        }
        if (c == 0) c = byName();
        return asc ? c < 0 : c > 0;
    };
    if (!g_sortWithKeys) {
        std::sort(l.entries.begin(), l.entries.end(), [&](const Entry& a, const Entry& b) {
            return order(a, b, [&] { return StrCmpLogicalW(l.name(a), l.name(b)); });
        });
        return;
    }
    NameKeys keys;
    keys.Build(l);
    std::vector<uint32_t> idx(l.entries.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<uint32_t>(i);
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
        return order(l.entries[a], l.entries[b], [&] { return keys.Compare(a, b); });
    });
    std::vector<Entry> sorted;
    sorted.reserve(idx.size());
    for (uint32_t i : idx) sorted.push_back(l.entries[i]);
    l.entries = std::move(sorted);
}

bool SameContent(const Listing& a, const Listing& b) {
    if (a.error != b.error || a.entries.size() != b.entries.size()) return false;
    for (size_t i = 0; i < a.entries.size(); ++i) {
        const Entry& x = a.entries[i];
        const Entry& y = b.entries[i];
        if (x.attrs != y.attrs || x.size != y.size || x.mtime != y.mtime || x.nameLen != y.nameLen) return false;
        if (wmemcmp(a.name(x), b.name(y), x.nameLen) != 0) return false;
    }
    return true;
}

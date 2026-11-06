#pragma once
// Folder enumeration: the core that has to be fast.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct Entry {
    uint32_t nameOff;  // offset into Listing::names
    uint32_t nameLen;  // in wchar_t, without the terminator
    uint32_t attrs;
    uint64_t size;
    int64_t mtime;  // FILETIME in UTC as an integer
    bool isDir() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};

enum class SortBy { Name, Date, Size };

// A listed folder. All names live together in a single block
// (0-terminated) to avoid one memory allocation per file.
struct Listing {
    std::wstring path;  // "" = drive list
    std::vector<Entry> entries;
    std::vector<wchar_t> names;
    DWORD error = 0;  // Win32 code if it couldn't be read
    SortBy sortBy = SortBy::Name;
    bool sortAsc = true;
    bool foldersFirst = true;
    // Applied view (app): groups (where each starts and its title) and the result cache.
    bool isView = false;
    std::vector<std::pair<uint32_t, std::wstring>> groups;
    std::vector<uint8_t> groupFolded;  // how each group starts according to the view (1 = collapsed)
    mutable std::shared_ptr<const Listing> view;
    mutable uint64_t viewSig = 0;
    const wchar_t* name(const Entry& e) const { return names.data() + e.nameOff; }
};

enum class EnumMethod { Nt, Win32 };

bool EnumerateDir(const std::wstring& path, EnumMethod method, Listing& out);
// Names in "natural" order like File Explorer. foldersFirst: folders on top; otherwise mixed.
// Sort keys by name: computed once per file, and comparing two is comparing bytes.
// They sort the same as StrCmpLogicalW, which recomputes everything on each comparison.
struct NameKeys {
    std::vector<char> bytes;
    std::vector<uint32_t> at;
    void Build(const Listing& l);
    int Compare(size_t a, size_t b) const { return std::strcmp(bytes.data() + at[a], bytes.data() + at[b]); }
};

extern bool g_sortWithKeys;  // bench: compares against direct sorting
void SortListing(Listing& l, SortBy by = SortBy::Name, bool asc = true, bool foldersFirst = true);
bool SameContent(const Listing& a, const Listing& b);
std::wstring ToExtendedPath(const std::wstring& path);  // adds \\?\ for long paths

inline double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

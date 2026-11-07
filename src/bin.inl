// The Recycle Bin as a folder. Each drive keeps what was deleted in $Recycle.Bin\<user>: the item itself as $Rxxxxxx, and
// next to it a small record $Ixxxxxx with its size, when it was deleted and where it came from. The list shows the
// original names; Join gives back the real $R path, so the preview and thumbnails work with no changes.

struct BinItem {
    std::wstring real, original;
};
std::mutex g_binM;
std::unordered_map<std::wstring, BinItem> g_binItems;  // Key(name in the list)

std::wstring VirtualJoin(const std::wstring& dir, const wchar_t* name) {
    if (IsNet(dir)) return L"\\\\" + std::wstring(name);
    std::lock_guard lk(g_binM);
    auto it = g_binItems.find(Key(name));
    return it != g_binItems.end() ? it->second.real : dir + L"\\" + name;
}

BinItem BinItemOf(const wchar_t* name) {
    std::lock_guard lk(g_binM);
    auto it = g_binItems.find(Key(name));
    return it != g_binItems.end() ? it->second : BinItem{};
}

// The record that goes with an item.
std::wstring BinRecordOf(const std::wstring& real) {
    std::wstring r = real;
    const size_t leaf = r.find_last_of(L'\\') + 1;
    if (leaf + 1 < r.size() && r[leaf] == L'$' && r[leaf + 1] == L'R') r[leaf + 1] = L'I';
    return r;
}

std::wstring BinUser() {
    static const std::wstring sid = [] {
        std::wstring out;
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
        std::vector<char> buf(n);
        if (n && GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
            wchar_t* s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
                out = s;
                LocalFree(s);
            }
        }
        CloseHandle(tok);
        return out;
    }();
    return sid;
}

// Where this user's deleted items are, one folder per drive that has them.
std::vector<std::wstring> BinDirs() {
    std::vector<std::wstring> dirs;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        std::wstring dir = std::wstring(root) + L"$Recycle.Bin\\" + BinUser();
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) dirs.push_back(std::move(dir));
    }
    return dirs;
}

// Size, date and original path, from the record. Windows 10 and later write version 2, with the length of the path.
bool BinReadRecord(const std::wstring& file, uint64_t& size, int64_t& deleted, std::wstring& original) {
    HANDLE h = CreateFileW(ToExtendedPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[24 + 4 + 2 * 4096];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 28) return false;
    uint64_t version;
    memcpy(&version, buf, 8);
    memcpy(&size, buf + 8, 8);
    memcpy(&deleted, buf + 16, 8);
    const size_t at = version >= 2 ? 28 : 24;
    size_t len = (got - at) / 2;
    if (version >= 2) {
        uint32_t n;
        memcpy(&n, buf + 24, 4);
        len = std::min<size_t>(len, n);
    }
    original.assign(reinterpret_cast<const wchar_t*>(buf + at), len);
    original.resize(wcsnlen(original.c_str(), original.size()));
    return !original.empty();
}

void BinEnumerate(Listing& out) {
    out.path = kBinPath;
    std::unordered_map<std::wstring, BinItem> items;
    std::unordered_map<std::wstring, int> repeats;
    for (const std::wstring& dir : BinDirs()) {
        Listing l;
        if (!EnumerateDir(dir, EnumMethod::Nt, l)) continue;
        std::unordered_map<std::wstring, DWORD> attrsOf;
        for (const Entry& e : l.entries) attrsOf[Key(l.name(e))] = e.attrs;
        for (const Entry& e : l.entries) {
            const wchar_t* n = l.name(e);
            if (e.nameLen < 3 || n[0] != L'$' || n[1] != L'I') continue;
            std::wstring item = n;
            item[1] = L'R';
            auto found = attrsOf.find(Key(item));
            uint64_t size = 0;
            int64_t deleted = 0;
            std::wstring original;
            if (found == attrsOf.end() || !BinReadRecord(Join(dir, n), size, deleted, original)) continue;
            // the same name deleted twice: both show as they are, told apart by an invisible mark before the extension
            std::wstring shown = LeafOf(original);
            if (const int again = repeats[Key(shown)]++) {
                const size_t dot = shown.rfind(L'.');
                shown.insert(dot == std::wstring::npos || dot == 0 ? shown.size() : dot, static_cast<size_t>(again), L'\u200B');
            }
            ListingAdd(out, shown.c_str(), shown.size(), found->second & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE), size, deleted);
            items[Key(shown)] = {Join(dir, item.c_str()), std::move(original)};
        }
    }
    std::lock_guard lk(g_binM);
    g_binItems = std::move(items);
}

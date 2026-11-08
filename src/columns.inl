// The columns besides the name: Modified and Size, and also the dimensions of images and videos, how long songs
// and videos last, how many items a folder has, and the attributes. Which ones, and in what order, come from
// [columns] show. Dimensions, duration and item counts need the file or the folder opened: a thread works them out
// for what is on screen, and they stay in memory by path and modified time. Included from app.cpp before the list.

constexpr UINT WM_APP_META = WM_APP + 15;

struct FileMeta {
    int64_t mtime = 0;
    uint32_t w = 0, h = 0;           // pixels: images and videos
    uint64_t dur = 0;                // in 100 ns units: songs and videos
    int32_t items = -1, shown = -1;  // folders: everything in it, and what isn't hidden
};
struct MetaReq {
    std::wstring path;
    int64_t mtime;
    bool dir;
};

std::mutex g_mtm;
std::condition_variable g_mtcv;
std::unordered_map<std::wstring, FileMeta> g_meta;  // key: Key(path)
std::unordered_set<std::wstring> g_metaPending;
std::vector<MetaReq> g_metaQueue;  // the last asked for is served first: what is on screen now
std::atomic<bool> g_metaDirty{false};
bool g_metaThreadUp = false;

// The Shell's own property keys (propkey.h), written out so no GUID has to be defined here.
constexpr GUID kFmtImage = {0x6444048F, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
constexpr GUID kFmtAudio = {0x64440490, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
constexpr GUID kFmtVideo = {0x64440491, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
constexpr PROPERTYKEY kPkImageW = {kFmtImage, 3}, kPkImageH = {kFmtImage, 4};    // Image.HorizontalSize / VerticalSize
constexpr PROPERTYKEY kPkVideoW = {kFmtVideo, 3}, kPkVideoH = {kFmtVideo, 4};    // Video.FrameWidth / FrameHeight
constexpr PROPERTYKEY kPkDuration = {kFmtAudio, 3};                              // Media.Duration

enum MediaKindT { MediaNone, MediaImage, MediaAudio, MediaVideo };

// What Windows takes a file to be, by its extension (the registry's PerceivedType). Main thread only.
int MediaKind(const wchar_t* ext) {
    static std::unordered_map<std::wstring, int> cache;
    if (!ext || !*ext) return MediaNone;
    const std::wstring k = Lower(ext);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    PERCEIVED t = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG f = 0;
    int r = MediaNone;
    if (SUCCEEDED(AssocGetPerceivedType(k.c_str(), &t, &f, nullptr)))
        r = t == PERCEIVED_TYPE_IMAGE ? MediaImage : t == PERCEIVED_TYPE_AUDIO ? MediaAudio : t == PERCEIVED_TYPE_VIDEO ? MediaVideo : MediaNone;
    return cache[k] = r;
}

uint64_t PropNumber(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    uint64_t out = 0;
    if (SUCCEEDED(ps->GetValue(key, &v))) {
        if (v.vt == VT_UI4) out = v.ulVal;
        else if (v.vt == VT_UI8) out = v.uhVal.QuadPart;
        else if (v.vt == VT_I4 && v.lVal > 0) out = static_cast<uint64_t>(v.lVal);
    }
    PropVariantClear(&v);
    return out;
}

// On the thread: a folder is counted, a file asks the Shell's property handler (which reads only the header).
void MetaRead(const MetaReq& r, FileMeta& m) {
    m.mtime = r.mtime;
    if (r.dir) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((ToExtendedPath(r.path) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;
        int all = 0, shown = 0;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            ++all;
            if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) ++shown;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        m.items = all;
        m.shown = shown;
        return;
    }
    ComPtr<IPropertyStore> ps;
    if (FAILED(SHGetPropertyStoreFromParsingName(r.path.c_str(), nullptr, GPS_OPENSLOWITEM, IID_PPV_ARGS(&ps)))) return;
    m.w = static_cast<uint32_t>(PropNumber(ps.Get(), kPkImageW));
    m.h = static_cast<uint32_t>(PropNumber(ps.Get(), kPkImageH));
    if (!m.w || !m.h) {
        m.w = static_cast<uint32_t>(PropNumber(ps.Get(), kPkVideoW));
        m.h = static_cast<uint32_t>(PropNumber(ps.Get(), kPkVideoH));
    }
    m.dur = PropNumber(ps.Get(), kPkDuration);
}

void MetaThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        MetaReq req;
        {
            std::unique_lock lk(g_mtm);
            g_mtcv.wait(lk, [] { return !g_metaQueue.empty(); });
            req = std::move(g_metaQueue.back());
            g_metaQueue.pop_back();
        }
        FileMeta m;
        MetaRead(req, m);
        const std::wstring key = Key(req.path);
        {
            std::lock_guard lk(g_mtm);
            g_meta[key] = m;
            g_metaPending.erase(key);
        }
        if (!g_metaDirty.exchange(true)) PostMessageW(g_hwnd, WM_APP_META, 0, 0);
    }
}

// Main thread: what is known about a file or folder. When it is missing or the item changed since, it is asked for
// (unless ask is false) and the answer comes later with WM_APP_META.
bool MetaGet(const std::wstring& path, int64_t mtime, bool dir, bool ask, FileMeta& out) {
    const std::wstring key = Key(path);
    std::lock_guard lk(g_mtm);
    auto it = g_meta.find(key);
    if (it != g_meta.end() && it->second.mtime == mtime) {
        out = it->second;
        return true;
    }
    if (!ask || g_metaPending.count(key)) return false;
    if (g_meta.size() > 50000) g_meta.clear();  // memory: it doesn't grow without end
    if (g_metaQueue.size() > 300) {             // what was left behind is no longer on screen
        for (const MetaReq& q : g_metaQueue) g_metaPending.erase(Key(q.path));
        g_metaQueue.clear();
    }
    g_metaPending.insert(key);
    g_metaQueue.push_back({path, mtime, dir});
    if (!g_metaThreadUp) {
        g_metaThreadUp = true;
        std::thread(MetaThread).detach();
    }
    g_mtcv.notify_one();
    return false;
}

// Whole seconds, cut like File Explorer does (not rounded): 3:05.4 is 3:05.
std::wstring FormatDuration(uint64_t d) {
    const unsigned long long s = d / 10000000;
    wchar_t b[40];
    if (s >= 3600) swprintf(b, 40, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else swprintf(b, 40, L"%llu:%02llu", s / 60, s % 60);
    return b;
}

// The letters File Explorer uses, in its order, and L for links (symbolic links and junctions).
std::wstring AttrLetters(DWORD a) {
    std::wstring s;
    if (a & FILE_ATTRIBUTE_READONLY) s += L'R';
    if (a & FILE_ATTRIBUTE_HIDDEN) s += L'H';
    if (a & FILE_ATTRIBUTE_SYSTEM) s += L'S';
    if (a & FILE_ATTRIBUTE_ARCHIVE) s += L'A';
    if (a & FILE_ATTRIBUTE_COMPRESSED) s += L'C';
    if (a & FILE_ATTRIBUTE_ENCRYPTED) s += L'E';
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) s += L'L';
    return s;
}

// What a column shows for a row. faint: still being worked out (a folder's size while it is counted).
std::wstring CellText(int kind, const Entry& e, const std::wstring& full, bool drives, bool& faint, bool ask = true) {
    wchar_t buf[64];
    switch (kind) {
        case ColDate:
            if (!e.mtime) return {};
            FormatDate(e.mtime, buf, 64);
            return buf;
        case ColSize: {
            if (!e.isDir()) {
                FormatSize(e.size, buf, 64);
                return buf;
            }
            DirSize ds;
            if (drives || !g_dsAny || !DirSizeOf(Key(Normalize(full)), ds)) return {};
            std::wstring s;
            if (ds.done || ds.bytes) {
                FormatSize(ds.bytes, buf, 64);
                s = buf;
            }
            if (!ds.done) {
                s += L"…";
                faint = true;
            }
            return s;
        }
        case ColAttrs: return drives ? std::wstring() : AttrLetters(e.attrs);
        case ColItems: {
            FileMeta m;
            if (!e.isDir() || drives || !MetaGet(full, e.mtime, true, ask, m)) return {};
            const int n = g_cfg.showHidden ? m.items : m.shown;
            return n < 0 ? std::wstring() : Thousands(n);
        }
        case ColDims:
        case ColDur: {
            if (e.isDir() || drives || OnlineOnly(e.attrs)) return {};  // online only: reading it would download it
            const int mk = MediaKind(PathFindExtensionW(full.c_str()));
            if (mk == MediaNone || (kind == ColDims && mk == MediaAudio) || (kind == ColDur && mk == MediaImage)) return {};
            FileMeta m;
            if (!MetaGet(full, e.mtime, false, ask, m)) return {};
            if (kind == ColDur) return m.dur ? FormatDuration(m.dur) : std::wstring();
            return m.w && m.h ? std::to_wstring(m.w) + L" × " + std::to_wstring(m.h) : std::wstring();
        }
    }
    return {};
}

// A column's title: in the Recycle Bin, the date is when it was deleted.
std::wstring ColLabel(int kind, int pane) {
    return kind == ColDate && IsBin(PT(pane).path) ? L"Deleted" : kColDefs[kind].label;
}

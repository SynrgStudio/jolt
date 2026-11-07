// Thumbnails, by path and size: a real one from the Shell (photos, videos, PDF, Office...) or, failing
// that, the file's icon at that size. Produced on a thread, kept in memory for drawing and on disk in
// %LOCALAPPDATA%\Jolt\thumbs (as PNG, named by path, size and modified time) so the second time is
// instant. The preview and the grid view both come here. Included from app.cpp before preview.inl.

constexpr UINT WM_APP_THUMB = WM_APP + 11;

struct Thumb {
    ComPtr<IWICBitmap> wic;   // what the thread produced
    ComPtr<ID2D1Bitmap> d2d;  // for drawing; made on first use, dropped with the render target
    UINT w = 0, h = 0;
    bool isIcon = false;  // the icon, not a real thumbnail
    bool failed = false;
    int64_t mtime = 0;
};
struct ThumbReq {
    std::wstring path;
    int size;
    int64_t mtime;
};

std::mutex g_thm;
std::unordered_map<std::wstring, std::shared_ptr<Thumb>> g_thumbs;  // key: lowercase path | size
std::unordered_set<std::wstring> g_thumbPending;
std::vector<ThumbReq> g_thumbQueue;  // the last asked for is served first: what is on screen now
std::condition_variable g_thcv;
std::atomic<bool> g_thumbDirty{false};
bool g_thumbThreadUp = false;

std::wstring ThumbKeyOf(const std::wstring& path, int size) { return Key(path) + L"|" + std::to_wstring(size); }

uint64_t ThumbHash(const std::wstring& s) {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : s) h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ull;
    return h;
}

std::wstring ThumbDir() {
    static std::wstring dir;
    static std::once_flag once;
    std::call_once(once, [] {
        dir = LocalDataDir() + L"\\thumbs";
        CreateDirectoryW(dir.c_str(), nullptr);
    });
    return dir;
}

std::wstring ThumbFile(const std::wstring& path, int size, int64_t mtime) {
    wchar_t name[80];
    swprintf(name, 80, L"%016llx-%llx-%d.png", static_cast<unsigned long long>(ThumbHash(Key(path))), static_cast<unsigned long long>(mtime), size);
    return ThumbDir() + L"\\" + name;
}

// A GDI bitmap from the Shell as a premultiplied WIC bitmap. Icons come with straight alpha (a channel
// above the alpha gives it away) and Direct2D wants it premultiplied, or the soft edges turn into specks.
bool HBitmapToWic(IWICImagingFactory* wic, HBITMAP hb, ComPtr<IWICBitmap>& out, UINT& w, UINT& h) {
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    if (bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    w = bm.bmWidth;
    h = std::abs(bm.bmHeight);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(nullptr);
    const bool got = GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS) == static_cast<int>(h);
    ReleaseDC(nullptr, dc);
    if (!got) return false;
    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        anyAlpha |= px[i + 3] != 0;
        straight |= px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3];
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i + 3 < px.size(); i += 4)
            for (size_t c = 0; c < 3; ++c) px[i + c] = static_cast<uint8_t>((px[i + c] * px[i + 3] + 127) / 255);
    }
    return SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, static_cast<UINT>(px.size()), px.data(), &out));
}

bool ThumbLoadPng(IWICImagingFactory* wic, const std::wstring& file, ComPtr<IWICBitmap>& out, UINT& w, UINT& h) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &out)))
        return false;
    out->GetSize(&w, &h);
    return w > 0 && h > 0;
}

bool ThumbSavePng(IWICImagingFactory* wic, IWICBitmap* bmp, const std::wstring& file) {
    const std::wstring tmp = file + L".tmp";
    bool ok;
    {  // the encoder and the frame keep the file open until they are gone
        ComPtr<IWICStream> st;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fr;
        UINT w, h;
        bmp->GetSize(&w, &h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) &&
             SUCCEEDED(fr->SetPixelFormat(&fmt)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ok) ok = MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

int64_t ThumbMtimeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(ToExtendedPath(path).c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

// The thumbnail of a file at a size, from the disk cache or from the Shell (and then into the cache).
// Any thread with COM initialized. mtime 0: read it from the file.
bool ThumbProduce(IWICImagingFactory* wic, const std::wstring& path, int size, int64_t mtime, ComPtr<IWICBitmap>& out, UINT& w, UINT& h, bool& isIcon) {
    if (!mtime) mtime = ThumbMtimeOf(path);
    const std::wstring file = ThumbFile(path, size, mtime);
    if (mtime && ThumbLoadPng(wic, file, out, w, h)) {
        // the icon ones are marked by a trailing "i" file next to them: cheap and keeps one format
        isIcon = PathExists(file + L".i");
        return true;
    }
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    // A real thumbnail if there is one; otherwise the icon at its largest real size (asking for more
    // makes Windows stretch it).
    isIcon = false;
    // small sizes (the grid) fit the box; big ones (the preview) take the largest the Shell has
    const DWORD fit = size > 256 ? SIIGBF_BIGGERSIZEOK : SIIGBF_RESIZETOFIT;
    if (FAILED(f->GetImage(SIZE{size, size}, fit | SIIGBF_THUMBNAILONLY, &hb)) || !hb) {
        hb = nullptr;
        const int is = std::min(size, 256);
        if (FAILED(f->GetImage(SIZE{is, is}, SIIGBF_ICONONLY, &hb)) || !hb) return false;
        isIcon = true;
    }
    const bool ok = HBitmapToWic(wic, hb, out, w, h);
    DeleteObject(hb);
    if (ok && mtime && ThumbSavePng(wic, out.Get(), file) && isIcon) {
        HANDLE m = CreateFileW((file + L".i").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
    }
    return ok;
}

// Cache files not touched for 60 days go, once per session.
void ThumbPurge() {
    const std::wstring dir = ThumbDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const int64_t nowT = (static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime, old = 60ll * 864000000000ll;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const int64_t at = (static_cast<int64_t>(fd.ftLastAccessTime.dwHighDateTime) << 32) | fd.ftLastAccessTime.dwLowDateTime;
        if (nowT - at > old) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void ThumbThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    ThumbPurge();
    for (;;) {
        ThumbReq req;
        {
            std::unique_lock lk(g_thm);
            g_thcv.wait(lk, [] { return !g_thumbQueue.empty(); });
            req = std::move(g_thumbQueue.back());
            g_thumbQueue.pop_back();
        }
        auto t = std::make_shared<Thumb>();
        t->mtime = req.mtime;
        if (!wic || !ThumbProduce(wic.Get(), req.path, req.size, req.mtime, t->wic, t->w, t->h, t->isIcon)) t->failed = true;
        const std::wstring key = ThumbKeyOf(req.path, req.size);
        {
            std::lock_guard lk(g_thm);
            g_thumbs[key] = std::move(t);
            g_thumbPending.erase(key);
        }
        if (!g_thumbDirty.exchange(true)) PostMessageW(g_hwnd, WM_APP_THUMB, 0, 0);
    }
}

// Main thread: the thumbnail if it is ready (null while loading, or after a failure), asking for it when
// it is missing or the file changed since.
Thumb* ThumbGet(const std::wstring& path, int size, int64_t mtime) {
    const std::wstring key = ThumbKeyOf(path, size);
    std::lock_guard lk(g_thm);
    auto it = g_thumbs.find(key);
    if (it != g_thumbs.end() && it->second->mtime == mtime) return it->second->failed ? nullptr : it->second.get();
    if (g_thumbPending.count(key)) return nullptr;
    if (g_thumbQueue.size() > 300) {  // what was left behind is no longer on screen
        for (const ThumbReq& q : g_thumbQueue) g_thumbPending.erase(ThumbKeyOf(q.path, q.size));
        g_thumbQueue.clear();
    }
    g_thumbPending.insert(key);
    g_thumbQueue.push_back({path, size, mtime});
    if (!g_thumbThreadUp) {
        g_thumbThreadUp = true;
        std::thread(ThumbThread).detach();
    }
    g_thcv.notify_one();
    return nullptr;
}

// Is it in memory already? (no request)
bool ThumbReady(const std::wstring& path, int size, int64_t mtime) {
    std::lock_guard lk(g_thm);
    auto it = g_thumbs.find(ThumbKeyOf(path, size));
    return it != g_thumbs.end() && it->second->mtime == mtime && !it->second->failed;
}

ID2D1Bitmap* ThumbBitmap(Thumb& t) {
    if (!t.d2d && t.wic && g_rt) g_rt->CreateBitmapFromWicBitmap(t.wic.Get(), nullptr, &t.d2d);
    return t.d2d.Get();
}

// The render target is gone: the D2D bitmaps with it (the WIC ones stay).
void ThumbsReleaseBitmaps() {
    std::lock_guard lk(g_thm);
    for (auto& [k, t] : g_thumbs) t->d2d.Reset();
}

// Memory: keep the map from growing without end (the disk cache stays).
void ThumbsTrim() {
    std::lock_guard lk(g_thm);
    if (g_thumbs.size() > 3000) g_thumbs.clear();
}

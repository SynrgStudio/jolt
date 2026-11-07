// Preview (Space): right-hand column showing what's under the cursor.
// Images via WIC (scaled and rotated per EXIF), text/code, folder contents (from the listing's
// memory) and, for everything else, the Shell thumbnail (PDF, videos, Office...). It all loads on
// a separate thread and only once the cursor stops: moving through the list doesn't slow down.
// Included from app.cpp after palette.inl.

constexpr UINT WM_APP_PREVIEW = WM_APP + 6;
constexpr UINT_PTR kTimerPreview = 6;

struct PreviewResult {
    uint64_t gen = 0;
    std::wstring path;
    int kind = 0;  // 0 nothing, 1 image, 2 text, 3 not on this PC
    ComPtr<IWICBitmap> bmp;
    UINT imgW = 0, imgH = 0;  // original size (already rotated)
    bool thumb = false;       // Shell thumbnail or icon (not enlarged)
    std::wstring text;
    bool truncated = false;
    std::wstring typeName;
};

struct PreviewState {
    std::wstring want;  // what's under the cursor
    std::wstring path;  // what was requested/is shown
    uint64_t gen = 0;
    std::unique_ptr<PreviewResult> res;
    ComPtr<ID2D1Bitmap> d2d;
    std::vector<std::pair<size_t, size_t>> lines;  // (start, length) in res->text
    std::vector<Span> spans;                       // syntax highlighting, sorted by start
    float scroll = 0;
    bool loading = false;
    bool isDir = false;  // the shown item is a folder
    D2D1_RECT_F box{};
} g_pv;

// Type names already seen, by extension: moving through the list shows them without waiting for the load.
std::unordered_map<std::wstring, std::wstring> g_pvTypes;
std::wstring PvExtKey(const std::wstring& path) { return Key(ExtOf(path.c_str(), path.size())); }

std::mutex g_pvm;
std::condition_variable g_pvcv;
std::wstring g_pvReq;
uint64_t g_pvReqGen = 0;

bool PvIsImage(const std::wstring& ext) {
    static const wchar_t* k[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".gif", L".tif", L".tiff",
                                 L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp", L".dds"};
    for (auto e : k)
        if (ext == e) return true;
    return false;
}
bool PvIsBinary(const std::wstring& ext) {
    static const wchar_t* k[] = {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".zip",
                                 L".7z", L".rar", L".exe", L".dll", L".msi", L".iso", L".mp4", L".mkv", L".avi",
                                 L".mov", L".wmv", L".webm", L".mp3", L".flac", L".wav", L".ogg", L".m4a", L".psd",
                                 L".ttf", L".otf", L".lnk", L".blend", L".fbx", L".stl", L".3mf"};
    for (auto e : k)
        if (ext == e) return true;
    return false;
}

bool PvImage(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    GUID container{};
    UINT frames = 1;
    if (SUCCEEDED(dec->GetContainerFormat(&container)) && container == GUID_ContainerFormatIco &&
        SUCCEEDED(dec->GetFrameCount(&frames))) {
        for (UINT i = 1; i < frames; ++i) {  // an icon stores several sizes: the largest one looks best
            ComPtr<IWICBitmapFrameDecode> other;
            UINT ow = 0, oh = 0;
            if (SUCCEEDED(dec->GetFrame(i, &other)) && SUCCEEDED(other->GetSize(&ow, &oh)) && ow > w) {
                frame = other;
                w = ow;
                h = oh;
            }
        }
    }
    if (!w || !h) return false;
    // EXIF orientation (phone photos)
    USHORT orient = 1;
    ComPtr<IWICMetadataQueryReader> q;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&q))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v)) ||
            SUCCEEDED(q->GetMetadataByName(L"/ifd/{ushort=274}", &v))) {
            if (v.vt == VT_UI2) orient = v.uiVal;
        }
        PropVariantClear(&v);
    }
    ComPtr<IWICBitmapSource> src = frame;
    const UINT maxSide = 2048;
    if (w > maxSide || h > maxSide) {
        double s = static_cast<double>(maxSide) / std::max(w, h);
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(src.Get(), std::max(1u, static_cast<UINT>(w * s)), std::max(1u, static_cast<UINT>(h * s)),
                                     WICBitmapInterpolationModeFant)))
            src = sc;
    }
    WICBitmapTransformOptions tr = WICBitmapTransformRotate0;
    switch (orient) {
        case 2: tr = WICBitmapTransformFlipHorizontal; break;
        case 3: tr = WICBitmapTransformRotate180; break;
        case 4: tr = WICBitmapTransformFlipVertical; break;
        case 5: tr = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
        case 6: tr = WICBitmapTransformRotate90; break;
        case 7: tr = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
        case 8: tr = WICBitmapTransformRotate270; break;
    }
    if (tr != WICBitmapTransformRotate0) {
        // the rotator needs the pixels in memory
        ComPtr<IWICBitmap> mem;
        ComPtr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(wic->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &mem)) &&
            SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(mem.Get(), tr)))
            src = rot;
        if (orient >= 5) std::swap(w, h);
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;
    if (FAILED(wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &r.bmp))) return false;
    r.kind = 1;
    r.imgW = w;
    r.imgH = h;
    return true;
}

// The Shell's thumbnail or icon, through the thumbnail cache (thumbs.inl): instant the second time.
bool PvThumb(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r) {
    UINT w = 0, h = 0;
    bool isIcon = false;
    if (!ThumbProduce(wic, path, 512, 0, r.bmp, w, h, isIcon)) return false;
    r.kind = 1;
    r.thumb = true;
    r.imgW = w;
    r.imgH = h;
    return true;
}

bool PvText(const std::wstring& path, PreviewResult& r) {
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD kMax = 256 * 1024;
    std::vector<char> buf(kMax);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf.data(), kMax, &got, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!ok) return false;
    buf.resize(got);
    r.truncated = static_cast<uint64_t>(size.QuadPart) > got;
    std::wstring w;
    if (got >= 2 && static_cast<uint8_t>(buf[0]) == 0xFF && static_cast<uint8_t>(buf[1]) == 0xFE) {
        w.assign(reinterpret_cast<const wchar_t*>(buf.data() + 2), (got - 2) / 2);
    } else {
        size_t start = got >= 3 && static_cast<uint8_t>(buf[0]) == 0xEF && static_cast<uint8_t>(buf[1]) == 0xBB &&
                               static_cast<uint8_t>(buf[2]) == 0xBF ? 3 : 0;
        // binary: there are null bytes near the start
        for (size_t i = start; i < std::min<size_t>(got, 8192); ++i)
            if (buf[i] == 0) return false;
        size_t len = got - start;
        if (r.truncated) {  // cut at a line end so a character isn't split
            size_t nl = std::string_view(buf.data() + start, len).find_last_of('\n');
            if (nl != std::string_view::npos) len = nl + 1;
        }
        const char* p = buf.data() + start;
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, static_cast<int>(len), nullptr, 0);
        UINT cp = CP_UTF8;
        if (n <= 0 && len) {
            cp = CP_ACP;
            n = MultiByteToWideChar(cp, 0, p, static_cast<int>(len), nullptr, 0);
        }
        w.resize(std::max(0, n));
        if (n > 0) MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, p, static_cast<int>(len), w.data(), n);
    }
    // tabs to 4 spaces, no carriage returns
    r.text.reserve(w.size());
    size_t col = 0;
    for (wchar_t c : w) {
        if (c == L'\r') continue;
        if (c == L'\t') {
            size_t sp = 4 - col % 4;
            r.text.append(sp, L' ');
            col += sp;
            continue;
        }
        r.text += c;
        col = c == L'\n' ? 0 : col + 1;
    }
    r.kind = 2;
    return true;
}

void PreviewThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    uint64_t seen = 0;
    for (;;) {
        std::wstring path;
        uint64_t gen;
        {
            std::unique_lock lk(g_pvm);
            g_pvcv.wait(lk, [&] { return g_pvReqGen != seen; });
            path = g_pvReq;
            gen = seen = g_pvReqGen;
        }
        auto r = std::make_unique<PreviewResult>();
        r->gen = gen;
        r->path = path;
        SHFILEINFOW sfi{};
        const bool online = OnlineOnlyPath(path);
        if (SHGetFileInfoW(path.c_str(), online ? FILE_ATTRIBUTE_NORMAL : 0, &sfi, sizeof(sfi), SHGFI_TYPENAME | (online ? SHGFI_USEFILEATTRIBUTES : 0)))
            r->typeName = sfi.szTypeName;
        std::wstring ext = ExtOf(path.c_str(), path.size());
        if (online) {  // a thumbnail Windows already has, at most: reading it would download it
            if (!wic || !PvThumb(wic.Get(), path, *r)) r->kind = 3;
        } else {
            bool done = wic && PvIsImage(ext) && PvImage(wic.Get(), path, *r);
            if (!done && !PvIsBinary(ext)) done = PvText(path, *r);
            if (!done && wic) PvThumb(wic.Get(), path, *r);
        }
        PreviewResult* raw = r.release();
        if (!PostMessageW(g_hwnd, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }
}

// What's under the active tab's cursor ("" = nothing).
std::wstring PreviewTarget() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return L"";
    return RowPath(t, t.sel);
}

// Called a little while after the cursor stops moving.
void PreviewLoad() {
    if (!g_preview || g_pv.want == g_pv.path) return;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(PreviewThread).detach(); });
    g_pv.path = g_pv.want;
    g_pv.res.reset();
    g_pv.d2d.Reset();
    g_pv.lines.clear();
    g_pv.scroll = 0;
    g_pv.loading = false;
    if (g_pv.path.empty()) {
        Invalidate();
        return;
    }
    DWORD a = GetFileAttributesW(ToExtendedPath(g_pv.path).c_str());
    g_pv.isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    if (g_pv.isDir) {
        if (!g_cache.count(Key(g_pv.path))) Enqueue(g_pv.path, JobKind::Revalidate);
        Invalidate();
        return;
    }
    g_pv.loading = true;
    {
        std::lock_guard lk(g_pvm);
        g_pvReq = g_pv.path;
        g_pv.gen = ++g_pvReqGen;
    }
    g_pvcv.notify_one();
    Invalidate();
}

void PreviewApply(PreviewResult* raw) {
    std::unique_ptr<PreviewResult> r(raw);
    if (r->gen != g_pv.gen || r->path != g_pv.path) return;
    g_pv.loading = false;
    g_pv.d2d.Reset();
    g_pv.lines.clear();
    g_pv.spans.clear();
    if (r->kind == 2) {
        // tabs become four spaces: the mono layout and the wrap arithmetic then agree
        for (size_t p = r->text.find(L'\t'); p != std::wstring::npos; p = r->text.find(L'\t', p + 4)) r->text.replace(p, 1, L"    ");
        if (const Lang* l = LangFor(r->path)) g_pv.spans = Highlight(r->text, *l);
        size_t start = 0;
        const std::wstring& s = r->text;
        while (start <= s.size() && g_pv.lines.size() < 20000) {
            size_t nl = s.find(L'\n', start);
            size_t end = nl == std::wstring::npos ? s.size() : nl;
            g_pv.lines.push_back({start, end - start});
            if (nl == std::wstring::npos) break;
            start = nl + 1;
        }
    }
    if (!r->typeName.empty()) g_pvTypes[PvExtKey(r->path)] = r->typeName;
    g_pv.res = std::move(r);
    Invalidate();
}

void PreviewToggle() {
    g_preview = !g_preview;
    g_pv.want.clear();
    g_pv.path = L"\x01";  // forces a load
    for (auto& tp : g_tabs) ClampScroll(*tp);
    LayoutEdit();
    LayoutRename();
    Invalidate();
    TouchSession();
}

void PreviewWheel(int delta) {
    g_pv.scroll = std::max(0.f, g_pv.scroll - delta * 3 * 16.f);
    Invalidate();
}

// A "label ...... value" line in the info block.
void PvInfoLine(const wchar_t* label, const std::wstring& value, float x0, float x1, float& y) {
    Color(cFaint);
    Text(label, wcslen(label), g_fmtName.Get(), x0, y, x0 + 90, y + 20);
    Color(0xC9C9D1);
    Text(value, g_fmtRight.Get(), x0 + 90, y, x1, y + 20);
    y += 20;
}

void DrawPreview(float W, float H) {
    if (!g_preview) return;
    const float pw = PreviewW(), x0 = W - pw, y0 = kTabH + kToolH, y1 = H - kStatus;
    Fill(D2D1::RectF(x0, y0, W, y1), cSide);
    // left edge: drag to change the width (double click: back to the default)
    Fill(D2D1::RectF(x0, y0, x0 + 1, y1), g_pvDrag || IsHover(HitKind::PreviewEdge, 0) ? cDim : cBorder);
    g_hits.push_back({D2D1::RectF(x0 - 3, y0, x0 + 4, y1), HitKind::PreviewEdge, 0});
    // request the new item once the cursor stops
    std::wstring want = PreviewTarget();
    if (want != g_pv.want) {
        g_pv.want = want;
        SetTimer(g_hwnd, kTimerPreview, g_pv.path.empty() || g_pv.path == L"\x01" ? 1 : 60, nullptr);
    }
    const float pad = 14, L = x0 + pad, R = W - pad;
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) {
        Color(cFaint);
        Text(L"Nothing selected", g_fmtCenter.Get(), x0, y0, W, y0 + 80);
        return;
    }
    // Info (bottom): straight from the cursor row, without waiting for the content to load. Every value has a
    // fixed slot and the block a fixed height, so a value that arrives late doesn't move anything.
    const std::wstring rowPath = RowPath(t, t.sel);
    const bool rowMatches = Key(rowPath) == Key(g_pv.path);
    const Entry& e = EntryAt(t, t.sel);
    wchar_t buf[64];
    std::wstring modified;
    if (e.mtime) {
        FormatDate(e.mtime, buf, 64);
        modified = buf;
    }
    constexpr int kInfoRows = 5;
    std::pair<const wchar_t*, std::wstring> info[kInfoRows];
    if (e.isDir()) {
        std::wstring contains, size;
        auto list = g_cache.find(Key(rowPath));
        if (list != g_cache.end() && !list->second->error)
            contains = Thousands(static_cast<int>(list->second->entries.size())) + L" items";
        DirSize ds;
        if (DirSizeOf(Key(rowPath), ds) && (ds.done || ds.bytes)) {
            FormatSize(ds.bytes, buf, 64);
            size = std::wstring(buf) + (ds.done ? L"" : L"…") + L"   ·   " + Thousands(static_cast<int>(ds.files)) + L" files";
        }
        info[0] = {L"Type", L"Folder"};
        info[1] = {L"Modified", modified};
        info[2] = {L"Contains", contains};
        info[3] = {L"Size", size};
    } else {
        std::wstring type = rowMatches && g_pv.res ? g_pv.res->typeName : L"";
        if (type.empty()) {
            auto known = g_pvTypes.find(PvExtKey(rowPath));
            if (known != g_pvTypes.end()) type = known->second;
        }
        FormatSize(e.size, buf, 64);
        info[0] = {L"Type", type};
        info[1] = {L"Size", std::wstring(buf) + L"  (" + Thousands(static_cast<int>(std::min<uint64_t>(e.size, INT_MAX))) + L" bytes)"};
        info[2] = {L"Modified", modified};
        if (rowMatches && g_pv.res && g_pv.res->kind == 1 && !g_pv.res->thumb)
            info[3] = {L"Dimensions", std::to_wstring(g_pv.res->imgW) + L" × " + std::to_wstring(g_pv.res->imgH)};
        else if (OnlineOnly(e.attrs)) info[3] = {L"Storage", L"Online only"};
        else if (e.attrs & FILE_ATTRIBUTE_PINNED) info[3] = {L"Storage", L"Always on this PC"};
    }
    if (const auto* tags = TagsOf(rowPath)) {
        std::wstring all;
        for (const std::wstring& x : *tags) all += (all.empty() ? L"" : L", ") + x;
        info[4] = {L"Tags", all};
    }
    const float infoH = 34 + kInfoRows * 20 + 10;
    const float top = y0 + pad, bot = std::max(top + 60, y1 - infoH - 6);
    const D2D1_RECT_F box = D2D1::RectF(L, top, R, bot);
    g_pv.box = box;
    // content: what has loaded, which for a moment can still be the previous item
    g_rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    auto cached = g_pv.isDir ? g_cache.find(Key(g_pv.path)) : g_cache.end();
    if (g_pv.path.empty() || g_pv.path == L"\x01") {
        // nothing requested yet (it takes a frame)
    } else if (g_pv.isDir) {
        if (cached == g_cache.end()) {
            auto note = g_folderNotes.find(Key(g_pv.path));
            const bool why = note != g_folderNotes.end();
            Color(why ? cDim : cFaint);
            Text(why ? FolderWhyLong(note->second) : L"Loading…", g_fmtCenter.Get(), L, top, R, top + 60);
        } else {
            auto l = Prepared(cached->second);
            const float rh = 22;
            int first = static_cast<int>(g_pv.scroll / rh);
            if (l->entries.empty()) {
                Color(cFaint);
                Text(L"Empty folder", g_fmtCenter.Get(), L, top, R, top + 60);
            }
            for (int i = std::max(0, first); i < static_cast<int>(l->entries.size()); ++i) {
                float y = top + i * rh - g_pv.scroll;
                if (y > bot) break;
                const Entry& ce = l->entries[i];
                const wchar_t* nm = l->name(ce);
                DrawIcon(Join(l->path, nm), &ce, nm, ce.nameLen, false, L + 2, y + (rh - kIcon) / 2, 1.f);
                Color(0xC9C9D1);
                Text(nm, ce.nameLen, g_fmtName.Get(), L + 26, y, R, y + rh);
            }
        }
    } else if (g_pv.res && g_pv.res->kind == 1 && g_pv.res->bmp) {
        if (!g_pv.d2d) g_rt->CreateBitmapFromWicBitmap(g_pv.res->bmp.Get(), nullptr, &g_pv.d2d);
        if (g_pv.d2d) {
            D2D1_SIZE_U px = g_pv.d2d->GetPixelSize();
            float scale = 96.f / g_dpi;  // pixels -> DIP
            float iw = px.width * scale, ih = px.height * scale;
            float bw = box.right - box.left, bh = box.bottom - box.top;
            float k = std::min(bw / iw, bh / ih);
            if (g_pv.res->thumb || k > 1) k = std::min(k, 1.f);  // don't enlarge
            float dw = iw * k, dh = ih * k;
            const float snap = g_dpi / 96.f;  // whole pixels: at its real size it isn't resampled
            float dx = std::round((box.left + (bw - dw) / 2) * snap) / snap;
            float dy = std::round((box.top + (bh - dh) / 2) * snap) / snap;
            g_rt->DrawBitmap(g_pv.d2d.Get(), D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    } else if (g_pv.res && g_pv.res->kind == 3) {
        const float mid = (top + bot) / 2;
        Color(cFaint);
        Glyph(gCloud, g_fmtIcon.Get(), L, mid - 44, R, mid - 18);
        Color(cDim);
        Text(L"Online only", g_fmtCenter.Get(), L, mid - 18, R, mid + 2);
        Color(cFaint);
        Text(L"Open it to download it", g_fmtCenter.Get(), L, mid + 2, R, mid + 22);
    } else if (g_pv.res && g_pv.res->kind == 2) {
        const float lh = 16;
        const std::wstring& s = g_pv.res->text;
        const int nl = static_cast<int>(g_pv.lines.size());
        const int digits = static_cast<int>(std::to_wstring(std::max(1, nl)).size());
        const float gutter = digits * 7.5f + 10;
        // wrapping: the mono font makes a line's rows len / columns, so the scroll stays in pixels
        static float charW = 0;
        static IDWriteTextFormat* charWFor = nullptr;
        if (charWFor != g_fmtMono.Get()) {
            charW = std::max(1.f, MeasureW(L"MMMMMMMMMM", g_fmtMono.Get()) / 10);
            charWFor = g_fmtMono.Get();
        }
        const bool wrap = g_cfg.previewWrap;
        const int cols = std::max(8, static_cast<int>((R - 6 - L - gutter) / charW));
        const float layW = wrap ? cols * charW + 0.5f : 20000.f;
        auto rowsOf = [&](int i) { return wrap ? std::max(1, (static_cast<int>(std::min<size_t>(g_pv.lines[i].second, 4000)) + cols - 1) / cols) : 1; };
        float total = 0;
        for (int i = 0; i < nl; ++i) total += rowsOf(i) * lh;
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, total - (bot - top)));
        if (s.empty()) {
            Color(cFaint);
            Text(L"Empty file", g_fmtCenter.Get(), L, top, R, top + 60);
        }
        // one brush per token class, for this render target
        static ComPtr<ID2D1SolidColorBrush> tokBrush[11];
        static ID2D1RenderTarget* tokRt = nullptr;
        if (tokRt != g_rt.Get()) {
            for (auto& b : tokBrush) b.Reset();
            tokRt = g_rt.Get();
        }
        auto brushFor = [&](Tok t) -> ID2D1SolidColorBrush* {
            auto& b = tokBrush[static_cast<int>(t)];
            if (!b) g_rt->CreateSolidColorBrush(D2D1::ColorF(TokColor(t)), &b);
            return b.Get();
        };
        float y = top - g_pv.scroll;
        int i = 0;
        for (; i < nl && y + rowsOf(i) * lh <= top; ++i) y += rowsOf(i) * lh;  // skip what is above
        for (; i < nl; ++i) {
            const float rows = static_cast<float>(rowsOf(i));
            if (y > bot) break;
            wchar_t num[16];
            swprintf(num, 16, L"%d", i + 1);
            Color(cFaint);
            Text(num, wcslen(num), g_fmtMonoR.Get(), L, y, L + gutter - 10, y + lh);
            auto [off, len] = g_pv.lines[i];
            len = std::min<size_t>(len, 4000);
            ComPtr<IDWriteTextLayout> lay;
            if (len && SUCCEEDED(g_dw->CreateTextLayout(s.c_str() + off, static_cast<UINT32>(len), wrap ? g_fmtMonoWrap.Get() : g_fmtMono.Get(), layW, rows * lh, &lay))) {
                // the spans touching this line (they are sorted by start; one may begin before it)
                auto it = std::lower_bound(g_pv.spans.begin(), g_pv.spans.end(), off, [](const Span& sp, size_t o) { return sp.start + sp.len <= o; });
                for (; it != g_pv.spans.end() && it->start < off + len; ++it) {
                    const size_t a = std::max<size_t>(it->start, off), b = std::min<size_t>(it->start + it->len, off + len);
                    if (b <= a) continue;
                    const DWRITE_TEXT_RANGE range{static_cast<UINT32>(a - off), static_cast<UINT32>(b - a)};
                    lay->SetDrawingEffect(brushFor(it->cls), range);
                    if (it->cls == Tok::Heading) lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
                }
                Color(0xD4D4DA);
                g_rt->DrawTextLayout(D2D1::Point2F(L + gutter, y), lay.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            y += rows * lh;
        }
    } else if (g_pv.loading) {
        // no flicker: "loading" isn't shown (it almost always takes less than a frame)
    } else {
        DrawIcon(g_pv.path, nullptr, nullptr, 0, false, (L + R) / 2 - kIcon / 2, top + 30, 1.f);
    }
    g_rt->PopAxisAlignedClip();
    // info
    float y = bot + 10;
    HLine(L, R, y - 4);
    const std::wstring name = RowName(t, t.sel);
    Color(cText);
    Text(name, g_fmtNameBold.Get(), L, y, R, y + 26);
    y += 30;
    for (auto& [label, value] : info) {
        if (value.empty()) {
            y += 20;
        } else if (wcscmp(label, L"Tags") == 0) {  // the tags as labels, not text
            Color(cFaint);
            Text(label, wcslen(label), g_fmtName.Get(), L, y, L + 90, y + 20);
            float xe;
            DrawTagLabels(rowPath, L + 90, y + 10, R, 1.f, xe);
            y += 20;
        } else {
            PvInfoLine(label, value, L, R, y);
        }
    }
}

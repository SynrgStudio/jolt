// Vista previa (Espacio): columna a la derecha con lo que está bajo el cursor.
// Imágenes con WIC (escaladas y rotadas según EXIF), texto/código, contenido de carpetas (desde la
// memoria del listado) y, para el resto, la miniatura del Shell (PDF, videos, Office...). Todo se
// carga en un hilo aparte y solo cuando el cursor se queda quieto: moverse por la lista no se frena.
// Se incluye desde app.cpp después de palette.inl.

constexpr UINT WM_APP_PREVIEW = WM_APP + 6;
constexpr UINT_PTR kTimerPreview = 6;

struct PreviewResult {
    uint64_t gen = 0;
    std::wstring path;
    int kind = 0;  // 0 nada, 1 imagen, 2 texto
    ComPtr<IWICBitmap> bmp;
    UINT imgW = 0, imgH = 0;  // tamaño original (ya rotado)
    bool thumb = false;       // miniatura o icono del Shell (no se agranda)
    std::wstring text;
    bool truncated = false;
    std::wstring typeName;
};

struct PreviewState {
    std::wstring want;  // lo que está bajo el cursor
    std::wstring path;  // lo que se pidió/muestra
    uint64_t gen = 0;
    std::unique_ptr<PreviewResult> res;
    ComPtr<ID2D1Bitmap> d2d;
    std::vector<std::pair<size_t, size_t>> lines;  // (inicio, largo) en res->text
    float scroll = 0;
    bool loading = false;
    D2D1_RECT_F box{};
} g_pv;

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
    if (!w || !h) return false;
    // orientación EXIF (fotos de celular)
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
        // el rotador necesita los píxeles en memoria
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

bool PvThumb(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    if (FAILED(f->GetImage(SIZE{512, 512}, SIIGBF_BIGGERSIZEOK, &hb)) || !hb) return false;
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight != 0) {
        const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // de arriba hacia abajo
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h) {
            bool anyAlpha = false;
            for (size_t i = 3; i < px.size() && !anyAlpha; i += 4) anyAlpha = px[i] != 0;
            if (!anyAlpha)
                for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            ok = SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4,
                                                       static_cast<UINT>(px.size()), px.data(), &r.bmp));
            if (ok) {
                r.kind = 1;
                r.thumb = true;
                r.imgW = w;
                r.imgH = h;
            }
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(hb);
    return ok;
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
        // binario: hay bytes nulos al principio
        for (size_t i = start; i < std::min<size_t>(got, 8192); ++i)
            if (buf[i] == 0) return false;
        size_t len = got - start;
        if (r.truncated) {  // cortar en un fin de línea para no partir un carácter
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
    // tabulaciones a 4 espacios, sin retornos de carro
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
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME)) r->typeName = sfi.szTypeName;
        std::wstring ext = ExtOf(path.c_str(), path.size());
        bool done = wic && PvIsImage(ext) && PvImage(wic.Get(), path, *r);
        if (!done && !PvIsBinary(ext)) done = PvText(path, *r);
        if (!done && wic) PvThumb(wic.Get(), path, *r);
        PreviewResult* raw = r.release();
        if (!PostMessageW(g_hwnd, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }
}

// Lo que está bajo el cursor de la pestaña activa ("" = nada).
std::wstring PreviewTarget() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return L"";
    return RowPath(t, t.sel);
}

// Se llama un rato después de que el cursor se quedó quieto.
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
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
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
    if (r->kind == 2) {
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
    g_pv.res = std::move(r);
    Invalidate();
}

void PreviewToggle() {
    g_preview = !g_preview;
    g_pv.want.clear();
    g_pv.path = L"\x01";  // fuerza la carga
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

// Una línea "etiqueta ...... valor" del bloque de datos.
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
    Fill(D2D1::RectF(x0, y0, x0 + 1, y1), cBorder);
    // pedir lo nuevo cuando el cursor se queda quieto
    std::wstring want = PreviewTarget();
    if (want != g_pv.want) {
        g_pv.want = want;
        SetTimer(g_hwnd, kTimerPreview, g_pv.path.empty() || g_pv.path == L"\x01" ? 1 : 60, nullptr);
    }
    const float pad = 14, L = x0 + pad, R = W - pad;
    Tab& t = T();
    if (g_pv.path.empty() || g_pv.path == L"\x01" || t.sel < 0 || t.sel >= Count()) {
        Color(cFaint);
        Text(L"Nada seleccionado", g_fmtCenter.Get(), x0, y0, W, y0 + 80);
        return;
    }
    // datos (abajo): de la fila del cursor si sigue siendo lo mostrado
    const bool rowMatches = Key(RowPath(t, t.sel)) == Key(g_pv.path);
    const Entry* e = rowMatches ? &EntryAt(t, t.sel) : nullptr;
    const bool isDir = e ? e->isDir() : false;
    std::vector<std::pair<const wchar_t*, std::wstring>> info;
    wchar_t buf[64];
    std::wstring type = g_pv.res && !g_pv.res->typeName.empty() ? g_pv.res->typeName : isDir ? L"Carpeta" : L"";
    if (!type.empty()) info.push_back({L"Tipo", type});
    if (e && !isDir) {
        FormatSize(e->size, buf, 64);
        info.push_back({L"Tamaño", std::wstring(buf) + L"  (" + Thousands(static_cast<int>(std::min<uint64_t>(e->size, INT_MAX))) + L" bytes)"});
    }
    auto cached = isDir ? g_cache.find(Key(g_pv.path)) : g_cache.end();
    if (isDir && cached != g_cache.end() && !cached->second->error)
        info.push_back({L"Contiene", Thousands(static_cast<int>(cached->second->entries.size())) + L" elementos"});
    DirSize ds;
    if (isDir && DirSizeOf(Key(g_pv.path), ds) && (ds.done || ds.bytes)) {
        FormatSize(ds.bytes, buf, 64);
        info.push_back({L"Tamaño", std::wstring(buf) + (ds.done ? L"" : L"…") + L"   ·   " + Thousands(static_cast<int>(ds.files)) + L" archivos"});
    }
    if (g_pv.res && g_pv.res->kind == 1 && !g_pv.res->thumb)
        info.push_back({L"Dimensiones", std::to_wstring(g_pv.res->imgW) + L" × " + std::to_wstring(g_pv.res->imgH)});
    if (e && e->mtime) {
        FormatDate(e->mtime, buf, 64);
        info.push_back({L"Modificado", buf});
    }
    const float infoH = 34 + static_cast<float>(info.size()) * 20 + 10;
    const float top = y0 + pad, bot = std::max(top + 60, y1 - infoH - 6);
    const D2D1_RECT_F box = D2D1::RectF(L, top, R, bot);
    g_pv.box = box;
    // contenido
    g_rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    if (isDir) {
        if (cached == g_cache.end()) {
            Color(cFaint);
            Text(L"Cargando…", g_fmtCenter.Get(), L, top, R, top + 60);
        } else if (cached->second->error) {
            Color(cError);
            Text(ErrorText(cached->second->error), g_fmtCenter.Get(), L, top, R, top + 60);
        } else {
            auto l = Prepared(cached->second);
            const float rh = 22;
            int first = static_cast<int>(g_pv.scroll / rh);
            if (l->entries.empty()) {
                Color(cFaint);
                Text(L"Carpeta vacía", g_fmtCenter.Get(), L, top, R, top + 60);
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
            float scale = 96.f / g_dpi;  // píxeles -> DIP
            float iw = px.width * scale, ih = px.height * scale;
            float bw = box.right - box.left, bh = box.bottom - box.top;
            float k = std::min(bw / iw, bh / ih);
            if (g_pv.res->thumb || k > 1) k = std::min(k, 1.f);  // no agrandar
            float dw = iw * k, dh = ih * k;
            float dx = box.left + (bw - dw) / 2, dy = box.top + (bh - dh) / 2;
            g_rt->DrawBitmap(g_pv.d2d.Get(), D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    } else if (g_pv.res && g_pv.res->kind == 2) {
        const float lh = 16;
        const std::wstring& s = g_pv.res->text;
        const int nl = static_cast<int>(g_pv.lines.size());
        const int digits = static_cast<int>(std::to_wstring(std::max(1, nl)).size());
        const float gutter = digits * 7.5f + 10;
        float maxScroll = std::max(0.f, nl * lh - (bot - top));
        g_pv.scroll = std::min(g_pv.scroll, maxScroll);
        if (s.empty()) {
            Color(cFaint);
            Text(L"Archivo vacío", g_fmtCenter.Get(), L, top, R, top + 60);
        }
        for (int i = std::max(0, static_cast<int>(g_pv.scroll / lh)); i < nl; ++i) {
            float y = top + i * lh - g_pv.scroll;
            if (y > bot) break;
            wchar_t num[16];
            swprintf(num, 16, L"%d", i + 1);
            Color(cFaint);
            Text(num, wcslen(num), g_fmtMonoR.Get(), L, y, L + gutter - 10, y + lh);
            Color(0xD4D4DA);
            auto [off, len] = g_pv.lines[i];
            Text(s.c_str() + off, std::min<size_t>(len, 400), g_fmtMono.Get(), L + gutter, y, R + 2000, y + lh);
        }
    } else if (g_pv.loading) {
        // sin parpadeo: no se muestra "cargando" (casi siempre tarda menos que un cuadro)
    } else {
        DrawIcon(g_pv.path, nullptr, nullptr, 0, false, (L + R) / 2 - kIcon / 2, top + 30, 1.f);
    }
    g_rt->PopAxisAlignedClip();
    // datos
    float y = bot + 10;
    HLine(L, R, y - 4);
    std::wstring name = rowMatches ? RowName(t, t.sel) : LeafOf(g_pv.path);
    Color(cText);
    Text(name, g_fmtNameBold.Get(), L, y, R, y + 26);
    y += 30;
    for (auto& [label, value] : info) PvInfoLine(label, value, L, R, y);
}

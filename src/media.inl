// Songs and videos playing in the preview, with Media Foundation's Media Engine. Nothing plays until asked (the play
// button, a click on the picture, or the palette's "preview play"); moving to another item, hiding the preview or the
// window stops it. Video runs in frame-server mode: each new frame is copied into a WIC bitmap the size it is shown
// at, and from there into a Direct2D bitmap. Included from app.cpp before preview.inl.

constexpr UINT WM_APP_MEDIA = WM_APP + 16;
constexpr UINT_PTR kTimerMedia = 17;

// The engine's events, sent to the window (they come on a Media Foundation thread).
class MediaNotify : public IMFMediaEngineNotify {
    std::atomic<ULONG> ref_{1};
    uint64_t gen_;

public:
    explicit MediaNotify(uint64_t gen) : gen_(gen) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) SetEvent(reinterpret_cast<HANDLE>(param1));
        else PostMessageW(g_hwnd, WM_APP_MEDIA, event, static_cast<LPARAM>(gen_));
        return S_OK;
    }
};

struct MediaState {
    ComPtr<IMFMediaEngine> engine;
    std::wstring path;
    bool video = false;
    uint64_t gen = 0;  // events from an engine already closed are left out
    DWORD w = 0, h = 0;  // the video's own size, once known
    std::wstring error;
    ComPtr<IWICBitmap> frame;  // where frames land, the size they are shown at
    UINT fw = 0, fh = 0;
    bool fresh = false;  // a new frame since the last draw
    bool shown = false;  // a frame has landed: the picture is the video now, not the thumbnail
    ComPtr<ID2D1Bitmap> d2d;
    bool seeking = false;  // dragging along the bar
    D2D1_RECT_F bar{};
} g_media;

void MediaClose() {
    if (g_media.engine) g_media.engine->Shutdown();
    if (g_hwnd) KillTimer(g_hwnd, kTimerMedia);
    const uint64_t gen = g_media.gen;
    g_media = MediaState();
    g_media.gen = gen;
}

bool MediaOpen(const std::wstring& path, bool video) {
    MediaClose();
    static const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    ComPtr<IMFMediaEngineClassFactory> f;
    ComPtr<IMFAttributes> a;
    g_media.path = path;
    g_media.video = video;
    if (!mf || FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(MFCreateAttributes(&a, 2))) {
        g_media.error = L"Media Foundation isn't available";
        return false;
    }
    ComPtr<MediaNotify> n;
    n.Attach(new MediaNotify(++g_media.gen));
    a->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, n.Get());
    a->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);  // frame-server mode
    if (FAILED(f->CreateInstance(video ? 0 : MF_MEDIA_ENGINE_AUDIOONLY, a.Get(), &g_media.engine))) {
        g_media.error = L"It can't be played here";
        return false;
    }
    BSTR url = SysAllocString(path.c_str());
    const HRESULT hr = g_media.engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) {
        g_media.engine->Shutdown();
        g_media.engine.Reset();
        g_media.error = L"It can't be played here";
        return false;
    }
    g_media.engine->Play();
    SetTimer(g_hwnd, kTimerMedia, 15, nullptr);
    return true;
}

bool MediaFor(const std::wstring& path) { return (g_media.engine || !g_media.error.empty()) && g_media.path == path; }
bool MediaPlaying() { return g_media.engine && !g_media.engine->IsPaused() && !g_media.engine->IsEnded(); }

// Play, pause, or start over once it ended.
void MediaPlayPause(const std::wstring& path, bool video) {
    if (!g_media.engine || g_media.path != path) {
        MediaOpen(path, video);
    } else if (g_media.engine->IsEnded()) {
        g_media.engine->SetCurrentTime(0);
        g_media.engine->Play();
    } else if (g_media.engine->IsPaused()) {
        g_media.engine->Play();
    } else {
        g_media.engine->Pause();
    }
    Invalidate();
}

void MediaEvent(DWORD event, uint64_t gen) {
    if (gen != g_media.gen || !g_media.engine) return;
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA && g_media.video) g_media.engine->GetNativeVideoSize(&g_media.w, &g_media.h);
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
        ComPtr<IMFMediaError> e;
        g_media.engine->GetError(&e);
        const USHORT code = e ? e->GetErrorCode() : 0;
        g_media.error = code == MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED ? L"Windows can't play this format" : L"It couldn't be played";
        KillTimer(g_hwnd, kTimerMedia);
    }
    Invalidate();
}

// Every 15 ms while open: a new video frame, if there is one, and the time moving on.
void MediaTick() {
    if (!g_media.engine) return;
    if (g_media.video && g_media.frame) {
        LONGLONG pts = 0;
        if (g_media.engine->OnVideoStreamTick(&pts) == S_OK) {
            const RECT dst{0, 0, static_cast<LONG>(g_media.fw), static_cast<LONG>(g_media.fh)};
            const MFARGB black{0, 0, 0, 255};
            if (SUCCEEDED(g_media.engine->TransferVideoFrame(g_media.frame.Get(), nullptr, &dst, &black))) g_media.fresh = g_media.shown = true;
        }
    }
    if (MediaPlaying() || g_media.fresh || g_media.seeking) Invalidate();
    SetTimer(g_hwnd, kTimerMedia, g_media.video ? 15 : 100, nullptr);
}

double MediaDuration(double fallback) {
    const double d = g_media.engine ? g_media.engine->GetDuration() : 0;
    return std::isfinite(d) && d > 0 ? d : fallback;
}

void MediaSeekAt(float x, double fallbackDur) {
    if (!g_media.engine || g_media.bar.right <= g_media.bar.left) return;
    const double frac = std::clamp((x - g_media.bar.left) / (g_media.bar.right - g_media.bar.left), 0.f, 1.f);
    const double d = MediaDuration(fallbackDur);
    if (d > 0) g_media.engine->SetCurrentTime(frac * d);
    Invalidate();
}

// The video's frame, fitted into r; false while there is none (the thumbnail shows meanwhile).
bool MediaDrawVideo(const D2D1_RECT_F& r) {
    if (!g_media.engine || !g_media.video || !g_media.w || !g_media.h) return false;
    const float rw = r.right - r.left, rh = r.bottom - r.top;
    const float k = std::min(rw / g_media.w, rh / g_media.h);
    const float dw = std::floor(g_media.w * k), dh = std::floor(g_media.h * k);
    const UINT pw = std::max(1u, static_cast<UINT>(std::lround(dw * g_dpi / 96.f))), ph = std::max(1u, static_cast<UINT>(std::lround(dh * g_dpi / 96.f)));
    if (pw != g_media.fw || ph != g_media.fh) {  // a new size: a new target, and the current frame into it
        g_media.frame.Reset();
        g_media.d2d.Reset();
        if (FAILED(g_wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &g_media.frame))) return false;
        g_media.fw = pw;
        g_media.fh = ph;
        const RECT dst{0, 0, static_cast<LONG>(pw), static_cast<LONG>(ph)};
        const MFARGB black{0, 0, 0, 255};
        if (g_media.shown && SUCCEEDED(g_media.engine->TransferVideoFrame(g_media.frame.Get(), nullptr, &dst, &black))) g_media.fresh = true;
    }
    if (!g_media.shown) return false;
    if (g_media.fresh || !g_media.d2d) {
        const WICRect all{0, 0, static_cast<INT>(pw), static_cast<INT>(ph)};
        ComPtr<IWICBitmapLock> lock;
        UINT size = 0, stride = 0;
        BYTE* data = nullptr;
        if (SUCCEEDED(g_media.frame->Lock(&all, WICBitmapLockRead, &lock)) && SUCCEEDED(lock->GetDataPointer(&size, &data)) &&
            SUCCEEDED(lock->GetStride(&stride))) {
            if (!g_media.d2d)
                g_rt->CreateBitmap(D2D1::SizeU(pw, ph), data, stride,
                                   D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &g_media.d2d);
            else
                g_media.d2d->CopyFromMemory(nullptr, data, stride);
        }
        g_media.fresh = false;
    }
    if (!g_media.d2d) return false;
    const float x = std::round(r.left + (rw - dw) / 2), y = std::round(r.top + (rh - dh) / 2);
    g_rt->DrawBitmap(g_media.d2d.Get(), D2D1::RectF(x, y, x + dw, y + dh), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

// The row under the picture: play / pause, the time, the bar to seek along, how long it lasts.
void MediaControls(const std::wstring& path, float L, float R, float y, double fallbackDur) {
    const bool mine = MediaFor(path);
    if (mine && !g_media.error.empty()) {
        Color(cDim);
        Text(g_media.error, g_fmtCenter.Get(), L, y, R, y + 28);
        return;
    }
    const bool playing = mine && MediaPlaying();
    const D2D1_RECT_F btn = D2D1::RectF(L, y, L + 28, y + 28);
    Round(btn, 14, IsHover(HitKind::PreviewPlay, 1) ? 0x3A3A44 : 0x2C2C34);
    Color(cText);
    Glyph(playing ? L'\xE769' : L'\xE768', g_fmtIcon.Get(), btn.left, btn.top, btn.right, btn.bottom);  // Pause / Play
    g_hits.push_back({btn, HitKind::PreviewPlay, 1});
    const double dur = mine ? MediaDuration(fallbackDur) : fallbackDur;
    const double now = mine && g_media.engine ? g_media.engine->GetCurrentTime() : 0;
    const std::wstring a = FormatDuration(static_cast<uint64_t>(std::max(0.0, now) * 1e7)), b = dur > 0 ? FormatDuration(static_cast<uint64_t>(dur * 1e7)) : L"";
    const float tw = std::max(34.f, MeasureW(b.empty() ? a : b, g_fmtName.Get()) + 4);
    Color(cDim);
    Text(a, g_fmtName.Get(), L + 36, y, L + 36 + tw, y + 28);
    Text(b, g_fmtRight.Get(), R - tw, y, R, y + 28);
    const float bl = L + 40 + tw, br = R - tw - 8, mid = y + 14;
    g_media.bar = D2D1::RectF(bl, mid - 8, br, mid + 8);
    const float frac = dur > 0 ? static_cast<float>(std::clamp(now / dur, 0.0, 1.0)) : 0.f;
    Round(D2D1::RectF(bl, mid - 2, br, mid + 2), 2, 0xFFFFFF, 0.14f);
    if (frac > 0) Round(D2D1::RectF(bl, mid - 2, bl + (br - bl) * frac, mid + 2), 2, cAccent);
    if (mine && (g_media.seeking || IsHover(HitKind::PreviewSeek, 1))) {
        const float kx = bl + (br - bl) * frac;
        Color(cText);
        g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, mid), 6, 6), g_brush.Get());
    }
    if (mine && g_media.engine) g_hits.push_back({g_media.bar, HitKind::PreviewSeek, 1});
}

// Checksums (Ctrl+K → "checksums", or the right-click menu): MD5, SHA-1 and SHA-256 of the selected files, in a panel
// over the window. Each file is read once, in the background, for the three at a time. A click on a hash copies it, and
// Copy all copies them all. A hash in the clipboard (copied from a download's page, before opening the panel or while
// it is open) is looked for among them: the one it matches shows in green, or the panel says it matches none.
// Included from app.cpp after archive.inl.

constexpr UINT WM_APP_HASH = WM_APP + 22;
constexpr UINT_PTR kTimerHashCopied = 22;  // "Copied" goes away
void TestLog(const std::wstring& s);         // testmode.inl
constexpr int kHsAlgs = 3;
const wchar_t* const kHsNames[kHsAlgs] = {L"MD5", L"SHA-1", L"SHA-256"};
const wchar_t* const kHsBcrypt[kHsAlgs] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM};
const int kHsLen[kHsAlgs] = {16, 20, 32};  // bytes

struct HsFile {
    std::wstring path;
    uint64_t size = 0;
    std::wstring hash[kHsAlgs];  // lowercase hex; empty until read
    std::wstring error;          // why it couldn't be read
    bool done = false;
};

enum class HsK { None, Hash, CopyAll, Close };
struct HsHit {
    D2D1_RECT_F r;
    HsK k;
    int file = 0, alg = 0;
};

struct HashPanel {
    bool open = false;
    std::vector<HsFile> files;
    int folders = 0;      // left out
    uint64_t gen = 0;
    int reading = -1;     // the file being read
    std::wstring clip;    // a hash found in the clipboard (lowercase hex), or empty
    std::wstring own;     // what the panel itself copied last: not a hash to check
    int copiedFile = -1, copiedAlg = -1;  // the last one copied: "Copied" next to it for a moment
    double copiedAt = 0;
    float scroll = 0, contentH = 0, viewH = 0;
    D2D1_RECT_F box{};
    std::vector<HsHit> hits;
    HsHit hover{};
    bool hovering = false;
} g_hs;
std::atomic<uint64_t> g_hsGen{0};
std::atomic<uint64_t> g_hsDone{0}, g_hsTotal{0};  // bytes of the file being read
std::atomic<bool> g_hsNotify{false};
std::wstring g_testClip;  // test mode: the clipboard, without touching the real one

struct HsMsg {
    uint64_t gen = 0;
    int file = -1;
    std::wstring hash[kHsAlgs], error;
};

// Hex text of some bytes, lowercase.
std::wstring HsHex(const uint8_t* p, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s(n * 2, L'0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

void HsThread(std::vector<std::wstring> paths, uint64_t gen) {
    BCRYPT_ALG_HANDLE alg[kHsAlgs]{};
    bool ok = true;
    for (int a = 0; a < kHsAlgs; ++a) ok &= BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg[a], kHsBcrypt[a], nullptr, 0));
    std::vector<uint8_t> buf(1 << 20);
    for (size_t i = 0; i < paths.size() && g_hsGen == gen; ++i) {
        auto m = std::make_unique<HsMsg>();
        m->gen = gen;
        m->file = static_cast<int>(i);
        HANDLE f = CreateFileW(ToExtendedPath(paths[i]).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER sz{};
        if (!ok) m->error = L"Windows can't compute these here";
        else if (f == INVALID_HANDLE_VALUE) m->error = ErrorText(GetLastError());
        else GetFileSizeEx(f, &sz);
        g_hsTotal = static_cast<uint64_t>(sz.QuadPart);
        g_hsDone = 0;
        BCRYPT_HASH_HANDLE h[kHsAlgs]{};
        if (m->error.empty())
            for (int a = 0; a < kHsAlgs; ++a)
                if (!BCRYPT_SUCCESS(BCryptCreateHash(alg[a], &h[a], nullptr, 0, nullptr, 0, 0))) m->error = L"Windows can't compute these here";
        double last = NowMs();
        while (m->error.empty() && g_hsGen == gen) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                m->error = ErrorText(GetLastError());
                break;
            }
            if (got == 0) break;
            for (int a = 0; a < kHsAlgs; ++a) BCryptHashData(h[a], buf.data(), got, 0);
            g_hsDone += got;
            if (NowMs() - last > 120 && !g_hsNotify.exchange(true)) {
                last = NowMs();
                PostMessageW(g_hwnd, WM_APP_HASH, 0, 0);
            }
        }
        for (int a = 0; a < kHsAlgs; ++a) {
            if (!h[a]) continue;
            uint8_t out[64];
            if (m->error.empty() && BCRYPT_SUCCESS(BCryptFinishHash(h[a], out, kHsLen[a], 0))) m->hash[a] = HsHex(out, kHsLen[a]);
            BCryptDestroyHash(h[a]);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (g_hsGen != gen) break;
        HsMsg* raw = m.release();
        if (!PostMessageW(g_hwnd, WM_APP_HASH, 1, reinterpret_cast<LPARAM>(raw))) delete raw;
    }
    for (int a = 0; a < kHsAlgs; ++a)
        if (alg[a]) BCryptCloseAlgorithmProvider(alg[a], 0);
}

// The first hash-looking word in the text: 32, 40 or 64 hex digits standing alone ("sha256:" before it, or the file
// name after it, as sums files have it, are fine). Lowercase; empty if there's none.
std::wstring HsFindHash(const std::wstring& s) {
    for (size_t i = 0; i < s.size();) {
        if (!iswxdigit(s[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && iswxdigit(s[j])) ++j;
        const size_t n = j - i;
        const bool alone = (i == 0 || !iswalnum(s[i - 1])) && (j == s.size() || !iswalnum(s[j]));
        if (alone && (n == 32 || n == 40 || n == 64)) {
            std::wstring h = s.substr(i, n);
            for (wchar_t& c : h) c = static_cast<wchar_t>(towlower(c));
            return h;
        }
        i = j;
    }
    return L"";
}

std::wstring ClipboardText() {
    if (g_test) return g_testClip;
    std::wstring out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(g_hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p, wcsnlen(p, 4096));
            GlobalUnlock(h);
        }
    CloseClipboard();
    return out;
}

// Copies text, or in test mode only says so (the real clipboard stays as it was).
bool CopyText(const std::wstring& s) {
    if (g_test) {
        g_testClip = s;
        TestLog(L"clipboard (test): " + s);
        return true;
    }
    return SetClipboardText(s);
}

void HsReadClipboard() {
    if (!g_hs.open) return;
    const std::wstring text = ClipboardText();
    if (!g_hs.own.empty() && text == g_hs.own) return;  // what it copied itself
    const std::wstring h = HsFindHash(text);
    if (h != g_hs.clip) {
        g_hs.clip = h;
        Invalidate();
    }
}

void HashClose() {
    if (!g_hs.open) return;
    ++g_hsGen;  // a file still being read stops
    g_hs.open = false;
    g_hs.files.clear();
    g_hs.hits.clear();
    Invalidate();
}

void HashOpen() {
    Tab& t = T();
    std::vector<HsFile> files;
    int folders = 0;
    if (!IsBin(t.path) && !InArchive(t.path))
        for (int i = 0; i < RowCount(t); ++i) {
            if (!IsSel(t, i) || IsHeader(t, i)) continue;
            const Entry& e = EntryAt(t, i);
            if (e.isDir()) {
                ++folders;
                continue;
            }
            HsFile f;
            f.path = RowPath(t, i);
            f.size = e.size;
            files.push_back(std::move(f));
        }
    if (files.empty()) {
        S.message = InArchive(t.path) ? L"Inside an archive: extract the file first to get its checksums"
                    : folders ? L"Checksums are for files: select one or more"
                              : L"Select one or more files for their checksums";
        Invalidate();
        return;
    }
    PopupClose();
    PaletteClose();
    ViewEditorClose();
    MenuEditorClose();
    SettingsClose();
    CancelRename();
    if (files.size() > 1000) files.resize(1000);
    g_hs = HashPanel{};
    g_hs.files = std::move(files);
    g_hs.folders = folders;
    g_hs.open = true;
    g_hs.gen = ++g_hsGen;
    g_hs.reading = 0;
    std::vector<std::wstring> paths;
    for (const HsFile& f : g_hs.files) paths.push_back(f.path);
    std::thread(HsThread, std::move(paths), g_hs.gen).detach();
    HsReadClipboard();
    Invalidate();
}

void HashApply(WPARAM wp, LPARAM lp) {
    if (wp == 0) {  // progress
        g_hsNotify = false;
        if (g_hs.open) Invalidate();
        return;
    }
    std::unique_ptr<HsMsg> m(reinterpret_cast<HsMsg*>(lp));
    if (!g_hs.open || m->gen != g_hs.gen || m->file < 0 || m->file >= static_cast<int>(g_hs.files.size())) return;
    HsFile& f = g_hs.files[m->file];
    for (int a = 0; a < kHsAlgs; ++a) f.hash[a] = m->hash[a];
    f.error = m->error;
    f.done = true;
    g_hs.reading = m->file + 1 < static_cast<int>(g_hs.files.size()) ? m->file + 1 : -1;
    Invalidate();
}

// Where the copied hash is: file and algorithm, or -1 (not found, or not all read yet: then *pending).
std::pair<int, int> HsMatch(bool* pending = nullptr) {
    if (pending) *pending = false;
    if (g_hs.clip.empty()) return {-1, -1};
    for (size_t i = 0; i < g_hs.files.size(); ++i) {
        const HsFile& f = g_hs.files[i];
        if (!f.done && pending) *pending = true;
        for (int a = 0; a < kHsAlgs; ++a)
            if (f.hash[a] == g_hs.clip) return {static_cast<int>(i), a};
    }
    return {-1, -1};
}

std::wstring HsAllText() {
    std::wstring out;
    for (const HsFile& f : g_hs.files) {
        if (!out.empty()) out += L"\r\n";
        out += LeafOf(f.path) + L"\r\n";
        if (!f.error.empty()) {
            out += L"  " + f.error + L"\r\n";
            continue;
        }
        for (int a = 0; a < kHsAlgs; ++a) {
            std::wstring label = kHsNames[a];
            label.resize(9, L' ');
            out += label + (f.done ? f.hash[a] : L"(still being read)") + L"\r\n";
        }
    }
    return out;
}

void HsCopy(int file, int alg) {
    if (file < 0 || file >= static_cast<int>(g_hs.files.size()) || g_hs.files[file].hash[alg].empty()) return;
    g_hs.own = g_hs.files[file].hash[alg];
    if (!CopyText(g_hs.own)) return;
    g_hs.copiedFile = file;
    g_hs.copiedAlg = alg;
    g_hs.copiedAt = NowMs();
    S.message = std::wstring(kHsNames[alg]) + L" of " + LeafOf(g_hs.files[file].path) + L" copied";
    SetTimer(g_hwnd, kTimerHashCopied, 1300, nullptr);
    Invalidate();
}

// ---- drawing
constexpr float kHsW = 720.f, kHsRow = 24.f, kHsHead = 58.f, kHsFoot = 64.f;

float HsBlockH(const HsFile& f) { return 30.f + (f.error.empty() ? kHsAlgs * kHsRow : kHsRow) + 10.f; }

void DrawHash(float W, float H) {
    if (!g_hs.open) return;
    g_hs.hits.clear();
    float content = 0;
    for (const HsFile& f : g_hs.files) content += HsBlockH(f);
    const float pw = std::min(kHsW, W - 40), ph = std::min(kHsHead + content + kHsFoot + 8, H - kTabH - 24);
    const float x0 = std::floor((W - pw) / 2), y0 = std::floor(std::max(kTabH + 8, (H - ph) / 2));
    const float R = x0 + pw, B = y0 + ph;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_hs.box = D2D1::RectF(x0, y0, R, B);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x0 - i, y0 - i + 2, R + i, B + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_hs.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_hs.box, 12, 12), g_brush.Get(), 1.f);
    auto isHover = [&](HsK k, int file = 0, int alg = 0) {
        return g_hs.hovering && g_hs.hover.k == k && g_hs.hover.file == file && g_hs.hover.alg == alg;
    };

    // title
    const int n = static_cast<int>(g_hs.files.size());
    uint64_t total = 0;
    for (const HsFile& f : g_hs.files) total += f.size;
    wchar_t sz[64];
    FormatSize(total, sz, 64);
    Color(cAccent);
    Glyph(0xE72E, g_fmtIcon.Get(), x0 + 18, y0 + 14, x0 + 42, y0 + 42);  // lock
    Color(cText);
    Text(L"Checksums", g_fmtNameBold.Get(), x0 + 48, y0 + 8, R - 60, y0 + 30);
    Color(cDim);
    std::wstring sub = (n == 1 ? LeafOf(g_hs.files[0].path) : Thousands(n) + L" files") + L"  ·  " + sz;
    if (g_hs.folders) sub += L"  ·  " + Thousands(g_hs.folders) + (g_hs.folders == 1 ? L" folder left out" : L" folders left out");
    Text(sub, g_fmtStatus.Get(), x0 + 48, y0 + 27, R - 60, y0 + 48);
    const D2D1_RECT_F cb = D2D1::RectF(R - 42, y0 + 14, R - 12, y0 + 42);
    if (isHover(HsK::Close)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(HsK::Close) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_hs.hits.push_back({cb, HsK::Close});
    HLine(x0, R, y0 + kHsHead - 2);

    // the files, scrolling when they don't fit
    const float top = y0 + kHsHead, bot = B - kHsFoot;
    g_hs.viewH = bot - top;
    g_hs.contentH = content;
    g_hs.scroll = std::clamp(g_hs.scroll, 0.f, std::max(0.f, content - g_hs.viewH));
    bool pending = false;
    const auto [mFile, mAlg] = HsMatch(&pending);
    const float cl = x0 + 22, cr = R - 22, hx = cl + 78;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, top, R, bot), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top + 6 - g_hs.scroll;
    for (int i = 0; i < n; ++i) {
        const HsFile& f = g_hs.files[i];
        const float bh = HsBlockH(f);
        if (y > bot) break;
        if (y + bh < top) {
            y += bh;
            continue;
        }
        FormatSize(f.size, sz, 64);
        const float sw = MeasureW(sz, g_fmtStatus.Get());
        Color(cText);
        Text(LeafOf(f.path), g_fmtNameBold.Get(), cl, y, cr - sw - 16, y + 26);
        Color(cFaint);
        Text(sz, g_fmtStatus.Get(), cr - sw, y, cr, y + 26);
        float ry = y + 30;
        if (!f.error.empty()) {
            Color(cError);
            Text(L"Couldn't read it: " + f.error, g_fmtStatus.Get(), cl, ry, cr, ry + kHsRow);
        } else if (!f.done) {
            const bool now = g_hs.reading == i;
            const double p = now && g_hsTotal ? std::min(1.0, static_cast<double>(g_hsDone.load()) / static_cast<double>(g_hsTotal.load())) : 0;
            Color(cDim);
            Text(now ? (g_hsTotal > (8u << 20) ? L"Reading…  " + std::to_wstring(static_cast<int>(p * 100)) + L"%" : std::wstring(L"Reading…")) : L"Waiting",
                 g_fmtStatus.Get(), cl, ry, cr, ry + kHsRow);
            if (now) {
                const D2D1_RECT_F bar = D2D1::RectF(cl, ry + kHsRow + 6, cr, ry + kHsRow + 9);
                Round(bar, 1.5f, cBorder);
                if (p > 0) Round(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * static_cast<float>(p), bar.bottom), 1.5f, cAccent);
            }
        } else {
            for (int a = 0; a < kHsAlgs; ++a, ry += kHsRow) {
                const D2D1_RECT_F rr = D2D1::RectF(cl - 8, ry, cr + 8, ry + kHsRow);
                const bool match = mFile == i && mAlg == a, hov = isHover(HsK::Hash, i, a);
                if (match) Round(rr, 5, cRecent, 0.14f);
                else if (hov) Round(rr, 5, 0xFFFFFF, 0.06f);
                Color(match ? cRecent : cDim);
                Text(kHsNames[a], g_fmtStatus.Get(), cl, ry, hx - 8, ry + kHsRow);
                Color(match ? cRecent : cText);
                Text(f.hash[a], g_fmtMono.Get(), hx, ry, cr - 64, ry + kHsRow);
                const bool copied = g_hs.copiedFile == i && g_hs.copiedAlg == a && NowMs() - g_hs.copiedAt < 1200;
                if (copied || hov || match) {
                    Color(copied ? cAccent : match ? cRecent : cFaint);
                    Text(copied ? L"Copied" : match ? L"✓ matches" : L"Copy", g_fmtStatusR.Get(), cr - 80, ry, cr, ry + kHsRow);
                }
                if (ry + kHsRow > top && ry < bot) g_hs.hits.push_back({rr, HsK::Hash, i, a});
            }
        }
        y += bh;
    }
    g_rt->PopAxisAlignedClip();
    if (content > g_hs.viewH) {  // where the view is in the list
        const float th = std::max(24.f, g_hs.viewH * g_hs.viewH / content);
        const float ty = top + (g_hs.viewH - th) * (g_hs.scroll / (content - g_hs.viewH));
        Round(D2D1::RectF(R - 7, ty, R - 4, ty + th), 1.5f, 0xFFFFFF, 0.18f);
    }

    // the copied hash, and the buttons
    HLine(x0, R, bot);
    const float fy = bot + 10;
    std::wstring note;
    UINT32 noteColor = cFaint;
    if (g_hs.clip.empty()) {
        note = L"Copy a hash (from the page you downloaded it from) to check it here";
    } else if (mFile >= 0) {
        note = L"✓ The copied hash is the " + std::wstring(kHsNames[mAlg]) + L" of " + LeafOf(g_hs.files[mFile].path);
        noteColor = cRecent;
    } else if (pending) {
        note = L"Looking for the copied hash…";
        noteColor = cDim;
    } else {
        note = L"The copied hash (" + g_hs.clip.substr(0, 8) + L"…) matches none of these";
        noteColor = 0xE0A458;
    }
    const D2D1_RECT_F copyAll = D2D1::RectF(cr - 96, fy + 8, cr, fy + 38);
    Color(noteColor);
    Text(note, g_fmtStatus.Get(), cl, fy + 8, copyAll.left - 14, fy + 38);
    Round(copyAll, 6, isHover(HsK::CopyAll) ? 0x3A3A44 : cBg);
    Color(cText);
    Text(L"Copy all", g_fmtCenter.Get(), copyAll.left, copyAll.top, copyAll.right, copyAll.bottom);
    g_hs.hits.push_back({copyAll, HsK::CopyAll});
}

// ---- mouse and keyboard
int HsHitAt(float x, float y) {
    for (int i = static_cast<int>(g_hs.hits.size()) - 1; i >= 0; --i)
        if (InRect(g_hs.hits[i].r, x, y)) return i;
    return -1;
}

void HashMouseMove(float x, float y) {
    const int h = HsHitAt(x, y);
    const bool was = g_hs.hovering;
    const HsHit old = g_hs.hover;
    g_hs.hovering = h >= 0;
    g_hs.hover = h >= 0 ? g_hs.hits[h] : HsHit{};
    if (was != g_hs.hovering || old.k != g_hs.hover.k || old.file != g_hs.hover.file || old.alg != g_hs.hover.alg) Invalidate();
}

void HashClick(float x, float y) {
    if (!InRect(g_hs.box, x, y)) {
        HashClose();
        return;
    }
    const int hi = HsHitAt(x, y);
    if (hi < 0) return;
    const HsHit h = g_hs.hits[hi];
    switch (h.k) {
        case HsK::Close: HashClose(); return;
        case HsK::Hash: HsCopy(h.file, h.alg); return;
        case HsK::CopyAll:
            g_hs.own = HsAllText();
            if (CopyText(g_hs.own)) S.message = g_hs.files.size() == 1 ? L"Checksums copied" : L"Checksums of " + Thousands(static_cast<int>(g_hs.files.size())) + L" files copied";
            Invalidate();
            return;
        default: return;
    }
}

void HashWheel(int delta) {
    g_hs.scroll = std::clamp(g_hs.scroll - delta / 120.f * 3 * kHsRow, 0.f, std::max(0.f, g_hs.contentH - g_hs.viewH));
    Invalidate();
}

void HashKey(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE: HashClose(); break;
        case 'C':
            if (KeyDown(VK_CONTROL) && CopyText(g_hs.own = HsAllText())) {
                S.message = L"Checksums copied";
                Invalidate();
            }
            break;
        case 'V':
            if (KeyDown(VK_CONTROL)) HsReadClipboard();
            break;
        case VK_DOWN: HashWheel(-40); break;
        case VK_UP: HashWheel(40); break;
        case VK_NEXT: HashWheel(-480); break;
        case VK_PRIOR: HashWheel(480); break;
        default: break;
    }
}

// Attributes and dates (Shift+Alt+Enter): a panel over the window for the selection. Hidden and read-only as
// switches (a dash when the items differ: those stay as they are unless clicked), and the modified and created
// dates as text, "dd/mm/yyyy  hh:mm", with a Now button. Nothing changes until Apply.
// Included from app.cpp after settings.inl (it uses its switch and field drawing).

enum class AtK { None, Hidden, ReadOnly, Field, Now, Apply, Cancel, Close };
struct AtHit {
    D2D1_RECT_F r;
    AtK k;
    int idx = 0;  // Field / Now: 0 modified, 1 created
};

struct AttrsPanel {
    bool open = false;
    std::vector<std::wstring> files;
    int hidden = 0, readOnly = 0;  // 0 off, 1 on, 2 mixed (left alone)
    std::wstring date[2], shown[2];  // what is typed, and what it was on open (unchanged: not written)
    int editing = -1;
    HWND edit = nullptr;
    D2D1_RECT_F box{};
    std::vector<AtHit> hits;
    AtHit hover{};
    bool hovering = false;
} g_at;

constexpr float kAtW = 520.f;

std::wstring AtDateText(int64_t ft) {
    wchar_t b[64];
    FormatDate(ft, b, 64);
    return b;
}

// "08/11/2026  03:47" (seconds optional) -> FILETIME, local time. false if it doesn't read.
bool AtParseDate(const std::wstring& s, FILETIME& out) {
    unsigned d = 0, m = 0, y = 0, hh = 0, mm = 0, ss = 0;
    const int n = swscanf_s(s.c_str(), L"%u/%u/%u %u:%u:%u", &d, &m, &y, &hh, &mm, &ss);
    if (n < 3 || d < 1 || d > 31 || m < 1 || m > 12 || y < 1601 || hh > 23 || mm > 59 || ss > 59) return false;
    SYSTEMTIME lt{};
    lt.wDay = static_cast<WORD>(d);
    lt.wMonth = static_cast<WORD>(m);
    lt.wYear = static_cast<WORD>(y);
    lt.wHour = static_cast<WORD>(hh);
    lt.wMinute = static_cast<WORD>(mm);
    lt.wSecond = static_cast<WORD>(ss);
    SYSTEMTIME ut;
    return TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut) && SystemTimeToFileTime(&ut, &out);
}

void AtCommitText();
void AtBeginText(int i);

void AttrsClose() {
    if (!g_at.open) return;
    AtCommitText();
    if (g_at.edit) ShowWindow(g_at.edit, SW_HIDE);
    g_at.open = false;
    g_at.files.clear();
    g_at.hits.clear();
    Invalidate();
}

void AttrsOpen() {
    Tab& t = T();
    const std::vector<std::wstring> files = SelectedFullPaths(t);
    if (files.empty() || IsBin(t.path)) {
        S.message = L"Select something first";
        Invalidate();
        return;
    }
    PopupClose();
    PaletteClose();
    ViewEditorClose();
    MenuEditorClose();
    SettingsClose();
    CancelRename();
    g_at = AttrsPanel{.edit = g_at.edit};
    g_at.files = files;
    int hid = 0, ro = 0;
    int64_t mt = 0, ct = 0;
    bool sameMt = true, sameCt = true;
    for (size_t i = 0; i < files.size(); ++i) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(ToExtendedPath(files[i]).c_str(), GetFileExInfoStandard, &d)) continue;
        hid += (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        ro += (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        const int64_t m = (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        const int64_t c = (static_cast<int64_t>(d.ftCreationTime.dwHighDateTime) << 32) | d.ftCreationTime.dwLowDateTime;
        if (i == 0) mt = m, ct = c;
        sameMt &= m / 600000000 == mt / 600000000;  // to the minute, which is what is shown
        sameCt &= c / 600000000 == ct / 600000000;
    }
    const int n = static_cast<int>(files.size());
    g_at.hidden = hid == 0 ? 0 : hid == n ? 1 : 2;
    g_at.readOnly = ro == 0 ? 0 : ro == n ? 1 : 2;
    g_at.date[0] = g_at.shown[0] = sameMt ? AtDateText(mt) : L"";
    g_at.date[1] = g_at.shown[1] = sameCt ? AtDateText(ct) : L"";
    g_at.open = true;
    Invalidate();
}

// Writes what changed to every file; the dates typed must read as dates.
void AttrsApply() {
    AtCommitText();
    FILETIME ft[2]{};
    bool set[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        const std::wstring v = Trim(g_at.date[i]);
        if (v.empty() || v == g_at.shown[i]) continue;
        if (!AtParseDate(v, ft[i])) {
            S.message = std::wstring(i ? L"Created" : L"Modified") + L": dates read as dd/mm/yyyy hh:mm, like 08/11/2026 03:47";
            Invalidate();
            return;
        }
        set[i] = true;
    }
    int done = 0, failed = 0;
    for (const std::wstring& f : g_at.files) {
        if (TestRefuses(f)) continue;
        const std::wstring x = ToExtendedPath(f);
        DWORD a = GetFileAttributesW(x.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            ++failed;
            continue;
        }
        bool ok = true;
        DWORD want = a;
        if (g_at.hidden != 2) want = g_at.hidden ? want | FILE_ATTRIBUTE_HIDDEN : want & ~FILE_ATTRIBUTE_HIDDEN;
        if (g_at.readOnly != 2) want = g_at.readOnly ? want | FILE_ATTRIBUTE_READONLY : want & ~FILE_ATTRIBUTE_READONLY;
        if (set[0] || set[1]) {
            // a read-only file refuses new dates: it is let go for a moment
            if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(x.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
            HANDLE h = CreateFileW(x.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE) ok = false;
            else {
                ok = SetFileTime(h, set[1] ? &ft[1] : nullptr, nullptr, set[0] ? &ft[0] : nullptr) != 0;
                CloseHandle(h);
            }
        }
        if (want != a || (a & FILE_ATTRIBUTE_READONLY)) ok &= SetFileAttributesW(x.c_str(), want) != 0;
        ok ? ++done : ++failed;
    }
    const int n = static_cast<int>(g_at.files.size());
    S.message = failed ? Thousands(failed) + (failed == 1 ? L" item couldn't be changed" : L" items couldn't be changed") + (done ? L"; " + Thousands(done) + L" done" : L"")
                       : L"Changed " + (n == 1 ? LeafOf(g_at.files[0]) : Thousands(n) + L" items");
    AttrsClose();
    Enqueue(T().path, JobKind::Revalidate);
    if (T().recursive) RecCheckSoon();
}

// ---- the date box
void AtFocus(HWND h) {
    if (!g_test) SetFocus(h);
}

LRESULT CALLBACK AtEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        AttrsApply();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_TAB) {  // the other date
        const int next = 1 - g_at.editing;
        AtCommitText();
        AtBeginText(next);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        g_at.editing = -1;
        ShowWindow(h, SW_HIDE);
        AtFocus(g_hwnd);
        Invalidate();
        return 0;
    }
    if (m == WM_KEYDOWN && w == 'A' && KeyDown(VK_CONTROL)) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB || w == 1)) return 0;
    if (m == WM_KILLFOCUS && g_at.editing >= 0) AtCommitText();
    return DefSubclassProc(h, m, w, l);
}

void AtBeginText(int i) {
    if (!g_at.edit) {
        g_at.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(g_at.edit, AtEditProc, 5, 0);
    }
    SendMessageW(g_at.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    SendMessageW(g_at.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetWindowTextW(g_at.edit, g_at.date[i].c_str());
    g_at.editing = i;
    Invalidate();
    UpdateWindow(g_hwnd);
    AtFocus(g_at.edit);
    SendMessageW(g_at.edit, EM_SETSEL, 0, -1);
}

void AtCommitText() {
    if (g_at.editing < 0 || !g_at.edit) return;
    const int i = g_at.editing;
    g_at.editing = -1;
    ShowWindow(g_at.edit, SW_HIDE);
    const int n = GetWindowTextLengthW(g_at.edit);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(g_at.edit, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    g_at.date[i] = s;
    Invalidate();
}

// ---- drawing
void DrawAttrs(float W, float H) {
    if (!g_at.open) return;
    g_at.hits.clear();
    const float pw = std::min(kAtW, W - 40), ph = 268;
    const float x0 = std::floor((W - pw) / 2), y0 = std::floor(std::max(kTabH + 8, (H - ph) / 2));
    const float R = x0 + pw, B = y0 + ph;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_at.box = D2D1::RectF(x0, y0, R, B);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x0 - i, y0 - i + 2, R + i, B + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_at.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_at.box, 12, 12), g_brush.Get(), 1.f);
    auto isHover = [&](AtK k, int idx = 0) { return g_at.hovering && g_at.hover.k == k && g_at.hover.idx == idx; };

    const int n = static_cast<int>(g_at.files.size());
    Color(cAccent);
    Glyph(0xE946, g_fmtIcon.Get(), x0 + 18, y0 + 14, x0 + 42, y0 + 42);  // info
    Color(cText);
    Text(L"Attributes and dates", g_fmtNameBold.Get(), x0 + 48, y0 + 8, R - 60, y0 + 30);
    Color(cDim);
    Text(n == 1 ? LeafOf(g_at.files[0]) : Thousands(n) + L" items", g_fmtStatus.Get(), x0 + 48, y0 + 27, R - 60, y0 + 48);
    const D2D1_RECT_F cb = D2D1::RectF(R - 42, y0 + 14, R - 12, y0 + 42);
    if (isHover(AtK::Close)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(AtK::Close) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_at.hits.push_back({cb, AtK::Close});
    HLine(x0, R, y0 + 56);

    const float cl = x0 + 22, cr = R - 22;
    float y = y0 + 66;
    // switches
    const struct {
        const wchar_t* label;
        int* state;
        AtK k;
    } sw[] = {{L"Hidden", &g_at.hidden, AtK::Hidden}, {L"Read-only", &g_at.readOnly, AtK::ReadOnly}};
    for (const auto& s : sw) {
        Color(cText);
        Text(s.label, g_fmtName.Get(), cl, y, cr - 60, y + 30);
        const D2D1_RECT_F r = D2D1::RectF(cr - 38, y + 5, cr, y + 25);
        if (*s.state == 2) {  // mixed: a gray switch with a dash
            Round(r, 10, 0x45454F);
            Fill(D2D1::RectF(r.left + 13, y + 14, r.right - 13, y + 16), 0xFFFFFF, 0.8f);
            Color(cFaint);
            Text(L"differs: left as is", g_fmtStatus.Get(), cr - 170, y, cr - 46, y + 30);
        } else {
            SetSwitch(r, *s.state == 1);
        }
        g_at.hits.push_back({D2D1::RectF(cl, y, cr, y + 30), s.k});
        y += 36;
    }
    y += 6;
    // dates
    const wchar_t* names[2] = {L"Modified", L"Created"};
    for (int i = 0; i < 2; ++i) {
        Color(cText);
        Text(names[i], g_fmtName.Get(), cl, y, cl + 100, y + 30);
        const D2D1_RECT_F now = D2D1::RectF(cr - 54, y + 1, cr, y + 29);
        const D2D1_RECT_F f = D2D1::RectF(cl + 100, y + 1, now.left - 10, y + 29);
        Round(f, 5, cBg);
        Color(isHover(AtK::Field, i) ? cDim : cBorder);
        g_rt->DrawRoundedRectangle(D2D1::RoundedRect(f, 5, 5), g_brush.Get(), 1.f);
        if (g_at.editing != i) {
            const bool mixed = g_at.date[i].empty();
            Color(mixed ? cFaint : cText);
            Text(mixed ? L"differs: left as is" : g_at.date[i], g_fmtName.Get(), f.left + 9, f.top, f.right - 6, f.bottom);
        }
        g_at.hits.push_back({f, AtK::Field, i});
        Round(now, 5, isHover(AtK::Now, i) ? 0x3A3A44 : cBg);
        Color(cText);
        Text(L"Now", g_fmtCenter.Get(), now.left, now.top, now.right, now.bottom);
        g_at.hits.push_back({now, AtK::Now, i});
        if (g_at.editing == i && g_at.edit) {
            RECT sr = ScreenRect(f);
            POINT a{sr.left, sr.top};
            ScreenToClient(g_hwnd, &a);
            const int pxl = MulDiv(9, g_dpi, 96), hh = MulDiv(18, g_dpi, 96);
            SetWindowPos(g_at.edit, HWND_TOP, a.x + pxl, a.y + (sr.bottom - sr.top - hh) / 2, sr.right - sr.left - pxl - 6, hh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        y += 36;
    }
    Color(cFaint);
    Text(L"dd/mm/yyyy  hh:mm   ·   Enter applies, Esc closes", g_fmtStatus.Get(), cl, y, cr - 200, y + 24);
    // buttons
    const D2D1_RECT_F apply = D2D1::RectF(cr - 90, B - 44, cr, B - 14), cancel = D2D1::RectF(apply.left - 90, B - 44, apply.left - 8, B - 14);
    Round(apply, 6, cAccent, isHover(AtK::Apply) ? 1.f : 0.85f);
    Color(0xFFFFFF);
    Text(L"Apply", g_fmtCenter.Get(), apply.left, apply.top, apply.right, apply.bottom);
    Round(cancel, 6, isHover(AtK::Cancel) ? 0x3A3A44 : cBg);
    Color(cText);
    Text(L"Cancel", g_fmtCenter.Get(), cancel.left, cancel.top, cancel.right, cancel.bottom);
    g_at.hits.push_back({apply, AtK::Apply});
    g_at.hits.push_back({cancel, AtK::Cancel});
}

// ---- mouse and keyboard
int AtHitAt(float x, float y) {
    for (int i = static_cast<int>(g_at.hits.size()) - 1; i >= 0; --i)
        if (InRect(g_at.hits[i].r, x, y)) return i;
    return -1;
}

void AttrsMouseMove(float x, float y) {
    const int h = AtHitAt(x, y);
    const bool was = g_at.hovering;
    const AtHit old = g_at.hover;
    g_at.hovering = h >= 0;
    g_at.hover = h >= 0 ? g_at.hits[h] : AtHit{};
    if (was != g_at.hovering || old.k != g_at.hover.k || old.idx != g_at.hover.idx) Invalidate();
}

void AttrsClick(float x, float y) {
    AtCommitText();
    if (!InRect(g_at.box, x, y)) {
        AttrsClose();
        return;
    }
    const int hi = AtHitAt(x, y);
    if (hi < 0) return;
    const AtHit h = g_at.hits[hi];
    switch (h.k) {
        case AtK::Close: case AtK::Cancel: AttrsClose(); return;
        case AtK::Apply: AttrsApply(); return;
        case AtK::Hidden: g_at.hidden = g_at.hidden == 1 ? 0 : 1; break;  // mixed goes to on, then off
        case AtK::ReadOnly: g_at.readOnly = g_at.readOnly == 1 ? 0 : 1; break;
        case AtK::Field: AtBeginText(h.idx); return;
        case AtK::Now: {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            g_at.date[h.idx] = AtDateText((static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime);
            break;
        }
        default: break;
    }
    Invalidate();
}

void AttrsKey(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE: AttrsClose(); break;
        case VK_RETURN: AttrsApply(); break;
        case VK_TAB: AtBeginText(0); break;
        default: break;
    }
}

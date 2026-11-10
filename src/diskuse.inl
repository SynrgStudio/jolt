// Disk usage (Ctrl+K → "disk usage"): a panel over the window with what takes the space in this folder. Its folders,
// biggest first, with a bar for their share, and one line for the files loose in it. Enter goes down into a folder,
// Backspace up. The sizes are the folder sizes (dirsize.inl): computing a folder gives every folder inside it too, so
// going down is instant. Included from app.cpp after tidy.inl.

struct DuRow {
    std::wstring name;  // empty: the files loose in this folder
    int entry = -1;     // in g_du.list
    uint64_t bytes = 0, files = 0;
    bool done = false;  // its size is final
};

enum class DuK { None, Row, Up, Close };
struct DuHit {
    D2D1_RECT_F r;
    DuK k;
    int row = 0;
};

struct DiskUsePanel {
    bool open = false;
    std::wstring dir;   // the folder shown
    Listing list;       // what is in it
    std::wstring cur;   // the row the cursor is on, by name (the order changes while sizes come in); L"\1" files
    bool see = false;   // scroll to the cursor at the next draw
    float scroll = 0, viewH = 0;
    D2D1_RECT_F box{};
    std::vector<DuHit> hits;
    DuHit hover{};
    bool hovering = false;
} g_du;
std::unordered_set<std::wstring> g_duFresh;  // folders computed from here this session: inside them nothing is asked again

constexpr float kDuRow = 28.f, kDuHead = 64.f, kDuFoot = 34.f;
const std::wstring kDuFiles = L"\1";

std::wstring DuKey(const DuRow& r) { return r.name.empty() ? kDuFiles : r.name; }

std::wstring DuCount(uint64_t n) { return Thousands(static_cast<int>(std::min<uint64_t>(n, INT_MAX))); }

std::vector<DuRow> DuRows() {
    std::vector<DuRow> v;
    DuRow loose;
    loose.done = true;
    const Listing& l = g_du.list;
    for (size_t i = 0; i < l.entries.size(); ++i) {
        const Entry& e = l.entries[i];
        if (!e.isDir()) {
            loose.bytes += e.size;
            ++loose.files;
            continue;
        }
        if (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // links aren't followed: what they hold is elsewhere
        DuRow r;
        r.name.assign(l.name(e), e.nameLen);
        r.entry = static_cast<int>(i);
        DirSize ds;
        if (DirSizeOf(Key(Join(g_du.dir, r.name.c_str())), ds)) {
            r.bytes = ds.bytes;
            r.files = ds.files;
            r.done = ds.done;
        }
        v.push_back(std::move(r));
    }
    if (loose.files) v.push_back(loose);
    std::sort(v.begin(), v.end(), [](const DuRow& a, const DuRow& b) {
        if (a.bytes != b.bytes) return a.bytes > b.bytes;
        if (a.name.empty() != b.name.empty()) return b.name.empty();
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    return v;
}

int DuCursor(const std::vector<DuRow>& rows) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (DuKey(rows[i]) == g_du.cur) return static_cast<int>(i);
    return rows.empty() ? -1 : 0;
}

bool DuFresh(const std::wstring& dir) {
    const std::wstring k = Key(dir);
    for (const std::wstring& f : g_duFresh)
        if (k == f || k.rfind(f.back() == L'\\' ? f : f + L"\\", 0) == 0) return true;
    return false;
}

void DuCalc(const std::wstring& dir) {
    g_duFresh.insert(Key(dir));
    DirSizeRequest({dir});
}

// Shows dir; select: the row the cursor starts on (the folder we came up from).
void DuGo(const std::wstring& dir, const std::wstring& select = L"") {
    g_du.dir = dir;
    EnumerateDir(dir, EnumMethod::Nt, g_du.list);
    g_du.cur = select;  // empty: the first row, the biggest, whichever that turns out to be as the sizes come in
    g_du.scroll = 0;
    g_du.see = true;
    if (!DuFresh(dir)) DuCalc(dir);
    Invalidate();
}

void DiskUseOpen() {
    Tab& t = T();
    if (t.path.empty() || IsVirtual(t.path) || IsBin(t.path) || InArchive(t.path)) {
        S.message = L"Disk usage is for a folder or a drive: open one first";
        Invalidate();
        return;
    }
    g_du = DiskUsePanel{};
    g_du.open = true;
    DuGo(t.path);
}

void DiskUseClose() {
    g_du.open = false;
    g_du.list = Listing{};
    Invalidate();
}

void DuDown(const DuRow& r) {
    if (r.name.empty()) return;
    DuGo(Join(g_du.dir, r.name.c_str()));
}

void DuUp() {
    const std::wstring up = ParentOf(g_du.dir);
    if (up.empty()) return;  // a drive: nothing above it but the list of drives
    DuGo(up, LeafOf(g_du.dir));
}

// The tab goes there: the folder under the cursor, or the one shown.
void DuShowInList(const std::vector<DuRow>& rows) {
    const int c = DuCursor(rows);
    const std::wstring to = c >= 0 && !rows[c].name.empty() ? Join(g_du.dir, rows[c].name.c_str()) : g_du.dir;
    DiskUseClose();
    if (Key(to) != Key(T().path)) Navigate(T(), to, true);
}

std::wstring DuDump() {
    if (!g_du.open) return L"closed";
    std::vector<DuRow> rows = DuRows();
    uint64_t total = 0;
    for (const DuRow& r : rows) total += r.bytes;
    std::wstring out = g_du.dir + L": " + std::to_wstring(total) + L" bytes" + (g_dsBusy ? L", calculating" : L"");
    const int c = DuCursor(rows);
    for (size_t i = 0; i < rows.size() && i < 40; ++i)
        out += std::wstring(L"\n  ") + (static_cast<int>(i) == c ? L"> " : L"  ") + (rows[i].name.empty() ? L"(files here)" : rows[i].name) + L" " +
               std::to_wstring(rows[i].bytes) + L" bytes, " + std::to_wstring(rows[i].files) + L" files" + (rows[i].done ? L"" : L" …");
    return out;
}

// ---- drawing
void DrawDiskUse(float W, float H) {
    if (!g_du.open) return;
    g_du.hits.clear();
    const std::vector<DuRow> rows = DuRows();
    const int n = static_cast<int>(rows.size()), cur = DuCursor(rows);
    uint64_t total = 0, files = 0;
    for (const DuRow& r : rows) {
        total += r.bytes;
        files += r.files;
    }
    const float pw = std::min(860.f, W - 40), ph = H - kTabH - 32;
    const float x0 = std::floor((W - pw) / 2), y0 = kTabH + 16, R = x0 + pw, B = y0 + ph;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_du.box = D2D1::RectF(x0, y0, R, B);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x0 - i, y0 - i + 2, R + i, B + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_du.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_du.box, 12, 12), g_brush.Get(), 1.f);
    auto isHover = [&](DuK k, int row = 0) { return g_du.hovering && g_du.hover.k == k && g_du.hover.row == row; };

    // title: the folder, and above it a way up
    wchar_t sz[64];
    const bool canUp = !ParentOf(g_du.dir).empty();
    float tx = x0 + 48;
    if (canUp) {
        const D2D1_RECT_F ub = D2D1::RectF(x0 + 12, y0 + 16, x0 + 42, y0 + 46);
        if (isHover(DuK::Up)) Round(ub, 6, 0xFFFFFF, 0.08f);
        Color(isHover(DuK::Up) ? cText : cDim);
        Glyph(0xE74A, g_fmtIconSm.Get(), ub.left, ub.top, ub.right, ub.bottom);  // up arrow
        g_du.hits.push_back({ub, DuK::Up});
    } else {
        Color(cAccent);
        Glyph(0xEDA2, g_fmtIcon.Get(), x0 + 16, y0 + 16, x0 + 40, y0 + 44);  // a drive
    }
    Color(cText);
    Text(L"Disk usage  ·  " + (IsDriveRoot(g_du.dir) ? g_du.dir : LeafOf(g_du.dir)), g_fmtNameBold.Get(), tx, y0 + 10, R - 60, y0 + 32);
    FormatSize(total, sz, 64);
    std::wstring sub = std::wstring(sz) + L" in " + DuCount(files) + (files == 1 ? L" file" : L" files");
    ULARGE_INTEGER freeB{}, totalB{};
    if (IsDriveRoot(g_du.dir) && GetDiskFreeSpaceExW(g_du.dir.c_str(), &freeB, &totalB, nullptr) && totalB.QuadPart) {
        wchar_t a[64], b[64];
        FormatSize(freeB.QuadPart, a, 64);
        FormatSize(totalB.QuadPart, b, 64);
        sub += std::wstring(L"  ·  ") + a + L" free of " + b;
    }
    if (g_dsBusy) sub += L"  ·  calculating…";
    Color(cDim);
    Text(sub, g_fmtStatus.Get(), tx, y0 + 31, R - 60, y0 + 52);
    const D2D1_RECT_F cb = D2D1::RectF(R - 42, y0 + 16, R - 12, y0 + 44);
    if (isHover(DuK::Close)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(DuK::Close) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_du.hits.push_back({cb, DuK::Close});
    HLine(x0, R, y0 + kDuHead - 2);

    // the rows, biggest first
    const float top = y0 + kDuHead, bot = B - kDuFoot;
    g_du.viewH = bot - top;
    const float content = n * kDuRow + 8;
    if (g_du.see && cur >= 0) {
        const float ry = cur * kDuRow;
        if (ry < g_du.scroll) g_du.scroll = ry;
        if (ry + kDuRow + 8 > g_du.scroll + g_du.viewH) g_du.scroll = ry + kDuRow + 8 - g_du.viewH;
        g_du.see = false;
    }
    g_du.scroll = std::clamp(g_du.scroll, 0.f, std::max(0.f, content - g_du.viewH));
    const float cl = x0 + 16, cr = R - 16;
    const float filesR = cr, filesL = filesR - 96, sizeR = filesL - 10, sizeL = sizeR - 70, pctR = sizeL - 8, pctL = pctR - 44;
    const float barR = pctL - 10, barL = std::max(cl + 200, barR - std::min(240.f, pw * 0.3f));
    const uint64_t biggest = n ? std::max<uint64_t>(1, rows[0].bytes) : 1;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, top, R, bot), D2D1_ANTIALIAS_MODE_ALIASED);
    if (!n) {
        Color(cFaint);
        Text(g_du.list.error ? L"Couldn't read it: " + ErrorText(g_du.list.error) : std::wstring(L"Nothing in this folder"), g_fmtCenter.Get(), x0, top + 20, R,
             top + 50);
    }
    for (int i = 0; i < n; ++i) {
        const float y = top + 4 + i * kDuRow - g_du.scroll;
        if (y + kDuRow < top) continue;
        if (y > bot) break;
        const DuRow& r = rows[i];
        const D2D1_RECT_F rr = D2D1::RectF(cl - 6, y, cr + 6, y + kDuRow);
        if (i == cur) Round(rr, 5, cAccent, 0.18f);
        else if (isHover(DuK::Row, i)) Round(rr, 5, 0xFFFFFF, 0.05f);
        g_du.hits.push_back({rr, DuK::Row, i});
        const float iy = y + (kDuRow - kIcon) / 2;
        if (r.name.empty()) {
            Color(cDim);
            Glyph(0xE7C3, g_fmtIconSm.Get(), cl, y, cl + kIcon, y + kDuRow);  // a page
        } else {
            const Entry& e = g_du.list.entries[r.entry];
            DrawIcon(Join(g_du.dir, r.name.c_str()), &e, g_du.list.name(e), e.nameLen, false, cl, iy, 1.f);
        }
        Color(r.name.empty() ? cDim : cText);
        Text(r.name.empty() ? L"Files in this folder" : r.name, g_fmtName.Get(), cl + 26, y, barL - 12, y + kDuRow);
        // the bar: its share of the biggest, so the first one fills it
        const D2D1_RECT_F bar = D2D1::RectF(barL, y + 10, barR, y + kDuRow - 10);
        Round(bar, 3, 0xFFFFFF, 0.05f);
        const float f = static_cast<float>(static_cast<double>(r.bytes) / static_cast<double>(biggest));
        if (r.bytes) Round(D2D1::RectF(bar.left, bar.top, bar.left + std::max(3.f, (bar.right - bar.left) * f), bar.bottom), 3, r.name.empty() ? cDim : cAccent, r.done ? 0.85f : 0.4f);
        wchar_t pct[16];
        const double share = total ? 100.0 * static_cast<double>(r.bytes) / static_cast<double>(total) : 0;
        if (share >= 10 || share == 0) swprintf_s(pct, L"%.0f%%", share);
        else swprintf_s(pct, L"%.1f%%", share);
        Color(cDim);
        Text(pct, g_fmtRight.Get(), pctL, y, pctR, y + kDuRow);
        if (r.done || r.bytes) {
            FormatSize(r.bytes, sz, 64);
            Color(r.done ? cText : cDim);
            Text(std::wstring(sz) + (r.done ? L"" : L"…"), g_fmtRight.Get(), sizeL, y, sizeR, y + kDuRow);
        } else {
            Color(cFaint);
            Text(L"…", g_fmtRight.Get(), sizeL, y, sizeR, y + kDuRow);
        }
        Color(cFaint);
        Text(DuCount(r.files) + (r.files == 1 ? L" file" : L" files"), g_fmtRight.Get(), filesL, y, filesR, y + kDuRow);
    }
    g_rt->PopAxisAlignedClip();
    if (content > g_du.viewH) {
        const float th = std::max(24.f, g_du.viewH * g_du.viewH / content);
        const float ty = top + (g_du.viewH - th) * (g_du.scroll / (content - g_du.viewH));
        Round(D2D1::RectF(R - 7, ty, R - 4, ty + th), 1.5f, 0xFFFFFF, 0.18f);
    }
    HLine(x0, R, bot);
    Color(cFaint);
    Text(L"Enter goes into a folder  ·  Backspace goes up  ·  Ctrl+Enter shows it in the list  ·  Ctrl+R calculates again  ·  Esc closes",
         g_fmtStatus.Get(), cl, bot, cr, B);
}

// ---- mouse and keyboard
int DuHitAt(float x, float y) {
    for (int i = static_cast<int>(g_du.hits.size()) - 1; i >= 0; --i)
        if (InRect(g_du.hits[i].r, x, y)) return i;
    return -1;
}

void DiskUseMouseMove(float x, float y) {
    const int h = DuHitAt(x, y);
    const bool was = g_du.hovering;
    const DuHit old = g_du.hover;
    g_du.hovering = h >= 0;
    g_du.hover = h >= 0 ? g_du.hits[h] : DuHit{};
    if (was != g_du.hovering || old.k != g_du.hover.k || old.row != g_du.hover.row) Invalidate();
}

void DiskUseClick(float x, float y, bool twice) {
    if (!InRect(g_du.box, x, y)) {
        DiskUseClose();
        return;
    }
    const int hi = DuHitAt(x, y);
    if (hi < 0) return;
    const DuHit h = g_du.hits[hi];
    if (h.k == DuK::Close) DiskUseClose();
    if (h.k == DuK::Up) DuUp();
    if (h.k == DuK::Row) {
        const std::vector<DuRow> rows = DuRows();
        if (h.row >= static_cast<int>(rows.size())) return;
        g_du.cur = DuKey(rows[h.row]);
        if (twice) DuDown(rows[h.row]);
        Invalidate();
    }
}

void DiskUseWheel(int delta) {
    g_du.scroll -= delta / 120.f * 3 * kDuRow;  // clamped when drawn
    Invalidate();
}

void DiskUseKey(WPARAM vk) {
    const std::vector<DuRow> rows = DuRows();
    const int n = static_cast<int>(rows.size()), c = DuCursor(rows);
    const int page = std::max(1, static_cast<int>(g_du.viewH / kDuRow) - 1);
    auto move = [&](int to) {
        if (!n) return;
        g_du.cur = DuKey(rows[std::clamp(to, 0, n - 1)]);
        g_du.see = true;
        Invalidate();
    };
    switch (vk) {
        case VK_ESCAPE: DiskUseClose(); break;
        case VK_DOWN: move(c + 1); break;
        case VK_UP: move(c - 1); break;
        case VK_NEXT: move(c + page); break;
        case VK_PRIOR: move(c - page); break;
        case VK_HOME: move(0); break;
        case VK_END: move(n - 1); break;
        case VK_RETURN:
            if (KeyDown(VK_CONTROL)) DuShowInList(rows);
            else if (c >= 0) DuDown(rows[c]);
            break;
        case VK_RIGHT:
            if (c >= 0) DuDown(rows[c]);
            break;
        case VK_BACK: case VK_LEFT: DuUp(); break;
        case 'R':
            if (KeyDown(VK_CONTROL)) DuCalc(g_du.dir);
            break;
        default: break;
    }
}

// Rename several items at once: a panel with the new name as a pattern, find/replace (plain or /regex/)
// and a live preview of every result, with conflicts marked. F2 on a multiple selection opens it
// (Ctrl+F2 always). Included from app.cpp after install.inl.

struct RnItem {
    std::wstring dir, oldName, newName;
    bool isDir = false;
    int64_t mtime = 0;
    int problem = 0;  // 0 fine, 1 invalid characters, 2 exists already, 3 repeated among the results
};
enum class RnK { None, Edit0, Edit1, Edit2, Rename, Cancel };
struct RnHit {
    D2D1_RECT_F r;
    RnK k;
};
struct RnData {
    bool open = false;
    std::vector<RnItem> items;
    HWND edit[3] = {};       // pattern, find, replace
    int focus = 0;           // which edit has the keyboard
    std::vector<RnHit> hits;
    RnK hover = RnK::None;
    D2D1_RECT_F box{};
    int changing = 0, problems = 0;
    std::wstring regexError;
    int scroll = 0;  // first preview row shown
} g_rn;

constexpr float kRnW = 700.f, kRnHead = 58.f, kRnEditRow = 40.f, kRnHint = 30.f, kRnRow = 24.f, kRnFoot = 54.f;
constexpr int kRnMaxRows = 14;
constexpr UINT32 kRnBad = 0xE06C75;

std::wstring RnText(int i) {
    if (!g_rn.edit[i]) return L"";
    wchar_t buf[1024];
    GetWindowTextW(g_rn.edit[i], buf, 1024);
    return buf;
}

// The pattern applied to one item: {name} {NAME} {Name} {ext} {n} {n:3} {n:3:10} {date} {parent}.
std::wstring RnExpand(const std::wstring& pat, const std::wstring& name, const std::wstring& ext, const RnItem& it, int index) {
    std::wstring out;
    for (size_t i = 0; i < pat.size();) {
        if (pat[i] != L'{') {
            out += pat[i++];
            continue;
        }
        size_t close = pat.find(L'}', i);
        if (close == std::wstring::npos) {
            out += pat.substr(i);
            break;
        }
        const std::wstring tag = pat.substr(i + 1, close - i - 1), low = Lower(tag);
        i = close + 1;
        if (low == L"name") {
            if (tag == L"NAME") {
                std::wstring u = name;
                CharUpperBuffW(u.data(), static_cast<DWORD>(u.size()));
                out += u;
            } else if (tag == L"Name") {
                std::wstring t = Lower(name);
                bool start = true;
                for (wchar_t& c : t) {
                    if (start && iswalpha(c)) c = static_cast<wchar_t>(towupper(c));
                    start = !iswalnum(c);
                }
                out += t;
            } else {
                out += tag == L"name" ? name : Lower(name);
            }
        } else if (low == L"ext") {
            out += tag == L"EXT" ? [&] { std::wstring u = ext; CharUpperBuffW(u.data(), static_cast<DWORD>(u.size())); return u; }() : ext;
        } else if (low == L"n" || low.rfind(L"n:", 0) == 0) {
            int pad = 1, start = 1;
            if (low.size() > 2) {
                pad = std::max(1, _wtoi(low.c_str() + 2));
                size_t c2 = low.find(L':', 2);
                if (c2 != std::wstring::npos) start = _wtoi(low.c_str() + c2 + 1);
            }
            wchar_t b[32];
            swprintf(b, 32, L"%0*d", std::min(pad, 12), start + index);
            out += b;
        } else if (low == L"date") {
            FILETIME ft{static_cast<DWORD>(it.mtime & 0xFFFFFFFF), static_cast<DWORD>(it.mtime >> 32)}, lt;
            SYSTEMTIME st;
            wchar_t b[32] = L"";
            if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st)) swprintf(b, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            out += b;
        } else if (low == L"parent") {
            out += LeafOf(it.dir);
        } else {
            out += L"{" + tag + L"}";  // not a placeholder: left as typed
        }
    }
    return out;
}

// Recomputes every new name from the three boxes and marks the problems.
void RnRefresh() {
    std::wstring pat = RnText(0), find = RnText(1), repl = RnText(2);
    if (pat.empty()) pat = L"{name}{ext}";
    g_rn.regexError.clear();
    std::unique_ptr<std::wregex> re;
    bool regex = find.size() >= 2 && find.front() == L'/' && find.back() == L'/';
    if (regex) {
        try {
            re = std::make_unique<std::wregex>(find.substr(1, find.size() - 2), std::regex::icase | std::regex::ECMAScript);
        } catch (const std::regex_error&) {
            g_rn.regexError = L"The regular expression isn't valid";
            re.reset();
        }
    }
    std::wstring lowFind = Lower(find);
    int index = 0;
    for (RnItem& it : g_rn.items) {
        std::wstring name = it.oldName, ext;
        if (!it.isDir) {
            size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) {
                ext = name.substr(dot);
                name.resize(dot);
            }
        }
        if (re) {
            name = std::regex_replace(name, *re, repl);
        } else if (!find.empty() && !regex) {
            // plain find: every occurrence, ignoring case
            std::wstring low = Lower(name), out;
            size_t p = 0;
            for (size_t h = low.find(lowFind); h != std::wstring::npos; h = low.find(lowFind, p)) {
                out += name.substr(p, h - p) + repl;
                p = h + lowFind.size();
            }
            name = out + name.substr(p);
        }
        std::wstring n = RnExpand(pat, name, ext, it, index++);
        while (!n.empty() && (n.back() == L' ' || n.back() == L'.')) n.pop_back();
        it.newName = n;
        it.problem = 0;
        if (n.empty() || n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) it.problem = 1;
    }
    // repeated results, and names already taken by something that isn't being renamed
    std::unordered_map<std::wstring, int> seen;
    for (RnItem& it : g_rn.items) ++seen[Key(Join(it.dir, it.newName.c_str()))];
    std::unordered_set<std::wstring> sources;
    for (const RnItem& it : g_rn.items) sources.insert(Key(Join(it.dir, it.oldName.c_str())));
    g_rn.changing = g_rn.problems = 0;
    for (RnItem& it : g_rn.items) {
        const std::wstring to = Join(it.dir, it.newName.c_str());
        const bool changes = it.newName != it.oldName;
        if (!it.problem && changes && seen[Key(to)] > 1) it.problem = 3;
        if (!it.problem && changes && Key(to) != Key(Join(it.dir, it.oldName.c_str())) && !sources.count(Key(to)) && PathExists(to)) it.problem = 2;
        if (it.problem) ++g_rn.problems;
        else if (changes) ++g_rn.changing;
    }
}

LRESULT CALLBACK RnEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR);
void RenameManyApply();

void RenameManyClose() {
    if (!g_rn.open) return;
    g_rn.open = false;
    for (HWND e : g_rn.edit)
        if (e) ShowWindow(e, SW_HIDE);
    g_rn.items.clear();
    g_rn.hits.clear();
    SetFocus(g_hwnd);
    Invalidate();
}

void RnFocus(int i) {
    g_rn.focus = (i + 3) % 3;
    if (g_rn.edit[g_rn.focus]) {
        SetFocus(g_rn.edit[g_rn.focus]);
        SendMessageW(g_rn.edit[g_rn.focus], EM_SETSEL, 0, -1);
    }
}

// Opens the panel for the selection (or the cursor row).
void RenameManyOpen() {
    Tab& t = T();
    std::vector<RnItem> items;
    for (int i = 0; i < RowCount(t); ++i) {
        if (!IsSel(t, i) || IsHeader(t, i) || RowDir(t, i).empty()) continue;
        const Entry& e = EntryAt(t, i);
        items.push_back({RowDir(t, i), RowName(t, i), L"", e.isDir(), e.mtime});
    }
    if (items.empty()) return;
    PaletteClose();
    CancelRename();
    PopupClose();
    g_rn.items = std::move(items);
    g_rn.open = true;
    g_rn.hover = RnK::None;
    g_rn.scroll = 0;
    for (int i = 0; i < 3; ++i) {
        if (!g_rn.edit[i]) {
            g_rn.edit[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetWindowSubclass(g_rn.edit[i], RnEditProc, 10 + i, 0);
        }
        SendMessageW(g_rn.edit[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
        SendMessageW(g_rn.edit[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    SetWindowTextW(g_rn.edit[0], L"{name}{ext}");
    SetWindowTextW(g_rn.edit[1], L"");
    SetWindowTextW(g_rn.edit[2], L"");
    RnRefresh();
    Invalidate();
    RnFocus(0);
}

LRESULT CALLBACK RnEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (!g_rn.open) return DefSubclassProc(h, m, w, l);
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        RenameManyApply();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        RenameManyClose();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_TAB) {
        RnFocus(static_cast<int>(id - 10) + (KeyDown(VK_SHIFT) ? -1 : 1));
        return 0;
    }
    if (m == WM_KEYDOWN && w == 'A' && KeyDown(VK_CONTROL)) {  // the EDIT control doesn't select all on its own
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB || w == 1)) return 0;
    if (m == WM_SETFOCUS) g_rn.focus = static_cast<int>(id - 10);
    LRESULT r = DefSubclassProc(h, m, w, l);
    if (m == WM_CHAR || m == WM_PASTE || m == WM_CUT || m == WM_CLEAR || (m == WM_KEYDOWN && (w == VK_DELETE || w == VK_BACK))) {
        RnRefresh();
        Invalidate();
    }
    return r;
}

// Renames everything that changes. When a new name is another item's old one, it goes through
// temporary names first, so shifts like 1->2->3 work. One undo step for all of it.
void RenameManyApply() {
    if (!g_rn.open || g_rn.problems || !g_rn.changing) {
        if (g_rn.open && g_rn.problems) MessageBeep(MB_ICONWARNING);
        return;
    }
    std::vector<RnItem> items;
    std::unordered_set<std::wstring> sources;
    for (const RnItem& it : g_rn.items)
        if (it.newName != it.oldName) {
            items.push_back(it);
            sources.insert(Key(Join(it.dir, it.oldName.c_str())));
        }
    bool viaTemp = false;
    for (const RnItem& it : items) viaTemp |= sources.count(Key(Join(it.dir, it.newName.c_str()))) > 0;
    Tab& t = T();
    std::vector<UndoItem> done;
    int failed = 0;
    std::vector<std::wstring> current;  // where each item is right now
    for (const RnItem& it : items) current.push_back(Join(it.dir, it.oldName.c_str()));
    if (viaTemp) {
        for (size_t i = 0; i < items.size(); ++i) {
            const std::wstring tmp = Join(items[i].dir, (items[i].oldName + L".jolt-renaming-" + std::to_wstring(i)).c_str());
            if (MoveFileExW(ToExtendedPath(current[i]).c_str(), ToExtendedPath(tmp).c_str(), 0)) current[i] = tmp;
        }
    }
    std::wstring firstNew;
    for (size_t i = 0; i < items.size(); ++i) {
        const std::wstring from = Join(items[i].dir, items[i].oldName.c_str()), to = Join(items[i].dir, items[i].newName.c_str());
        if (MoveFileExW(ToExtendedPath(current[i]).c_str(), ToExtendedPath(to).c_str(), 0)) {
            UndoItem u;
            u.from = from;
            u.to = to;
            done.push_back(u);
            for (auto& tab : g_tabs) TreeFollow(*tab, from, to);
            if (firstNew.empty()) firstNew = to;
        } else {
            ++failed;
            if (current[i] != from) MoveFileExW(ToExtendedPath(current[i]).c_str(), ToExtendedPath(from).c_str(), 0);  // back from the temporary name
        }
    }
    const std::wstring dir = items.empty() ? t.path : items[0].dir;
    RenameManyClose();
    if (!done.empty()) UndoPush({UndoKind::Rename, done});
    if (!firstNew.empty() && Key(ParentOf(firstNew)) == Key(t.path)) t.pendingSelect = LeafOf(firstNew);
    S.message = Thousands(static_cast<int>(done.size())) + (done.size() == 1 ? L" item renamed" : L" items renamed") +
                (failed ? L"   ·   " + Thousands(failed) + L" couldn't be renamed" : L"");
    std::unordered_set<std::wstring> dirs;
    for (const RnItem& it : items) dirs.insert(it.dir);
    for (const std::wstring& d : dirs) Enqueue(d, JobKind::Revalidate);
    Invalidate();
}

void RnPlaceEdit(int i, const D2D1_RECT_F& r) {
    if (!g_rn.edit[i]) return;
    RECT sr = ScreenRect(r);
    POINT a{sr.left, sr.top};
    ScreenToClient(g_hwnd, &a);
    const int pxl = MulDiv(8, g_dpi, 96), hh = MulDiv(18, g_dpi, 96);
    const int top = a.y + (sr.bottom - sr.top - hh) / 2;
    SetWindowPos(g_rn.edit[i], HWND_TOP, a.x + pxl, top, sr.right - sr.left - 2 * pxl, hh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void DrawRenamePanel(float W, float H) {
    if (!g_rn.open) return;
    g_rn.hits.clear();
    const int n = static_cast<int>(g_rn.items.size()), rows = std::min(n, kRnMaxRows);
    const float h = kRnHead + 2 * kRnEditRow + kRnHint + rows * kRnRow + 12 + kRnFoot;
    const float x = std::floor((W - kRnW) / 2), y = std::floor(std::max(20.f, (H - h) / 2));
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_rn.box = D2D1::RectF(x, y, x + kRnW, y + h);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x - i, y - i + 2, x + kRnW + i, y + h + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_rn.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_rn.box, 12, 12), g_brush.Get(), 1.f);

    Color(cAccent);
    Glyph(0xE8AC, g_fmtIcon.Get(), x + 20, y + 16, x + 44, y + 44);
    Color(cText);
    Text(L"Rename " + Thousands(n) + (n == 1 ? L" item" : L" items"), g_fmtName.Get(), x + 50, y + 12, x + kRnW - 20, y + 32);
    Color(cDim);
    Text(L"Every change shows below before anything is renamed. Ctrl+Z takes it all back.", g_fmtName.Get(), x + 50, y + 30, x + kRnW - 20, y + 50);

    auto field = [&](int i, const wchar_t* label, float l, float r, float ry) {
        Color(cDim);
        Text(label, g_fmtName.Get(), l, ry + 9, l + 64, ry + 31);
        const D2D1_RECT_F e = D2D1::RectF(l + 68, ry + 5, r, ry + 35);
        Round(e, 6, cSide);
        Color(g_rn.focus == i ? cAccent : cBorder);
        g_rt->DrawRoundedRectangle(D2D1::RoundedRect(e, 6, 6), g_brush.Get(), 1.f);
        g_rn.hits.push_back({e, static_cast<RnK>(static_cast<int>(RnK::Edit0) + i)});
        RnPlaceEdit(i, e);
    };
    float ry = y + kRnHead;
    field(0, L"New name", x + 20, x + kRnW - 20, ry);
    ry += kRnEditRow;
    const float half = x + 20 + (kRnW - 40) / 2;
    field(1, L"Find", x + 20, half - 8, ry);
    field(2, L"Replace", half + 8, x + kRnW - 20, ry);
    ry += kRnEditRow;
    Color(cFaint);
    Text(g_rn.regexError.empty() ? L"{name}  {NAME}  {Name}  {ext}  {n}  {n:3}  {n:3:10}  {date}  {parent}     Find: text, or /regex/ with $1 in Replace"
                                 : g_rn.regexError,
         g_fmtStatus.Get(), x + 24, ry + 2, x + kRnW - 20, ry + kRnHint);
    ry += kRnHint;
    HLine(x + 20, x + kRnW - 20, ry + 2);
    ry += 8;
    // preview: old -> new
    g_rn.scroll = std::clamp(g_rn.scroll, 0, std::max(0, n - rows));
    const float mid = x + 20 + (kRnW - 40) * 0.47f;
    for (int i = 0; i < rows; ++i) {
        const RnItem& it = g_rn.items[g_rn.scroll + i];
        const float top = ry + i * kRnRow;
        const bool changes = it.newName != it.oldName;
        Color(it.problem ? kRnBad : changes ? cDim : cFaint);
        Text(it.oldName, g_fmtName.Get(), x + 24, top, mid - 24, top + kRnRow);
        Color(it.problem ? kRnBad : changes ? cAccent : cFaint);
        Glyph(0xE72A, g_fmtIconSm.Get(), mid - 20, top, mid, top + kRnRow);
        Color(it.problem ? kRnBad : changes ? cText : cFaint);
        std::wstring shown = it.newName;
        if (it.problem == 1) shown += L"   (invalid name)";
        else if (it.problem == 2) shown += L"   (already exists)";
        else if (it.problem == 3) shown += L"   (repeated)";
        Text(shown, g_fmtName.Get(), mid + 4, top, x + kRnW - 24, top + kRnRow);
    }
    ry += rows * kRnRow + 4;
    if (n > rows) {
        Color(cFaint);
        Text(L"and " + Thousands(n - rows) + L" more (wheel to scroll)", g_fmtStatus.Get(), x + 24, ry - 2, x + kRnW - 24, ry + 18);
    }
    // footer
    const float fy = y + h - kRnFoot;
    HLine(x + 20, x + kRnW - 20, fy);
    std::wstring sum = g_rn.changing ? Thousands(g_rn.changing) + L" will change" : L"Nothing changes yet";
    if (g_rn.problems) sum += L"   ·   " + Thousands(g_rn.problems) + (g_rn.problems == 1 ? L" problem" : L" problems");
    Color(g_rn.problems ? kRnBad : cDim);
    Text(sum, g_fmtName.Get(), x + 24, fy + 15, x + kRnW - 230, fy + 39);
    const D2D1_RECT_F c = D2D1::RectF(x + kRnW - 214, fy + 12, x + kRnW - 130, fy + 42);
    Round(c, 7, 0xFFFFFF, g_rn.hover == RnK::Cancel ? 0.14f : 0.08f);
    Color(cText);
    Text(L"Cancel", g_fmtCenter.Get(), c.left, c.top, c.right, c.bottom);
    g_rn.hits.push_back({c, RnK::Cancel});
    const bool can = g_rn.changing && !g_rn.problems;
    const D2D1_RECT_F b = D2D1::RectF(x + kRnW - 120, fy + 12, x + kRnW - 20, fy + 42);
    Round(b, 7, cAccent, !can ? 0.3f : g_rn.hover == RnK::Rename ? 1.f : 0.85f);
    Color(0xFFFFFF, can ? 1.f : 0.6f);
    Text(L"Rename", g_fmtCenter.Get(), b.left, b.top, b.right, b.bottom);
    if (can) g_rn.hits.push_back({b, RnK::Rename});
}

RnK RnHitAt(float x, float y) {
    for (const RnHit& h : g_rn.hits)
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h.k;
    return RnK::None;
}

void RnHover(float x, float y) {
    const RnK k = RnHitAt(x, y);
    if (k != g_rn.hover) {
        g_rn.hover = k;
        Invalidate();
    }
}

void RnClick(float x, float y) {
    switch (RnHitAt(x, y)) {
        case RnK::Edit0: RnFocus(0); break;
        case RnK::Edit1: RnFocus(1); break;
        case RnK::Edit2: RnFocus(2); break;
        case RnK::Rename: RenameManyApply(); break;
        case RnK::Cancel: RenameManyClose(); break;
        case RnK::None:
            if (x < g_rn.box.left || x >= g_rn.box.right || y < g_rn.box.top || y >= g_rn.box.bottom) RenameManyClose();
            break;
    }
}

void RnWheel(int delta) {
    g_rn.scroll -= delta * 3;
    Invalidate();
}

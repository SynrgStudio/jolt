// Miller columns (Ctrl+Alt+M, per tab): the tab's folder as one column among the folders above it, with what the
// highlighted folder holds in the column after. The tab's own list is the active column (selection, rename, drag and
// drop, the filter: all as usual); the others are drawn here from the listing cache, and a click on them navigates.
// The plain arrows already fit: Left goes up selecting where you came from, Right goes into the highlighted folder.
// Included from app.cpp before the column header.

std::wstring ShortcutFor(Cmd cmd);  // palette.inl

struct MillerHit {
    std::wstring dir, name;  // the column's folder and the item clicked (empty: the column itself)
    int pane = 0;
};
std::vector<MillerHit> g_millerHits;     // HitKind::Miller indexes into this; rebuilt every frame
std::unordered_map<std::wstring, float> g_millerShown;  // the scroll each side column was drawn with
double g_millerClickAt = 0;              // a click on an item: the double click that follows opens it

// The folder the cursor is on in the active column, whose contents go in the next column ("" if it isn't one).
std::wstring MillerNext(const Tab& t) {
    if (!t.list || t.list->error || t.sel < 0 || t.sel >= RowCount(t) || IsHeader(t, t.sel) || !EntryAt(t, t.sel).isDir()) return L"";
    return t.rows[t.sel].l->path.empty() ? RowName(t, t.sel) + L"\\" : RowPath(t, t.sel);
}

std::wstring MillerTitle(const std::wstring& dir) {
    if (dir.empty()) return L"This PC";
    if (IsDriveRoot(dir)) return DriveLabel(dir[0]);
    if (IsVirtual(dir)) return VirtualTitle(dir);
    return LeafOf(dir);
}

// A folder's listing for a side column, as its view shows it: from memory, or asked for (null until it arrives).
// Each one is checked again in the background once per navigation, in case it changed.
std::shared_ptr<const Listing> MillerList(const std::wstring& dir) {
    const std::wstring k = Key(dir);
    auto it = g_cache.find(k);
    if (it != g_cache.end()) {
        uint64_t& gen = g_millerChecked[k];
        if (gen != g_navGen.load() + 1) {
            gen = g_navGen.load() + 1;
            g_millerPending.insert(k);
            Enqueue(dir, JobKind::Revalidate);
        }
        return Prepared(it->second);
    }
    if (g_millerPending.insert(k).second) Enqueue(dir, JobKind::Load);
    return nullptr;
}

// An item in a column: the recent mark, the icon, the name, and a chevron on folders.
void DrawMillerItem(const Entry& e, const wchar_t* name, const std::wstring& full, const D2D1_RECT_F& rr, float y, float a, bool onSel,
                    bool drives, float recent, const std::wstring& filterPat) {
    if (recent > 0) Round(D2D1::RectF(rr.left + 1, y + 6, rr.left + 3.5f, y + kRow - 6), 1.25f, cRecent, recent * a);
    const float x = rr.left + 8;
    DrawIcon(full, &e, name, e.nameLen, drives, x, y + (kRow - kIcon) / 2, a);
    const float tx = x + kIcon + 8, tr = rr.right - (e.isDir() ? 22.f : 6.f);
    UINT32 shownLen = e.nameLen;
    if (g_hideExt && !e.isDir() && !drives)
        for (UINT32 k = e.nameLen; k > 1; --k)
            if (name[k - 1] == L'.') {
                shownLen = k - 1;
                break;
            }
    if (drives) {
        Color(cText, a);
        Text(DriveLabel(name[0]), g_fmtName.Get(), tx, y, tr, y + kRow);
    } else if (!filterPat.empty()) {
        TextHighlighted(std::wstring(name, shownLen), filterPat, g_fmtName.Get(), tx, y, tr, y + kRow, NameColorFor(name, e.nameLen, e.isDir(), cText), cAccent, a);
    } else {
        Color(NameColorFor(name, e.nameLen, e.isDir(), cText), a);
        Text(name, shownLen, g_fmtName.Get(), tx, y, tr, y + kRow);
    }
    if (e.isDir()) {
        Color(onSel ? 0xC6D4EE : cFaint, a);
        Glyph(gChevron, g_fmtIconSm.Get(), rr.right - 20, y, rr.right - 4, y + kRow);
    }
}

// The columns that aren't the tab's own list, and the lines between them. Called with the list's clip on.
void DrawMillerSides(int pane, float top, float bot) {
    Tab& t = PT(pane);
    const MillerGeo m = MillerLayout(t);
    const int d = m.depth();
    const std::wstring next = MillerNext(t);
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const int64_t now = (static_cast<int64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime, since = RecentSince(now);
    for (int col = 0; col < m.n; ++col) {
        const int i = m.first + col;
        const float x0 = m.L + col * m.colW, x1 = x0 + m.colW;
        if (col > 0) Fill(D2D1::RectF(x0, top, x0 + 1, bot), cBorder);
        if (i == d) continue;
        std::wstring dir, onPath;
        if (i < d) {
            dir = m.chain[i];
            onPath = m.chain[i + 1];
        } else if (i == d + 1 && !next.empty()) {
            dir = next;
        } else {
            continue;
        }
        // the column itself: a click on its empty part goes to that folder
        g_hits.push_back({D2D1::RectF(x0, top, x1, bot), HitKind::Miller, static_cast<int>(g_millerHits.size())});
        g_millerHits.push_back({dir, L"", pane});
        const auto l = MillerList(dir);
        if (!l) continue;
        if (l->error) {
            Color(cFaint);
            Text(L"Can't open it", g_fmtCenter.Get(), x0, top, x1, top + 60);
            continue;
        }
        const bool drives = l->path.empty();
        // the one on the way to the tab's folder, by name (a drive by its letter)
        const std::wstring leaf = onPath.empty() ? L"" : drives ? onPath.substr(0, 2) : LeafOf(onPath);
        const int count = static_cast<int>(l->entries.size());
        int mark = -1;
        for (int k = 0; k < count && !leaf.empty(); ++k) {
            const Entry& e = l->entries[k];
            if (e.nameLen == leaf.size() && _wcsnicmp(l->name(e), leaf.c_str(), leaf.size()) == 0) {
                mark = k;
                break;
            }
        }
        // scrolled to show it, unless the wheel moved the column since
        const float h = bot - top, contentH = count * kRow;
        const std::wstring sk = std::to_wstring(pane) + L"|" + Key(dir);
        auto sc = g_millerScroll.find(sk);
        float scroll = sc != g_millerScroll.end() ? sc->second : mark >= 0 && (mark + 1) * kRow > h ? mark * kRow - (h - kRow) / 2 : 0.f;
        scroll = std::clamp(scroll, 0.f, std::max(0.f, contentH - h));
        g_millerShown[sk] = scroll;
        for (int k = static_cast<int>(scroll / kRow); k < count; ++k) {
            const float y = top + k * kRow - scroll;
            if (y >= bot) break;
            const Entry& e = l->entries[k];
            const wchar_t* name = l->name(e);
            const std::wstring full = drives ? std::wstring(name) + L"\\" : Join(l->path, name);
            const D2D1_RECT_F rr = D2D1::RectF(x0 + 4, y + 1, x1 - 4, y + kRow - 1);
            const int hid = static_cast<int>(g_millerHits.size());
            g_millerHits.push_back({dir, std::wstring(name, e.nameLen), pane});
            g_hits.push_back({rr, HitKind::Miller, hid});
            if (k == mark) Round(rr, 5, 0xFFFFFF, 0.11f);  // the way to the tab's folder: a selection without focus
            else if (IsHover(HitKind::Miller, hid)) Round(rr, 5, 0xFFFFFF, 0.045f);
            float a = (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && !drives ? 0.5f : 1.f;
            if (!g_cutKeys.empty() && g_cutKeys.count(Key(full))) a *= 0.45f;
            DrawMillerItem(e, name, full, rr, y, a, false, drives, e.isDir() || drives ? 0.f : RecentAlpha(e.mtime, since, now), L"");
        }
        if (contentH > h) {  // thin scrollbar
            const float thumbH = std::max(24.f, h * h / contentH), thumbY = top + (h - thumbH) * (scroll / (contentH - h));
            Round(D2D1::RectF(x1 - 5, thumbY, x1 - 2, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.14f);
        }
    }
}

// The column titles: each column's folder, the active one brighter.
void DrawMillerTitles(int pane, float y0, float y1, float firstL) {
    const Tab& t = PT(pane);
    const MillerGeo m = MillerLayout(t);
    const std::wstring next = MillerNext(t);
    for (int col = 0; col < m.n; ++col) {
        const int i = m.first + col;
        if (i > m.depth() + 1 || (i == m.depth() + 1 && next.empty())) break;
        const float x0 = m.L + col * m.colW;
        Color(i == m.depth() ? cText : cDim);
        Text(MillerTitle(i <= m.depth() ? m.chain[i] : next), g_fmtHeader.Get(), col == 0 ? std::max(x0 + 12, firstL) : x0 + 12, y0,
             x0 + m.colW - 8, y1);
    }
}

// A click on a side column: there goes the tab, with the item clicked selected (on its empty part, with what was).
void MillerOpen(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_millerHits.size())) return;
    const MillerHit h = g_millerHits[idx];
    ActivatePane(h.pane);
    Tab& t = T();
    if (!h.name.empty()) g_lastSel[Key(h.dir)] = h.name;
    g_millerKeep = true;  // the columns stay where they are: what was clicked doesn't move
    if (Key(h.dir) != Key(t.path)) Navigate(t, h.dir, true);
    else if (!h.name.empty()) {
        const int r = FindRow(t, h.name);
        if (r >= 0) SetSel(r);
    }
    g_millerClickAt = h.name.empty() ? 0 : NowMs();
    Invalidate();
}

// The wheel over a side column scrolls that column.
bool MillerWheel(float x, float y, int delta) {
    const Hit h = HitAt(x, y);
    if (h.kind != HitKind::Miller || h.idx < 0 || h.idx >= static_cast<int>(g_millerHits.size())) return false;
    const MillerHit& mh = g_millerHits[h.idx];
    const std::wstring sk = std::to_wstring(mh.pane) + L"|" + Key(mh.dir);
    auto it = g_millerShown.find(sk);
    g_millerScroll[sk] = (it != g_millerShown.end() ? it->second : 0.f) - delta / 120.f * 3 * kRow;  // clamped when drawn
    Invalidate();
    return true;
}

void ToggleMiller() {
    Tab& t = T();
    if (!t.miller && (t.recursive || IsVirtual(t.path))) {
        S.message = L"Columns: not in a search or a virtual folder";
        Invalidate();
        return;
    }
    t.miller = !t.miller;
    t.millerFirst = -1;
    t.cellsKey = 0;
    const Place p = SavePlace(t);
    Rebuild(t, &p, L"");  // no groups or tree in the columns
    EnsureVisible(t);
    S.message = t.miller ? L"Columns   ·   ← and → move between them   ·   " + ShortcutFor(Cmd::ViewColumns) + L" goes back" : L"Back to the folder's view";
    LayoutRename();
    TouchSession();
    Invalidate();
}

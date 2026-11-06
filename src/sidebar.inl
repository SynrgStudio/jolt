// Sidebar: drag to reorder favorites (or add one from recents and drives), plus its menu.
// Releasing without moving is a normal click: that's why these rows open on button up, not button down.

constexpr float kSideDragMin = 4.f;

bool SideDragStart(const Hit& h, float x, float y) {
    if (h.kind != HitKind::Side || h.idx < 0 || h.idx >= static_cast<int>(g_sidePaths.size()) || g_sidePaths[h.idx].empty()) return false;
    g_sideDrag = {};
    g_sideDrag.press = true;
    g_sideDrag.idx = h.idx;
    g_sideDrag.x0 = g_sideDrag.x = x;
    g_sideDrag.y0 = g_sideDrag.y = y;
    g_sideDrag.path = g_sidePaths[h.idx];
    g_sideDrag.name = h.idx < static_cast<int>(g_favs.size()) ? g_favs[h.idx].name : LeafOf(g_sideDrag.path);
    SetCapture(g_hwnd);
    return true;
}

void SideDragMove(float x, float y) {
    SideDrag& d = g_sideDrag;
    if (!d.active && std::abs(x - d.x0) < kSideDragMin && std::abs(y - d.y0) < kSideDragMin) return;
    d.active = true;
    d.x = x;
    d.y = y;
    d.inFavs = x < kSideW && y >= g_favTop - 24 && y < g_favBottom + 12;
    d.insert = std::clamp(static_cast<int>(std::floor((y - g_favTop + kSideRow / 2) / kSideRow)), 0, static_cast<int>(g_favs.size()));
    Invalidate();
}

void SideDragCancel() {
    if (!g_sideDrag.press) return;
    g_sideDrag = {};
    if (GetCapture() == g_hwnd) ReleaseCapture();
    Invalidate();
}

void SideDragEnd() {
    if (!g_sideDrag.press) return;
    const SideDrag d = g_sideDrag;
    g_sideDrag = {};
    if (GetCapture() == g_hwnd) ReleaseCapture();
    Invalidate();
    if (!d.active) {
        Navigate(T(), d.path, true);
        return;
    }
    if (!d.inFavs) return;
    auto same = [&](const Favorite& v) { return Key(v.path) == Key(d.path); };
    auto it = std::find_if(g_favs.begin(), g_favs.end(), same);
    int at = d.insert;
    Favorite moved{d.name, d.path};
    if (it != g_favs.end()) {  // already a favorite: it changes position
        const int from = static_cast<int>(it - g_favs.begin());
        moved = *it;
        g_favs.erase(it);
        if (at > from) --at;
    }
    g_favs.insert(g_favs.begin() + at, moved);
    SaveFavs();
    HeatRefresh();
}

void DrawSideDrag() {
    const SideDrag& d = g_sideDrag;
    if (!d.active) return;
    const float gy = d.y - kSideRow / 2;
    const D2D1_RECT_F r = D2D1::RectF(6, gy, kSideW - 7, gy + kSideRow);
    Round(r, 5, cTop, 0.8f);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5, 5), g_brush.Get(), 1.f);
    DrawIcon(d.path, nullptr, nullptr, 0, IsDriveRoot(d.path), r.left + 10, gy + 5, 1.f);
    Color(cText);
    Text(d.name, g_fmtSide.Get(), r.left + 34, gy, r.right - 6, gy + kSideRow);
    if (d.inFavs) {
        const float ly = g_favTop + d.insert * kSideRow;
        Round(D2D1::RectF(8, ly - 1, kSideW - 9, ly + 1), 1, cAccent);
    }
}

enum SideCmd : UINT { SideNewTab = 1, SideRename, SideRemoveFav, SideAddFav, SideForget };

void OpenSideMenu(int idx, POINT screenPt) {
    if (idx < 0 || idx >= static_cast<int>(g_sidePaths.size())) return;
    const std::wstring path = g_sidePaths[idx];
    if (path.empty()) return;
    const bool fav = idx < static_cast<int>(g_favs.size()), hot = idx >= g_sideHotFirst;
    std::vector<MenuItem> items;
    auto add = [&](const wchar_t* text, UINT id) {
        MenuItem m;
        m.text = text;
        m.id = id;
        items.push_back(m);
    };
    add(L"Open in new tab", SideNewTab);
    if (fav) {
        add(L"Rename", SideRename);
        add(L"Remove from Favorites", SideRemoveFav);
    } else {
        add(L"Add to Favorites", SideAddFav);
        if (hot) add(L"Remove from Recents", SideForget);
    }
    const RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(std::move(items), anchor, [path, idx](UINT id) {
        switch (id) {
            case SideNewTab: NewTab(path); break;
            case SideRename: BeginFavRename(idx); break;
            case SideRemoveFav: FavRemove(path); break;
            case SideAddFav: FavAdd(path); break;
            case SideForget: HeatForget(path); break;
        }
        Invalidate();
    });
}

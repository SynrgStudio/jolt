// Box selection: drag from empty space or from the free part of a row.
// On the icon or name, and on a multiple selection, dragging still moves the files.
// With no keys the box replaces the selection, Shift adds and Ctrl toggles. Esc restores it.

int HeaderRowIn(int pane, float x, float y);

constexpr UINT_PTR kTimerBox = 11;

struct BoxSel {
    bool active = false, ticking = false;
    int pane = 0;
    Tab* tab = nullptr;
    float x0 = 0, y0 = 0;  // fixed corner; y is measured in the content, so it moves with the list
    float x = 0, y = 0;    // mouse, in the window
    int mode = 0;          // 0 replaces, 1 adds, 2 toggles
    std::vector<uint8_t> base;
} g_box;
std::vector<uint8_t> g_boxBefore;  // the selection before the button was pressed

bool BoxAlive() { return g_box.active && &PT(g_box.pane) == g_box.tab; }

float BoxContentY() {
    return std::clamp(g_box.y, ListTop(), std::max(ListTop(), ListBottom() - 1)) - ListTop() + g_box.tab->scroll;
}

// Is the point on the row's icon or name?
bool OnItem(int row, float x) {
    Tab& t = T();
    if (row < 0 || row >= Count()) return false;
    if (GeoFor(t).grid) return true;  // the whole cell is the item
    const Row& rw = t.rows[row];
    const Entry& e = rw.l->entries[rw.e];
    const Columns c = Cols(g_act);
    const float chevX = c.nameL + RowIndent(t, row), textX = chevX + kChev + 28;
    const wchar_t* name = rw.l->name(e);
    const bool drives = rw.l->path.empty();
    UINT32 shown = e.nameLen;
    if (g_hideExt && !e.isDir() && !drives)
        for (UINT32 k = e.nameLen; k > 1; --k)
            if (name[k - 1] == L'.') {
                shown = k - 1;
                break;
            }
    const float w = MeasureW(drives ? DriveLabel(name[0]) : std::wstring(name, shown), g_fmtName.Get());
    return x >= chevX + kChev && x <= std::min(textX + w + 8, c.nameR);
}

// Does dragging from this point draw a box?
bool BoxCanStart(int row, float x, float y, bool mods) {
    const Columns c = Cols(g_act);
    if (Count() == 0 || x < c.L || x >= c.R || y < ListTop() || y >= ListBottom()) return false;
    if (HitAt(x, y).kind != HitKind::None || HeaderRowIn(g_act, x, y) >= 0) return false;
    return row < 0 || (!OnItem(row, x) && (mods || !IsSel(T(), row) || SelCount(T()) <= 1));
}

void BoxApply() {
    Tab& t = *g_box.tab;
    const int n = RowCount(t);
    if (g_box.base.size() != t.selMask.size()) g_box.base.assign(t.selMask.size(), 0);  // the list changed
    t.selMask = g_box.base;
    const float cy = BoxContentY();
    const float y1 = std::min(g_box.y0, cy), y2 = std::max(g_box.y0, cy);
    const float x1 = std::min(g_box.x0, g_box.x), x2 = std::max(g_box.x0, g_box.x);
    const bool grid = GeoFor(t).grid;  // in the grid the box must also reach the cell sideways
    int lo = -1, hi = -1;
    for (int i = 0; i < n; ++i) {
        if (IsHeader(t, i)) continue;
        const D2D1_RECT_F r = RowRect(t, i);
        if (r.bottom <= y1 || r.top > y2) continue;
        if (grid && (r.right <= x1 || r.left > x2)) continue;
        t.selMask[i] = g_box.mode == 2 ? !g_box.base[i] : 1;
        if (lo < 0) lo = i;
        hi = i;
    }
    if (lo >= 0) {
        t.sel = cy >= g_box.y0 ? hi : lo;
        t.anchor = cy >= g_box.y0 ? lo : hi;
    }
    Invalidate();
}

void BoxArmScroll() {
    if (!g_box.ticking && (g_box.y < ListTop() || g_box.y >= ListBottom())) {
        g_box.ticking = true;
        SetTimer(g_hwnd, kTimerBox, 16, nullptr);
    }
}

void BoxBegin(POINT pressed, float x, float y) {
    Tab& t = T();
    g_box.active = true;
    T().implicitSel = false;
    g_box.ticking = false;
    g_box.pane = g_act;
    g_box.tab = &t;
    g_box.x0 = pressed.x * 96.f / g_dpi;
    g_box.y0 = pressed.y * 96.f / g_dpi - ListTop() + t.scroll;
    g_box.x = x;
    g_box.y = y;
    g_box.mode = KeyDown(VK_CONTROL) ? 2 : KeyDown(VK_SHIFT) ? 1 : 0;
    if (g_box.mode) g_box.base = g_boxBefore;
    else g_box.base.assign(t.selMask.size(), 0);
    g_hoverRow = -1;
    g_hoverHeader = -1;
    BoxApply();
    BoxArmScroll();
}

void BoxEnd() {
    if (!g_box.active) return;
    g_box.active = false;
    g_box.ticking = false;
    g_box.base.clear();
    KillTimer(g_hwnd, kTimerBox);
    if (GetCapture() == g_hwnd) ReleaseCapture();
    SetTimer(g_hwnd, kTimerPrefetch, 150, nullptr);
    Invalidate();
}

void BoxCancel() {
    if (BoxAlive() && g_boxBefore.size() == g_box.tab->selMask.size()) g_box.tab->selMask = g_boxBefore;
    BoxEnd();
}

void BoxMove(float x, float y) {
    if (!BoxAlive()) {
        BoxEnd();
        return;
    }
    g_box.x = x;
    g_box.y = y;
    BoxApply();
    BoxArmScroll();
}

// With the mouse outside the list, it scrolls faster the farther away it is.
void BoxTick() {
    g_box.ticking = false;
    if (!BoxAlive()) {
        BoxEnd();
        return;
    }
    const float over = g_box.y < ListTop() ? g_box.y - ListTop() : g_box.y >= ListBottom() ? g_box.y - ListBottom() + 1 : 0;
    if (over == 0) return;
    const float step = std::clamp(std::abs(over) * 0.4f, 4.f, kRow * 3);
    g_box.tab->scroll += over < 0 ? -step : step;
    ClampScroll(*g_box.tab);
    BoxApply();
    BoxArmScroll();
}

void DrawBox(int pane, float listTop, float listBot) {
    if (!BoxAlive() || g_box.pane != pane) return;
    const Columns c = Cols(pane);
    const float xa = std::clamp(g_box.x0, c.L + 3, c.R - 3), xb = std::clamp(g_box.x, c.L + 3, c.R - 3);
    const float ya = listTop + g_box.y0 - g_box.tab->scroll;
    const float yb = std::clamp(g_box.y, listTop, listBot - 1);
    const D2D1_RECT_F r = D2D1::RectF(std::floor(std::min(xa, xb)) + 0.5f, std::floor(std::min(ya, yb)) + 0.5f,
                                      std::floor(std::max(xa, xb)) + 0.5f, std::floor(std::max(ya, yb)) + 0.5f);
    Round(r, 3, cAccent, 0.16f);
    Color(cAccent, 0.85f);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 3, 3), g_brush.Get(), 1.f);
}

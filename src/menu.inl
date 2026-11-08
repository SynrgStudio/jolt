// Custom popup menus, the Shell context menu and its editor.
// Included from app.cpp (inside the anonymous namespace) to share the drawing code.

// ================================================================ drawing in another window
// The drawing helpers use g_rt/g_brush; to draw in a popup they are swapped for a while.
struct TargetScope {
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    TargetScope(ID2D1RenderTarget* r, ID2D1SolidColorBrush* b) {
        rt = g_rt;
        br = g_brush;
        g_rt = r;
        g_brush = b;
    }
    ~TargetScope() {
        g_rt = rt;
        g_brush = br;
    }
};

bool CreateTargetFor(HWND hwnd, ComPtr<ID2D1HwndRenderTarget>& rt, ComPtr<ID2D1SolidColorBrush>& brush) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(),
                                              static_cast<float>(g_dpi), static_cast<float>(g_dpi));
    auto hprops = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom),
                                                   D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(g_d2d->CreateHwndRenderTarget(props, hprops, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (g_textParams) rt->SetTextRenderingParams(g_textParams.Get());
    rt->CreateSolidColorBrush(D2D1::ColorF(cText), &brush);
    return true;
}

// HBITMAP (from an HMENU) -> Direct2D bitmap for the current render target.
ComPtr<ID2D1Bitmap> BitmapFromHBitmap(HBITMAP hbm) {
    ComPtr<ID2D1Bitmap> out;
    if (!hbm || reinterpret_cast<ULONG_PTR>(hbm) <= 11) return out;  // HBMMENU_* are not real bitmaps
    BITMAP bm{};
    GetObjectW(hbm, sizeof(bm), &bm);
    ComPtr<IWICBitmap> wb;
    WICBitmapAlphaChannelOption alpha = bm.bmBitsPixel == 32 ? WICBitmapUsePremultipliedAlpha : WICBitmapIgnoreAlpha;
    if (FAILED(g_wic->CreateBitmapFromHBITMAP(hbm, nullptr, alpha, &wb))) {
        if (FAILED(g_wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapIgnoreAlpha, &wb))) return out;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g_wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return out;
    g_rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
    return out;
}

// ================================================================ popup menu
struct MenuItem {
    std::wstring text, shortcut, key;
    UINT id = 0;  // 0 = no action
    bool sep = false, enabled = true, checked = false, bold = false;
    HMENU hsub = nullptr;        // Shell submenu (read when opened)
    std::vector<MenuItem> sub;   // own submenu
    HBITMAP hbm = nullptr;       // icon (owned by the HMENU or the ShellMenu)
    bool ownerDrawn = false;     // hbm is the whole row, drawn by the extension
    int icon = -1;               // or a system icon, the one of iconPath as soon as it's known
    std::wstring iconPath;
    bool hasSub() const { return hsub != nullptr || !sub.empty(); }
};

constexpr float kMenuRow = 26.f, kMenuSep = 9.f, kMenuPad = 6.f, kMenuIconX = 12.f, kMenuTextX = 38.f;

struct PopupLevel {
    HWND hwnd = nullptr;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<IWICBitmap> wic;  // test mode: the menu is drawn into a bitmap
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<MenuItem> items;
    int hover = -1;
    int openSub = -1;  // item whose submenu is open
    int mark = -1;     // the item you are on: shown as such, and where the keyboard starts
    float scroll = 0, viewH = 0, contentH = 0;  // a list taller than the menu scrolls
    RECT rc{};
    std::unordered_map<HBITMAP, ComPtr<ID2D1Bitmap>> bmps;
    std::unordered_map<int, ComPtr<ID2D1Bitmap>> icons;
};

struct Popup {
    std::vector<std::unique_ptr<PopupLevel>> levels;
    std::function<void(UINT)> onSelect;
    std::function<void(HMENU, int, const std::wstring&, std::vector<MenuItem>&)> loadSub;  // the item's key too: "new" is Jolt's
    std::function<void(UINT)> onMiddle;  // middle click on an item, in the menus that want it
    int pendingLevel = -1, pendingItem = -1;
    float maxH = 0;  // 0: as tall as the screen
    int mark = -1;
    std::wstring typed;  // what was typed to jump to an item
    DWORD typedAt = 0;
    bool open() const { return !levels.empty(); }
} g_popup;

constexpr UINT_PTR kTimerSub = 7;
constexpr DWORD kPopupTypeMs = 900;  // typing to jump to an item: a pause starts a new word
constexpr wchar_t kPopupClass[] = L"JoltPopup";

float ItemH(const MenuItem& m) { return m.sep ? kMenuSep : kMenuRow; }

void MenuSize(const std::vector<MenuItem>& items, float& w, float& h) {
    w = 160;
    h = 2 * kMenuPad;
    // measuring text is slow: in a folder with thousands of subfolders only the longest names decide the width
    size_t minLen = 0;
    if (items.size() > 256) {
        std::vector<size_t> lens;
        lens.reserve(items.size());
        for (const MenuItem& m : items) lens.push_back(m.text.size());
        std::nth_element(lens.begin(), lens.begin() + 255, lens.end(), std::greater<size_t>());
        minLen = lens[255];
    }
    for (const MenuItem& m : items) {
        h += ItemH(m);
        if (m.sep || m.text.size() < minLen) continue;
        float tw = kMenuTextX + MeasureW(m.text, g_fmtName.Get()) + 28;
        if (!m.shortcut.empty()) tw += MeasureW(m.shortcut, g_fmtRight.Get()) + 24;
        if (m.ownerDrawn && m.hbm) {
            BITMAP bm{};
            GetObjectW(m.hbm, sizeof(bm), &bm);
            tw = kMenuIconX + bm.bmWidth * 96.f / g_dpi + 16;
        }
        w = std::max(w, tw);
    }
    w = std::min(w, 460.f);
}

int ItemAt(const PopupLevel& L, float y) {
    float top = kMenuPad - L.scroll;
    for (size_t i = 0; i < L.items.size(); ++i) {
        float hh = ItemH(L.items[i]);
        if (y >= top && y < top + hh) return static_cast<int>(i);
        top += hh;
    }
    return -1;
}

float ItemTop(const PopupLevel& L, int idx) {
    float top = kMenuPad - L.scroll;
    for (int i = 0; i < idx; ++i) top += ItemH(L.items[i]);
    return top;
}

float MaxScroll(const PopupLevel& L) { return std::max(0.f, L.contentH - L.viewH); }

void EnsureItemVisible(PopupLevel& L, int idx) {
    const float top = ItemTop(L, idx), bot = top + ItemH(L.items[idx]);
    if (top < kMenuPad) L.scroll += top - kMenuPad;
    else if (bot > L.viewH - kMenuPad) L.scroll += bot - (L.viewH - kMenuPad);
    L.scroll = std::clamp(L.scroll, 0.f, MaxScroll(L));
}

bool CreatePopupTarget(PopupLevel& L) {
    if (!g_test) {
        ComPtr<ID2D1HwndRenderTarget> rt;
        if (!CreateTargetFor(L.hwnd, rt, L.brush)) return false;
        L.rt = rt;
        return true;
    }
    RECT rc;
    GetClientRect(L.hwnd, &rc);
    if (FAILED(g_wic->CreateBitmap(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnLoad, &L.wic)))
        return false;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              static_cast<float>(g_dpi), static_cast<float>(g_dpi));
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(L.wic.Get(), props, &L.rt))) return false;
    L.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (g_textParams) L.rt->SetTextRenderingParams(g_textParams.Get());
    L.rt->CreateSolidColorBrush(D2D1::ColorF(cText), &L.brush);
    return true;
}

void DrawLevel(PopupLevel& L) {
    if (!L.rt && !CreatePopupTarget(L)) return;
    TargetScope scope(L.rt.Get(), L.brush.Get());
    D2D1_SIZE_F sz = g_rt->GetSize();
    const float W = sz.width;
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(cTop));
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, sz.height - 0.5f), 6, 6),
                               g_brush.Get(), 1.f);
    const bool scrolls = L.contentH > L.viewH;
    if (scrolls) g_rt->PushAxisAlignedClip(D2D1::RectF(1, 1, W - 1, sz.height - 1), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = kMenuPad - L.scroll;
    for (size_t i = 0; i < L.items.size(); ++i) {
        const MenuItem& m = L.items[i];
        float hh = ItemH(m);
        if (y + hh <= 0) {  // above the visible part
            y += hh;
            continue;
        }
        if (y >= sz.height) break;
        if (m.sep) {
            HLine(kMenuIconX, W - kMenuIconX, y + hh / 2);
            y += hh;
            continue;
        }
        bool hov = static_cast<int>(i) == L.hover || static_cast<int>(i) == L.openSub;
        if (hov && m.enabled) Round(D2D1::RectF(5, y + 1, W - 5, y + hh - 1), 5, 0xFFFFFF, 0.09f);
        if (static_cast<int>(i) == L.mark) Round(D2D1::RectF(5, y + 6, 8, y + hh - 6), 1.5f, cAccent);
        int icon = m.icon;
        if (!m.iconPath.empty()) {
            const int known = IconForPathCached(m.iconPath);
            if (known >= 0) icon = known;
            else if (known == -2) RequestIcon(m.iconPath);
        }
        if (icon >= 0) {
            auto it = L.icons.find(icon);
            if (it == L.icons.end()) it = L.icons.emplace(icon, MakeIconBitmap(icon)).first;
            if (it->second)
                g_rt->DrawBitmap(it->second.Get(), D2D1::RectF(kMenuIconX, y + (hh - 16) / 2, kMenuIconX + 16, y + (hh + 16) / 2),
                                 m.enabled ? 1.f : 0.5f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if (m.hbm) {
            auto it = L.bmps.find(m.hbm);
            if (it == L.bmps.end()) it = L.bmps.emplace(m.hbm, BitmapFromHBitmap(m.hbm)).first;
            if (it->second) {
                D2D1_SIZE_F bs = it->second->GetSize();
                if (m.ownerDrawn) {
                    float bw = bs.width, bh = bs.height;
                    g_rt->DrawBitmap(it->second.Get(), D2D1::RectF(kMenuIconX, y + (hh - bh) / 2, kMenuIconX + bw, y + (hh + bh) / 2),
                                     m.enabled ? 1.f : 0.5f);
                } else {
                    float s = std::min(16.f, std::max(bs.width, bs.height));
                    float bw = bs.width * s / std::max(bs.width, bs.height), bh = bs.height * s / std::max(bs.width, bs.height);
                    g_rt->DrawBitmap(it->second.Get(),
                                     D2D1::RectF(kMenuIconX + (16 - bw) / 2, y + (hh - bh) / 2, kMenuIconX + (16 + bw) / 2,
                                                 y + (hh + bh) / 2),
                                     m.enabled ? 1.f : 0.5f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
            }
        } else if (m.checked) {
            Color(cText);
            Glyph(0xE73E, g_fmtIconSm.Get(), kMenuIconX, y, kMenuIconX + 16, y + hh);
        }
        if (!m.ownerDrawn) {
            Color(m.enabled ? cText : cFaint);
            float textR = W - 12 - (m.hasSub() ? 16 : 0);
            Text(m.text, m.bold ? g_fmtNameBold.Get() : g_fmtName.Get(), kMenuTextX, y, textR, y + hh);
            if (!m.shortcut.empty()) {
                Color(m.enabled ? cDim : cFaint);
                Text(m.shortcut, g_fmtRight.Get(), kMenuTextX, y, textR, y + hh);
            }
        }
        if (m.hasSub()) {
            Color(m.enabled ? cDim : cFaint);
            Glyph(gChevron, g_fmtIconSm.Get(), W - 24, y, W - 8, y + hh);
        }
        y += hh;
    }
    if (scrolls) {
        g_rt->PopAxisAlignedClip();
        const float trackH = sz.height - 8, thumbH = std::max(24.f, trackH * L.viewH / L.contentH);
        const float thumbY = 4 + (trackH - thumbH) * (L.scroll / MaxScroll(L));
        Round(D2D1::RectF(W - 5, thumbY, W - 2, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.2f);
    }
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        L.bmps.clear();
        L.icons.clear();
        L.brush.Reset();
        L.rt.Reset();
    }
}

void PopupCloseFrom(int level) {
    while (static_cast<int>(g_popup.levels.size()) > level) {
        PopupLevel& L = *g_popup.levels.back();
        L.bmps.clear();
        L.icons.clear();
        L.brush.Reset();
        L.rt.Reset();
        DestroyWindow(L.hwnd);
        g_popup.levels.pop_back();
    }
    if (!g_popup.levels.empty()) {
        g_popup.levels.back()->openSub = -1;
        InvalidateRect(g_popup.levels.back()->hwnd, nullptr, FALSE);
    }
}

void PopupClose() {
    if (!g_popup.open()) return;
    if (GetCapture() == g_popup.levels[0]->hwnd) ReleaseCapture();
    PopupCloseFrom(0);
    g_popup.onSelect = nullptr;
    g_popup.onMiddle = nullptr;
    g_popup.loadSub = nullptr;
    if (g_crumbMenuOpen) {
        g_crumbMenuOpen = false;
        Invalidate();
    }
}

void PopupShowLevel(std::vector<MenuItem> items, const RECT& anchor, bool isSub) {
    auto L = std::make_unique<PopupLevel>();
    L->items = std::move(items);
    float w, h;
    MenuSize(L->items, w, h);
    POINT ref{anchor.left, anchor.top};
    HMONITOR mon = MonitorFromPoint(ref, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    L->contentH = h;
    h = std::min(h, (wa.bottom - wa.top) * 96.f / g_dpi);  // never taller than the screen: the rest scrolls
    if (!isSub && g_popup.maxH > 0) h = std::min(h, g_popup.maxH);
    L->viewH = h;
    if (!isSub && g_popup.mark >= 0 && g_popup.mark < static_cast<int>(L->items.size())) {
        L->mark = g_popup.mark;  // it opens with the marked item in the middle
        L->scroll = std::clamp(ItemTop(*L, L->mark) - (h - kMenuRow) / 2, 0.f, MaxScroll(*L));
    }
    int pw = MulDiv(static_cast<int>(w), g_dpi, 96), ph = MulDiv(static_cast<int>(h), g_dpi, 96);
    int x, y;
    if (isSub) {
        x = anchor.right - MulDiv(4, g_dpi, 96);
        y = anchor.top - MulDiv(static_cast<int>(kMenuPad), g_dpi, 96);
        if (x + pw > wa.right && !g_test) x = anchor.left - pw + MulDiv(4, g_dpi, 96);
    } else {
        x = anchor.left;
        y = anchor.bottom;
        if (x + pw > wa.right && !g_test) x = std::max<LONG>(wa.left, anchor.left - pw);
        if (y + ph > wa.bottom && !g_test) y = std::max<LONG>(wa.top, anchor.top - ph);
    }
    if (!g_test) {  // in tests the window is off screen: the menu stays next to it
        if (y + ph > wa.bottom) y = std::max(static_cast<int>(wa.top), static_cast<int>(wa.bottom) - ph);
        if (x < wa.left) x = wa.left;
    }
    L->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP, x, y, pw,
                              ph, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    DWORD pref = 2;  // DWMWCP_ROUNDSMALL
    DwmSetWindowAttribute(L->hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
    L->rc = {x, y, x + pw, y + ph};
    if (!g_test) ShowWindow(L->hwnd, SW_SHOWNOACTIVATE);
    g_popup.levels.push_back(std::move(L));
    if (!isSub) SetCapture(g_popup.levels[0]->hwnd);
}

void PopupOpen(std::vector<MenuItem> items, const RECT& anchor, std::function<void(UINT)> onSelect,
               std::function<void(HMENU, int, const std::wstring&, std::vector<MenuItem>&)> loadSub = nullptr, float maxH = 0, int mark = -1) {
    PopupClose();
    if (items.empty()) return;
    g_popup.onSelect = std::move(onSelect);
    g_popup.loadSub = std::move(loadSub);
    g_popup.maxH = maxH;
    g_popup.mark = mark;
    g_popup.typed.clear();
    PopupShowLevel(std::move(items), anchor, false);
}

void PopupOpenSub(int level, int item) {
    PopupCloseFrom(level + 1);
    PopupLevel& L = *g_popup.levels[level];
    MenuItem& m = L.items[item];
    if (!m.enabled || !m.hasSub()) return;
    std::vector<MenuItem> items = m.sub;
    if (m.hsub && g_popup.loadSub) g_popup.loadSub(m.hsub, item, m.key, items);
    if (items.empty()) return;
    L.openSub = item;
    InvalidateRect(L.hwnd, nullptr, FALSE);
    int top = L.rc.top + MulDiv(static_cast<int>(ItemTop(L, item)), g_dpi, 96);
    RECT anchor{L.rc.left, top, L.rc.right, top};
    PopupShowLevel(std::move(items), anchor, true);
}

// Returns the level under the screen point (deepest first), or -1.
int PopupLevelAt(POINT pt) {
    for (int i = static_cast<int>(g_popup.levels.size()) - 1; i >= 0; --i)
        if (PtInRect(&g_popup.levels[i]->rc, pt)) return i;
    return -1;
}

void PopupSelect(int level, int item) {
    MenuItem& m = g_popup.levels[level]->items[item];
    if (!m.enabled || m.sep) return;
    if (m.hasSub()) {
        PopupOpenSub(level, item);
        return;
    }
    UINT id = m.id;
    auto cb = g_popup.onSelect;
    PopupClose();
    if (cb && id) cb(id);
}

void PopupKey(WPARAM vk) {
    if (!g_popup.open()) return;
    int li = static_cast<int>(g_popup.levels.size()) - 1;
    PopupLevel& L = *g_popup.levels[li];
    int n = static_cast<int>(L.items.size());
    const int cur = L.hover >= 0 ? L.hover : L.mark;
    auto show = [&](int i) {
        L.hover = i;
        EnsureItemVisible(L, i);
        InvalidateRect(L.hwnd, nullptr, FALSE);
    };
    auto step = [&](int dir, int from) {
        int i = from;
        for (int k = 0; k < n; ++k) {
            i = (i + dir + n) % n;
            if (!L.items[i].sep && L.items[i].enabled) break;
        }
        show(i);
    };
    auto page = [&](int dir) {  // a page further, stopping at the ends
        int i = std::clamp(cur + dir * std::max(1, static_cast<int>(L.viewH / kMenuRow) - 1), 0, n - 1);
        while (i >= 0 && i < n && (L.items[i].sep || !L.items[i].enabled)) i += dir;
        if (i >= 0 && i < n) show(i);
    };
    switch (vk) {
        case VK_ESCAPE: PopupClose(); break;
        case VK_DOWN: step(+1, cur); break;
        case VK_UP: step(-1, std::max(cur, 0)); break;
        case VK_HOME: step(+1, -1); break;
        case VK_END: step(-1, 0); break;
        case VK_NEXT: page(+1); break;
        case VK_PRIOR: page(-1); break;
        case VK_RIGHT:
            if (L.hover >= 0 && L.items[L.hover].hasSub()) {
                PopupOpenSub(li, L.hover);
                if (static_cast<int>(g_popup.levels.size()) > li + 1) {
                    g_popup.levels.back()->hover = -1;
                    PopupKey(VK_DOWN);
                }
            }
            break;
        case VK_LEFT:
            if (li > 0) PopupCloseFrom(li);
            break;
        case VK_SPACE:
            if (!g_popup.typed.empty() && GetTickCount() - g_popup.typedAt <= kPopupTypeMs) break;  // part of a name
            [[fallthrough]];
        case VK_RETURN:
            if (L.hover >= 0) PopupSelect(li, L.hover);
            break;
    }
}

// Typing goes to the item that starts with it; the same letter again, to the next one with it.
void PopupChar(wchar_t c) {
    if (!g_popup.open() || c < 32) return;
    const DWORD now = GetTickCount();
    if (now - g_popup.typedAt > kPopupTypeMs) g_popup.typed.clear();
    if (c == L' ' && g_popup.typed.empty()) return;
    g_popup.typedAt = now;
    g_popup.typed += c;
    const std::wstring& s = g_popup.typed;
    const bool again = s.find_first_not_of(s[0]) == std::wstring::npos;
    const size_t len = again ? 1 : s.size();
    PopupLevel& L = *g_popup.levels.back();
    const int n = static_cast<int>(L.items.size()), cur = L.hover >= 0 ? L.hover : L.mark;
    for (int k = again ? 1 : 0; k <= n; ++k) {  // a longer word may still be on the current item
        const int i = (std::max(cur, 0) + k) % n;
        const MenuItem& m = L.items[i];
        if (m.sep || !m.enabled || _wcsnicmp(m.text.c_str(), s.c_str(), len) != 0) continue;
        L.hover = i;
        EnsureItemVisible(L, i);
        InvalidateRect(L.hwnd, nullptr, FALSE);
        return;
    }
}

void PopupWheel(int delta, POINT pt) {
    if (!g_popup.open()) return;
    int li = PopupLevelAt(pt);
    if (li < 0) li = static_cast<int>(g_popup.levels.size()) - 1;
    PopupLevel& L = *g_popup.levels[li];
    const float to = std::clamp(L.scroll - delta * 3.f * kMenuRow / WHEEL_DELTA, 0.f, MaxScroll(L));
    if (to == L.scroll) return;
    PopupCloseFrom(li + 1);
    L.scroll = to;
    if (PtInRect(&L.rc, pt)) L.hover = ItemAt(L, (pt.y - L.rc.top) * 96.f / g_dpi);  // what is under the cursor now
    InvalidateRect(L.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto find = [&](HWND h) -> PopupLevel* {
        for (auto& L : g_popup.levels)
            if (L->hwnd == h) return L.get();
        return nullptr;
    };
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (PopupLevel* L = find(hwnd)) DrawLevel(*L);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            static POINT last{LONG_MIN, LONG_MIN};
            if (pt.x == last.x && pt.y == last.y) return 0;  // the cursor did not move: do not overwrite the keyboard hover
            last = pt;
            int li = PopupLevelAt(pt);
            for (int i = 0; i < static_cast<int>(g_popup.levels.size()); ++i) {
                PopupLevel& L = *g_popup.levels[i];
                int item = -1;
                if (i == li) item = ItemAt(L, (pt.y - L.rc.top) * 96.f / g_dpi);
                if (item != L.hover) {
                    L.hover = item;
                    InvalidateRect(L.hwnd, nullptr, FALSE);
                }
            }
            if (li >= 0) {
                PopupLevel& L = *g_popup.levels[li];
                // open the submenu of the item under the cursor (or close the previous one) after a short delay
                if (L.hover >= 0 && L.hover != L.openSub) {
                    g_popup.pendingLevel = li;
                    g_popup.pendingItem = L.hover;
                    SetTimer(hwnd, kTimerSub, 220, nullptr);
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerSub) {
                KillTimer(hwnd, kTimerSub);
                int li = g_popup.pendingLevel, it = g_popup.pendingItem;
                g_popup.pendingLevel = -1;
                if (li >= 0 && li < static_cast<int>(g_popup.levels.size())) {
                    PopupLevel& L = *g_popup.levels[li];
                    if (L.hover == it) {
                        if (it >= 0 && L.items[it].hasSub() && L.items[it].enabled)
                            PopupOpenSub(li, it);
                        else
                            PopupCloseFrom(li + 1);
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            if (PopupLevelAt(pt) < 0) PopupClose();  // click outside: it closes and the click does not pass through
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            int li = PopupLevelAt(pt);
            if (li >= 0) {
                PopupLevel& L = *g_popup.levels[li];
                int item = ItemAt(L, (pt.y - L.rc.top) * 96.f / g_dpi);
                if (item >= 0) PopupSelect(li, item);
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            const int li = PopupLevelAt(pt);
            if (li < 0 || !g_popup.onMiddle) return 0;
            PopupLevel& L = *g_popup.levels[li];
            const int item = ItemAt(L, (pt.y - L.rc.top) * 96.f / g_dpi);
            if (item < 0 || L.items[item].sep || !L.items[item].enabled || !L.items[item].id) return 0;
            const UINT id = L.items[item].id;
            auto cb = g_popup.onMiddle;
            PopupClose();
            cb(id);
            return 0;
        }
        case WM_MOUSEWHEEL:
            PopupWheel(GET_WHEEL_DELTA_WPARAM(wp), POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_CAPTURECHANGED:
            if (g_popup.open() && hwnd == g_popup.levels[0]->hwnd && reinterpret_cast<HWND>(lp) != hwnd) PopupClose();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterPopupClass(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = PopupProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);
}

// ================================================================ Shell context menu
constexpr UINT kCmdFirst = 1, kCmdLast = 0x6FFF, kCmdFav = 0x7000, kCmdCustom = 0x7100;  // kCmdCustom + index: the user's commands
constexpr UINT kCmdTemplate = 0x7200, kCmdTemplatesFolder = 0x72FF;                   // kCmdTemplate + index into Templates()
constexpr UINT kCmdTagNew = 0x7FFF, kCmdTag = 0x8000;                                   // kCmdTag + index into AllTags()
void TagPaletteOpen();  // palette.inl
void RunCustomCommand(int i);  // ops.inl

// The user's commands ([commands]) at the end of a Shell menu, so the editor can hide or move them like the rest.
std::vector<MenuItem> WithCustomCommands(std::vector<MenuItem> items) {
    for (size_t i = 0; i < g_cfg.custom.size(); ++i) {
        if (i == 0 && !items.empty()) {
            MenuItem s;
            s.sep = true;
            s.key = L"---";
            items.push_back(s);
        }
        MenuItem it;
        it.text = g_cfg.custom[i].name;
        it.key = L"jolt:" + Lower(g_cfg.custom[i].name);
        it.id = kCmdCustom + static_cast<UINT>(i);
        items.push_back(it);
    }
    return items;
}

struct ShellMenu {
    ComPtr<IShellFolder> psf;
    ComPtr<IContextMenu> cm;
    ComPtr<IContextMenu2> cm2;
    ComPtr<IContextMenu3> cm3;
    HMENU hmenu = nullptr;
    std::wstring folder;  // what the chosen command starts in
    std::vector<PIDLIST_RELATIVE> pidls;
    std::vector<HBITMAP> ownBitmaps;  // rows drawn by extensions (owner-draw)
    ~ShellMenu() {
        if (hmenu) DestroyMenu(hmenu);
        for (auto p : pidls) CoTaskMemFree(p);
        for (auto b : ownBitmaps) DeleteObject(b);
    }
    LRESULT MenuMsg(UINT msg, WPARAM wp, LPARAM lp) {
        LRESULT res = 0;
        if (cm3) cm3->HandleMenuMsg2(msg, wp, lp, &res);
        else if (cm2) cm2->HandleMenuMsg(msg, wp, lp);
        return res;
    }
};
std::unique_ptr<ShellMenu> g_shellMenu;

std::vector<MenuItem> ReadMenu(ShellMenu& m, HMENU h);
std::vector<MenuItem> ApplyMenuCfg(const std::wstring& ctx, std::vector<MenuItem> raw, bool includeHidden, std::vector<bool>* visibleOut);

// The "New" submenu: the templates first, then what Windows offers, then the templates folder. Its order and
// what is hidden come from [background/new] in menu.txt.
std::vector<MenuItem> NewSubmenuItems(ShellMenu& m, HMENU sub, bool includeHidden, std::vector<bool>* visibleOut = nullptr) {
    int pos = 0;
    for (int i = 0, n = GetMenuItemCount(m.hmenu); i < n; ++i)
        if (GetSubMenu(m.hmenu, i) == sub) pos = i;
    m.MenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(pos, FALSE));
    std::vector<MenuItem> items;
    const std::vector<TemplateDef> tpl = Templates();
    for (size_t i = 0; i < tpl.size(); ++i) {
        MenuItem it;
        it.text = tpl[i].name;
        it.key = L"jolt:template:" + Lower(tpl[i].name);
        it.id = kCmdTemplate + static_cast<UINT>(i);
        it.icon = tpl[i].isDir ? g_iconFolder : -1;
        it.iconPath = TemplatesDir() + L"\\" + tpl[i].name;
        items.push_back(std::move(it));
    }
    if (!items.empty()) {
        MenuItem s;
        s.sep = true;
        s.key = L"---";
        items.push_back(s);
    }
    for (MenuItem& it : ReadMenu(m, sub)) items.push_back(std::move(it));
    MenuItem s, f;
    s.sep = true;
    s.key = L"---";
    f.text = L"Templates folder…";
    f.key = L"jolt:templates";
    f.id = kCmdTemplatesFolder;
    items.push_back(s);
    items.push_back(f);
    return ApplyMenuCfg(L"background/new", std::move(items), includeHidden, visibleOut);
}

bool BindFolder(const std::wstring& path, ComPtr<IShellFolder>& psf) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (path.empty())
        SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pidl);
    else
        SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
    if (!pidl) return false;
    HRESULT hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&psf));
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr) && psf;
}

std::unique_ptr<ShellMenu> FinishShellMenu(std::unique_ptr<ShellMenu> m, bool extended);

// empty names = the folder background menu.
std::unique_ptr<ShellMenu> CreateShellMenu(const std::wstring& folder, const std::vector<std::wstring>& names,
                                           bool extended) {
    auto m = std::make_unique<ShellMenu>();
    if (!BindFolder(folder, m->psf)) return nullptr;
    m->folder = folder;
    if (names.empty()) {
        m->psf->CreateViewObject(g_hwnd, IID_PPV_ARGS(&m->cm));
    } else {
        for (const std::wstring& n : names) {
            PIDLIST_RELATIVE child = nullptr;
            std::wstring nn = n;
            if (SUCCEEDED(m->psf->ParseDisplayName(g_hwnd, nullptr, nn.data(), nullptr, &child, nullptr)) && child)
                m->pidls.push_back(child);
        }
        if (m->pidls.empty()) return nullptr;
        m->psf->GetUIObjectOf(g_hwnd, static_cast<UINT>(m->pidls.size()), (PCUITEMID_CHILD_ARRAY)m->pidls.data(),
                              IID_IContextMenu, nullptr, reinterpret_cast<void**>(m->cm.GetAddressOf()));
    }
    return FinishShellMenu(std::move(m), extended);
}

// Items from several folders (search results): the desktop takes full ID lists as its items, so a single menu
// holds files from anywhere. folder: where the chosen command starts.
std::unique_ptr<ShellMenu> CreateShellMenuPaths(const std::wstring& folder, const std::vector<std::wstring>& paths, bool extended) {
    auto m = std::make_unique<ShellMenu>();
    if (FAILED(SHGetDesktopFolder(&m->psf))) return nullptr;
    m->folder = folder;
    for (const std::wstring& p : paths) {
        PIDLIST_ABSOLUTE full = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &full, 0, nullptr)) && full)
            m->pidls.push_back(reinterpret_cast<PIDLIST_RELATIVE>(full));
    }
    if (m->pidls.empty()) return nullptr;
    m->psf->GetUIObjectOf(g_hwnd, static_cast<UINT>(m->pidls.size()), (PCUITEMID_CHILD_ARRAY)m->pidls.data(), IID_IContextMenu,
                          nullptr, reinterpret_cast<void**>(m->cm.GetAddressOf()));
    return FinishShellMenu(std::move(m), extended);
}

// The menu for the selection, wherever its items are.
std::unique_ptr<ShellMenu> SelectionShellMenu(const Tab& t, bool extended) {
    if (SelSpansFolders(t)) return CreateShellMenuPaths(SelDir(t), SelectedFullPaths(t), extended);
    return CreateShellMenu(SelDir(t), SelectedNames(t), extended);
}

std::unique_ptr<ShellMenu> FinishShellMenu(std::unique_ptr<ShellMenu> m, bool extended) {
    if (!m->cm) return nullptr;
    m->cm.As(&m->cm2);
    m->cm.As(&m->cm3);
    m->hmenu = CreatePopupMenu();
    UINT flags = CMF_NORMAL | CMF_EXPLORE | (extended ? CMF_EXTENDEDVERBS : 0);
    if (FAILED(m->cm->QueryContextMenu(m->hmenu, 0, kCmdFirst, kCmdLast, flags))) return nullptr;
    return m;
}

void ShellInvoke(ShellMenu& m, UINT id, POINT pt) {
    if (TestRefuses(m.folder)) return;  // Windows' own commands can change anything
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof(ici);
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
    if (KeyDown(VK_SHIFT)) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    if (KeyDown(VK_CONTROL)) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    ici.hwnd = g_hwnd;
    ici.lpVerb = MAKEINTRESOURCEA(id - kCmdFirst);
    ici.lpVerbW = MAKEINTRESOURCEW(id - kCmdFirst);
    ici.lpDirectoryW = m.folder.c_str();
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = pt;
    m.cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
}

std::wstring StripAmp(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&' && i + 1 < s.size()) {
            if (s[i + 1] == L'&') out += L'&';
            else out += s[i + 1];
            ++i;
        } else if (s[i] != L'&') {
            out += s[i];
        }
    }
    return out;
}

// Extensions that draw their items by hand (e.g. "Send to"): they are asked to draw into a bitmap of ours.
HBITMAP RenderOwnerDrawn(ShellMenu& m, HMENU hmenu, UINT id, ULONG_PTR data) {
    if (!m.cm2 && !m.cm3) return nullptr;
    MEASUREITEMSTRUCT mis{};
    mis.CtlType = ODT_MENU;
    mis.itemID = id;
    mis.itemData = data;
    m.MenuMsg(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&mis));
    int w = std::clamp(static_cast<int>(mis.itemWidth) + MulDiv(24, g_dpi, 96), 40, 800);
    int h = std::clamp(static_cast<int>(mis.itemHeight), 16, 64);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, hbm);
    RECT rc{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(RGB(0x20, 0x20, 0x24));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xE9, 0xE9, 0xEE));
    SelectObject(dc, g_editFont ? g_editFont : GetStockObject(DEFAULT_GUI_FONT));
    DRAWITEMSTRUCT dis{};
    dis.CtlType = ODT_MENU;
    dis.itemID = id;
    dis.itemAction = ODA_DRAWENTIRE;
    dis.hwndItem = reinterpret_cast<HWND>(hmenu);
    dis.hDC = dc;
    dis.rcItem = rc;
    dis.itemData = data;
    m.MenuMsg(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&dis));
    SelectObject(dc, old);
    DeleteDC(dc);
    m.ownBitmaps.push_back(hbm);
    return hbm;
}

std::vector<MenuItem> ReadMenu(ShellMenu& m, HMENU h) {
    std::vector<MenuItem> out;
    int n = GetMenuItemCount(h);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP | MIIM_DATA;
        if (!GetMenuItemInfoW(h, i, TRUE, &mii)) continue;
        std::wstring text;
        if (mii.cch) {
            text.resize(mii.cch);
            mii.dwTypeData = text.data();
            mii.cch++;
            GetMenuItemInfoW(h, i, TRUE, &mii);
            text.resize(wcslen(text.c_str()));
        }
        MenuItem it;
        it.sep = (mii.fType & MFT_SEPARATOR) != 0;
        it.enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        it.checked = (mii.fState & MFS_CHECKED) != 0;
        it.bold = (mii.fState & MFS_DEFAULT) != 0;
        it.hsub = mii.hSubMenu;
        it.id = it.hsub ? 0 : mii.wID;
        it.hbm = mii.hbmpItem;
        size_t tab = text.find(L'\t');
        if (tab != std::wstring::npos) {
            it.shortcut = text.substr(tab + 1);
            text = text.substr(0, tab);
        }
        it.text = StripAmp(text);
        if ((mii.fType & MFT_OWNERDRAW) && !it.sep) {
            it.ownerDrawn = true;
            it.hbm = RenderOwnerDrawn(m, h, mii.wID, mii.dwItemData);
            if (!it.hbm) it.text = L"(unnamed item)";
        }
        if (!it.sep && !it.hsub && mii.wID >= kCmdFirst && mii.wID <= kCmdLast) {
            wchar_t verb[128] = {};
            if (m.cm->GetCommandString(mii.wID - kCmdFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 128) ==
                    S_OK &&
                verb[0])
                it.key = Lower(verb);
        }
        if (it.key.empty()) it.key = it.sep ? L"---" : Lower(it.text);
        if (it.key == L"rename") continue;  // we do not rename in the list yet; ours goes here later
        out.push_back(std::move(it));
    }
    // no separators at the start, at the end, or repeated
    std::vector<MenuItem> clean;
    for (auto& it : out) {
        if (it.sep && (clean.empty() || clean.back().sep)) continue;
        clean.push_back(std::move(it));
    }
    while (!clean.empty() && clean.back().sep) clean.pop_back();
    return clean;
}

// ================================================================ menu settings
// %APPDATA%\Jolt\menu.txt: sections [background] [file] [folder]; one option per line in the
// desired order; "-" in front = hidden; "---" = separator. Anything not listed goes at the end.
struct MenuCfgItem {
    std::wstring key;
    bool visible;
};
std::unordered_map<std::wstring, std::vector<MenuCfgItem>> g_menuCfg;
bool g_menuCfgLoaded = false;

std::wstring MenuCfgFile() {
    std::wstring f = FavFile();
    if (f.empty()) return L"";
    return f.substr(0, f.find_last_of(L'\\') + 1) + L"menu.txt";
}

void LoadMenuCfg() {
    g_menuCfgLoaded = true;
    g_menuCfg.clear();
    std::ifstream in(MenuCfgFile(), std::ios::binary);
    if (!in) return;
    std::string line;
    std::wstring ctx;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::wstring w = Wide(line);
        if (w.front() == L'[' && w.back() == L']') {
            ctx = w.substr(1, w.size() - 2);
            g_menuCfg[ctx];
            continue;
        }
        if (ctx.empty()) continue;
        bool vis = true;
        if (w[0] == L'-' && w != L"---") {
            vis = false;
            w = w.substr(1);
        }
        g_menuCfg[ctx].push_back({w, vis});
    }
}

void SaveMenuCfg() {
    std::ofstream out(MenuCfgFile(), std::ios::binary);
    out << "# Jolt context menu. Sections: [background] [file] [folder] [background/new] (the New submenu).\n"
           "# One option per line, in the order they appear. \"-\" in front = hidden. \"---\" = separator.\n"
           "# Anything not listed here is added at the end, in the order Windows gives it.\n";
    for (const wchar_t* ctx : {L"background", L"file", L"folder", L"background/new"}) {
        auto it = g_menuCfg.find(ctx);
        if (it == g_menuCfg.end()) continue;
        out << "\n[" << Utf8(ctx) << "]\n";
        for (const MenuCfgItem& c : it->second) out << (c.visible ? "" : "-") << Utf8(c.key) << '\n';
    }
}

// Sorts/filters according to the settings. With includeHidden=true returns everything (for the editor),
// marking hidden items as disabled.
std::vector<MenuItem> ApplyMenuCfg(const std::wstring& ctx, std::vector<MenuItem> raw, bool includeHidden,
                                   std::vector<bool>* visibleOut = nullptr) {
    if (!g_menuCfgLoaded) LoadMenuCfg();
    auto it = g_menuCfg.find(ctx);
    std::vector<MenuItem> out;
    std::vector<bool> vis;
    if (it == g_menuCfg.end()) {
        out = std::move(raw);
        vis.assign(out.size(), true);
    } else {
        std::vector<bool> used(raw.size(), false);
        for (const MenuCfgItem& c : it->second) {
            if (c.key == L"---") {
                MenuItem s;
                s.sep = true;
                s.key = L"---";
                out.push_back(s);
                vis.push_back(c.visible);
                continue;
            }
            for (size_t i = 0; i < raw.size(); ++i) {
                if (!used[i] && !raw[i].sep && raw[i].key == c.key) {
                    used[i] = true;
                    out.push_back(raw[i]);
                    vis.push_back(c.visible);
                    break;
                }
            }
        }
        bool first = true;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (used[i] || raw[i].sep) continue;
            if (first && !out.empty()) {
                MenuItem s;
                s.sep = true;
                s.key = L"---";
                out.push_back(s);
                vis.push_back(true);
            }
            first = false;
            out.push_back(raw[i]);
            vis.push_back(true);
        }
    }
    if (!includeHidden) {
        std::vector<MenuItem> shown;
        for (size_t i = 0; i < out.size(); ++i) {
            if (!vis[i]) continue;
            if (out[i].sep && (shown.empty() || shown.back().sep)) continue;
            shown.push_back(out[i]);
        }
        while (!shown.empty() && shown.back().sep) shown.pop_back();
        return shown;
    }
    if (visibleOut) *visibleOut = vis;
    return out;
}

// ================================================================ opening menus from the app
// Runs a Shell verb ("copy", "cut", "paste", "delete"…) on names in the current
// folder; with empty names it acts on the folder itself (e.g. "paste").
bool ShellVerb(const std::vector<std::wstring>& names, const char* verb) {
    Tab& t = T();
    if (!t.list || t.list->error) return false;
    if (t.path.empty() && names.empty()) return false;
    if (names.empty() && strcmp(verb, "paste") != 0) return false;  // nothing selected: nothing to do
    if (TestRefuses(names.empty() ? t.path : SelDir(t))) return false;
    auto m = names.empty() ? CreateShellMenu(t.path, names, false) : SelectionShellMenu(t, false);
    if (!m) return false;
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof(ici);
    ici.fMask = CMIC_MASK_ASYNCOK;
    if (KeyDown(VK_SHIFT)) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = g_hwnd;
    ici.lpVerb = verb;
    ici.nShow = SW_SHOWNORMAL;
    HRESULT hr = m->cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
    g_shellMenu = std::move(m);  // some handlers keep working after returning
    return SUCCEEDED(hr);
}

// ---- clipboard compatible with File Explorer: CF_HDROP + "Preferred DropEffect"
bool ClipboardSetFiles(const std::vector<std::wstring>& fullPaths, bool cut) {
    if (fullPaths.empty()) return false;
    size_t chars = 1;
    for (const auto& p : fullPaths) chars += p.size() + 1;
    HGLOBAL hg = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    HGLOBAL he = GlobalAlloc(GHND, sizeof(DWORD));
    if (!hg || !he) {
        if (hg) GlobalFree(hg);
        if (he) GlobalFree(he);
        return false;
    }
    auto* df = static_cast<DROPFILES*>(GlobalLock(hg));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* w = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
    for (const auto& p : fullPaths) {
        memcpy(w, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
        w += p.size() + 1;
    }
    *w = 0;
    GlobalUnlock(hg);
    *static_cast<DWORD*>(GlobalLock(he)) = cut ? DROPEFFECT_MOVE : (DROPEFFECT_COPY | DROPEFFECT_LINK);
    GlobalUnlock(he);
    if (!OpenClipboard(g_hwnd)) {
        GlobalFree(hg);
        GlobalFree(he);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hg);
    SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), he);
    CloseClipboard();
    return true;
}

// Pastes the clipboard contents into destFolder, with IFileOperation (progress, conflicts and undo
// as in File Explorer). If everything comes from the same folder, copies with another name are created.
bool ClipboardPaste(const std::wstring& destFolder) {
    if (destFolder.empty() || !IsClipboardFormatAvailable(CF_HDROP)) return false;
    std::vector<std::wstring> files;
    bool move = false;
    if (!OpenClipboard(g_hwnd)) return false;
    if (HDROP hd = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t buf[4096];
            if (DragQueryFileW(hd, i, buf, 4096)) files.push_back(buf);
        }
    }
    if (HGLOBAL he = GetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT))) {
        if (DWORD* e = static_cast<DWORD*>(GlobalLock(he))) {
            move = (*e & DROPEFFECT_MOVE) && !(*e & DROPEFFECT_COPY);
            GlobalUnlock(he);
        }
    }
    CloseClipboard();
    if (files.empty()) return false;
    bool sameFolder = true;
    for (const auto& f : files)
        if (Key(ParentOf(f)) != Key(destFolder)) sameFolder = false;
    if (move && sameFolder) return false;  // moving to the same folder: nothing to do
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return false;
    op->SetOwnerWindow(g_hwnd);
    DWORD flags = FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT;
    if (sameFolder) flags |= FOF_RENAMEONCOLLISION;
    op->SetOperationFlags(flags);
    ComPtr<IShellItem> dest;
    if (FAILED(SHCreateItemFromParsingName(destFolder.c_str(), nullptr, IID_PPV_ARGS(&dest)))) return false;
    int added = 0;
    OpRecorder* rec = new OpRecorder();
    for (const auto& f : files) {
        ComPtr<IShellItem> src;
        if (FAILED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(&src)))) continue;
        rec->AddSource(f, destFolder);
        if (move)
            op->MoveItem(src.Get(), dest.Get(), nullptr, nullptr);
        else
            op->CopyItem(src.Get(), dest.Get(), nullptr, nullptr);
        ++added;
    }
    if (!added) {
        rec->Release();
        return false;
    }
    rec->Attach(op.Get());
    HRESULT hr = op->PerformOperations();
    rec->Detach(op.Get());
    rec->Commit();
    rec->Release();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (SUCCEEDED(hr) && move && !aborted && OpenClipboard(g_hwnd)) {  // what was cut has already moved
        EmptyClipboard();
        CloseClipboard();
    }
    return SUCCEEDED(hr);
}

void DeleteSelection();
void Properties();

std::wstring ShortcutFor(Cmd cmd);  // palette.inl
void BinRestoreSelection();  // ops.inl
void BinDeleteSelection();
void EmptyRecycleBin();

// In the Recycle Bin the menu is Jolt's: Windows' would act on the files with their made-up names.
void OpenBinMenu(POINT screenPt, int row) {
    std::vector<MenuItem> items;
    auto add = [&](const wchar_t* text, UINT id, Cmd cmd) {
        MenuItem m;
        m.text = text;
        m.shortcut = ShortcutFor(cmd);
        m.id = id;
        items.push_back(std::move(m));
    };
    if (row >= 0) {
        add(L"Restore", 1, Cmd::Restore);
        add(L"Delete for good", 2, Cmd::Delete);
        MenuItem sep;
        sep.sep = true;
        items.push_back(sep);
    }
    add(L"Empty the Recycle Bin", 3, Cmd::EmptyRecycleBin);
    const RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(std::move(items), anchor, [](UINT id) {
        if (id == 1) BinRestoreSelection();
        if (id == 2) BinDeleteSelection();
        if (id == 3) EmptyRecycleBin();
        Invalidate();
    });
}

void OpenShellMenu(POINT screenPt, int row) {
    Tab& t = T();
    if (!t.list || t.list->error) return;
    if (IsBin(t.path)) {
        OpenBinMenu(screenPt, row);
        return;
    }
    if (IsVirtual(t.path)) return;
    std::vector<std::wstring> names;
    std::wstring ctx = L"background", folder = t.path;
    if (row >= 0 && row < Count()) {
        folder = SelDir(t);
        names = SelectedNames(t);  // the right click already put the row in the selection
        bool allDirs = true;
        for (int i = 0; i < RowCount(t); ++i)
            if (IsSel(t, i) && !EntryAt(t, i).isDir()) allDirs = false;
        ctx = allDirs ? L"folder" : L"file";
    }
    g_shellMenu = names.empty() ? CreateShellMenu(folder, names, KeyDown(VK_SHIFT)) : SelectionShellMenu(t, KeyDown(VK_SHIFT));
    if (!g_shellMenu) return;
    std::vector<MenuItem> items = ApplyMenuCfg(ctx, WithCustomCommands(ReadMenu(*g_shellMenu, g_shellMenu->hmenu)), false);
    if (row >= 0) {  // Tags: the known ones (checked when the whole selection has one) and a new one
        MenuItem tg, sep;
        tg.text = L"Tags";
        tg.key = L"jolt:tags";
        const std::vector<std::wstring> all = AllTags(), sel = SelectedFullPaths(t);
        for (size_t i = 0; i < all.size() && i < 2000; ++i) {
            MenuItem it;
            it.text = all[i];
            it.id = kCmdTag + static_cast<UINT>(i);
            it.checked = !sel.empty();
            for (const std::wstring& p : sel) it.checked = it.checked && HasTag(p, all[i]);
            tg.sub.push_back(it);
        }
        if (!all.empty()) {
            MenuItem s;
            s.sep = true;
            tg.sub.push_back(s);
        }
        MenuItem nw;
        nw.text = L"New tag…";
        nw.id = kCmdTagNew;
        tg.sub.push_back(nw);
        sep.sep = true;
        items.insert(items.begin(), {tg, sep});
    }
    std::wstring favPath;  // on a single folder, it can be added to favorites without entering it
    if (ctx == L"folder" && SelCount(t) == 1) {
        favPath = RowPath(t, t.sel);
        MenuItem fav, sep;
        fav.text = L"Add to favorites";
        fav.id = kCmdFav;
        sep.sep = true;
        items.insert(items.begin(), {fav, sep});
    }
    RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(
        std::move(items), anchor,
        [screenPt, favPath](UINT id) {
            if (id == kCmdFav) {
                FavAdd(favPath);
                return;
            }
            if (id == kCmdTagNew) {
                TagPaletteOpen();
                return;
            }
            if (id >= kCmdTag) {
                const std::vector<std::wstring> all = AllTags();
                if (id - kCmdTag < all.size()) TagToggle(SelectedFullPaths(T()), all[id - kCmdTag]);
                return;
            }
            if (id == kCmdTemplatesFolder) {
                OpenTemplatesFolder();
                return;
            }
            if (id >= kCmdTemplate) {
                const std::vector<TemplateDef> tpl = Templates();
                if (id - kCmdTemplate < tpl.size()) NewFromTemplate(tpl[id - kCmdTemplate].name);
                return;
            }
            if (id >= kCmdCustom) {
                RunCustomCommand(static_cast<int>(id - kCmdCustom));
                return;
            }
            if (!g_shellMenu) return;
            // Delete goes through Jolt so it stays in the Ctrl+Z history; Properties too when the items are in
            // several folders, since that verb can't open them together
            char verb[32] = "";
            if (id >= kCmdFirst && !SelectedNames(T()).empty() &&
                SUCCEEDED(g_shellMenu->cm->GetCommandString(id - kCmdFirst, GCS_VERBA, nullptr, verb, sizeof(verb)))) {
                if (!_stricmp(verb, "delete")) {
                    DeleteSelection();
                    return;
                }
                if (!_stricmp(verb, "properties") && SelSpansFolders(T())) {
                    Properties();
                    return;
                }
            }
            ShellInvoke(*g_shellMenu, id, screenPt);
            if (T().recursive) RecCheckSoon();  // it may have changed what the results show
        },
        [](HMENU h, int pos, const std::wstring& key, std::vector<MenuItem>& out) {
            if (!g_shellMenu) return;
            if (key == L"new") {  // Jolt's templates go in, and the user's order and hiding apply
                out = NewSubmenuItems(*g_shellMenu, h, false);
                return;
            }
            g_shellMenu->MenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(h), MAKELPARAM(pos, FALSE));
            out = ReadMenu(*g_shellMenu, h);
        });
}

void OpenMenuEditor();

enum AppCmd : UINT { CmdMenuEditor = 1, CmdConfigFolder, CmdInstall, CmdAbout, CmdQuit, CmdSettings };
void SettingsOpen(int section = -1);  // settings.inl
void InstPanelOpen();
void BgQuit();

void OpenAppMenu(const RECT& anchorScreen) {
    std::vector<MenuItem> items;
    auto add = [&](const wchar_t* text, const wchar_t* shortcut, UINT id) {
        MenuItem m;
        m.text = text;
        m.shortcut = shortcut;
        m.id = id;
        items.push_back(m);
    };
    auto sep = [&] {
        MenuItem m;
        m.sep = true;
        items.push_back(m);
    };
    add(L"Settings…", ShortcutFor(Cmd::Settings).c_str(), CmdSettings);
    add(L"Context menu…", L"", CmdMenuEditor);
    add(L"Config folder", L"", CmdConfigFolder);
    if (!g_adminWindow) add(L"Install…", L"", CmdInstall);
    sep();
    add(L"About Jolt", L"", CmdAbout);
    sep();
    add(L"Quit", L"", CmdQuit);
    PopupOpen(std::move(items), anchorScreen, [](UINT id) {
        switch (id) {
            case CmdSettings: SettingsOpen(); break;
            case CmdMenuEditor: OpenMenuEditor(); break;
            case CmdConfigFolder: {
                std::wstring f = FavFile();
                if (!f.empty()) NewTab(f.substr(0, f.find_last_of(L'\\')));
                break;
            }
            case CmdAbout:
                MessageBoxW(g_hwnd,
                            L"Jolt " JOLT_VERSION_W L" — file explorer.\n\nC++ · Win32 · Direct2D.\n"
                            L"Built so that opening folders feels instant.",
                            L"About Jolt", MB_OK | MB_ICONINFORMATION);
                break;
            case CmdInstall: InstPanelOpen(); break;
            case CmdQuit: BgQuit(); break;
        }
    });
}

// ================================================================ tab menu
std::wstring ShortcutFor(Cmd cmd);  // palette.inl

void OpenTabMenu(int i, POINT screenPt) {
    if (i < 0 || i >= static_cast<int>(g_tabs.size())) return;
    Tab* const tab = g_tabs[i].get();
    auto add = [](std::vector<MenuItem>& items, const wchar_t* text, UINT id, Cmd cmd, bool enabled = true) {
        MenuItem m;
        m.text = text;
        m.shortcut = ShortcutFor(cmd);
        m.id = id;
        m.enabled = enabled;
        items.push_back(std::move(m));
    };
    std::vector<MenuItem> items;
    add(items, tab->pinned ? L"Unpin tab" : L"Pin tab", 1, Cmd::PinTab);
    add(items, L"Close tab", 2, Cmd::CloseTab, !tab->pinned);
    add(items, L"Close other tabs", 3, Cmd::CloseOthers);
    const RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(std::move(items), anchor, [tab](UINT id) {
        int at = -1;
        for (int k = 0; k < static_cast<int>(g_tabs.size()); ++k)
            if (g_tabs[k].get() == tab) at = k;
        if (at < 0) return;
        if (id == 1) SetPinned(at, !tab->pinned);
        if (id == 2) CloseTab(at);
        if (id == 3) {
            SelectTab(at);
            CloseOtherTabs();
        }
    });
}

// ================================================================ breadcrumb dropdown
// The folders inside a folder, from the chevron after its segment; a right click on a segment lists that segment's
// siblings, which is the same list one level up. It opens on the folder you are in, or on the way to it.
struct CrumbWait {
    std::wstring dir;
    RECT anchor{};
    HWND popup = nullptr;  // the "Loading…" menu it will replace, if it's still open
} g_crumbWait;

// Where a segment's siblings are. The top of a tree has none: there, its own contents.
std::wstring CrumbSiblingsDir(const std::wstring& p) { return p.empty() ? p : ParentOf(p); }

// Is this item the folder we are in, or on the way to it?
bool OnTheWay(const std::wstring& item, const std::wstring& here) {
    std::wstring k = Key(item);
    if (k == here) return true;
    if (k.back() != L'\\') k += L'\\';
    return here.rfind(k, 0) == 0;
}

void CrumbMenuShow(const Listing& l, const std::wstring& dir, const RECT& anchor) {
    const std::wstring here = Key(T().path);
    const bool drives = dir.empty();
    struct Sub {
        const wchar_t* name;
        std::wstring path;
    };
    std::vector<Sub> subs;
    for (const Entry& e : l.entries) {
        if (!e.isDir() && !drives) continue;
        std::wstring path = Join(dir, l.name(e));
        const bool hidden = !drives && (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        if (hidden && !g_cfg.showHidden && !OnTheWay(path, here)) continue;
        subs.push_back({l.name(e), std::move(path)});
    }
    if (!drives) std::sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) { return StrCmpLogicalW(a.name, b.name) < 0; });
    auto paths = std::make_shared<std::vector<std::wstring>>();
    paths->reserve(subs.size());
    std::vector<MenuItem> items;
    items.reserve(subs.size());
    int mark = -1;
    for (Sub& s : subs) {
        MenuItem m;
        m.text = drives ? DriveLabel(s.name[0]) : s.name;
        m.icon = drives ? -1 : g_iconFolder;
        m.iconPath = s.path;
        if (mark < 0 && OnTheWay(s.path, here)) {
            mark = static_cast<int>(items.size());
            m.bold = true;
        }
        paths->push_back(std::move(s.path));
        m.id = static_cast<UINT>(paths->size());
        items.push_back(std::move(m));
    }
    if (items.empty()) {
        MenuItem m;
        m.text = l.error ? FolderWhyLong({l.error, L""}) : L"No folders here";
        m.enabled = false;
        items.push_back(std::move(m));
    }
    PopupOpen(std::move(items), anchor, [paths](UINT id) { Navigate(T(), (*paths)[id - 1], true); }, nullptr,
              20 * kMenuRow + 2 * kMenuPad, mark);
    g_popup.onMiddle = [paths](UINT id) { NewTab((*paths)[id - 1]); };
    g_crumbMenuOpen = true;
    g_crumbMenuDir = dir;
    Invalidate();
}

// r: the segment or chevron it hangs from, in the main window.
void CrumbMenu(const std::wstring& dir, const D2D1_RECT_F& r) {
    RECT anchor = ScreenRect(r);
    anchor.bottom += MulDiv(4, g_dpi, 96);
    g_crumbWait.popup = nullptr;
    auto it = g_cache.find(Key(dir));
    if (it != g_cache.end()) {
        CrumbMenuShow(*it->second, dir, anchor);
        Enqueue(dir, JobKind::Revalidate);
        return;
    }
    auto j = Enqueue(dir, JobKind::Load);
    if (WaitForSingleObject(j->done, 30) == WAIT_OBJECT_0 && j->result && !j->taken.exchange(true)) {
        RememberListing(Key(dir), j->result);
        CrumbMenuShow(*j->result, dir, anchor);
        return;
    }
    // the network or a sleeping disk: the menu opens now and fills in when the list arrives
    MenuItem m;
    m.text = L"Loading…";
    m.enabled = false;
    PopupOpen({m}, anchor, nullptr);
    g_crumbMenuOpen = true;
    g_crumbMenuDir = dir;
    g_crumbWait = {dir, anchor, g_popup.levels[0]->hwnd};
    Invalidate();
}

// A listing arrived: is it the one the open menu is waiting for?
void CrumbMenuListed(const Listing& l) {
    if (!g_crumbWait.popup || Key(l.path) != Key(g_crumbWait.dir)) return;
    const CrumbWait w = g_crumbWait;
    g_crumbWait.popup = nullptr;
    if (g_popup.open() && g_popup.levels[0]->hwnd == w.popup) CrumbMenuShow(l, w.dir, w.anchor);
}

// ================================================================ context menu editor
// Shows the real Windows menu for three contexts and lets you hide (checkbox) and reorder (drag). It is a panel
// drawn over the main window, like the rename panel: no window of its own, so it is there at once.
constexpr float kEdW = 640.f, kEdMaxH = 620.f, kEdTop = 54.f, kEdFoot = 44.f, kEdRow = 28.f;
constexpr UINT WM_APP_MENUED = WM_APP + 14;  // reads a context's menu once the panel has been drawn
constexpr int kEdCtxCount = 4;
const wchar_t* kEdTabNames[kEdCtxCount] = {L"Folder background", L"File", L"Folder", L"New submenu"};

struct EditorCtx {
    std::wstring ctx;        // background / file / folder
    std::wstring sample;     // description of the sample used
    std::unique_ptr<ShellMenu> sm;
    std::vector<MenuItem> items;  // current order (includes hidden items)
    std::vector<bool> visible;
    bool loaded = false;
};

struct Editor {
    bool open = false;
    D2D1_RECT_F box{};  // the panel, in window DIPs
    std::unordered_map<HBITMAP, ComPtr<ID2D1Bitmap>> bmps;  // made with the main window's target
    EditorCtx ctxs[kEdCtxCount];
    int cur = 0;
    int hover = -1, sel = -1;
    int drag = -1;       // item being dragged
    float dragY = 0;     // cursor position while dragging
    bool dragging = false;
    float scroll = 0;
    int hoverTab = -1;
    bool hoverReset = false, hoverClose = false;
} g_ed;

// The main target is being recreated: the item icons made with it go too.
void MenuEditorReleaseBitmaps() { g_ed.bmps.clear(); }

void EditorSave(EditorCtx& c) {
    std::vector<MenuCfgItem> cfg;
    for (size_t i = 0; i < c.items.size(); ++i) cfg.push_back({c.items[i].key, c.visible[i]});
    g_menuCfg[c.ctx] = std::move(cfg);
    SaveMenuCfg();
}

void EditorLoadCtx(EditorCtx& c) {
    c.loaded = true;
    c.items.clear();
    c.visible.clear();
    c.sm.reset();
    Tab& t = T();
    std::wstring folder = t.path;
    std::vector<std::wstring> names;
    if (c.ctx == L"background" || c.ctx == L"background/new") {
        c.sample = t.path.empty() ? L"This PC" : t.path;
    } else {
        bool wantDir = c.ctx == L"folder";
        int pick = -1;
        if (t.list && !t.list->error) {
            if (t.sel >= 0 && t.sel < Count() && EntryAt(t, t.sel).isDir() == wantDir) pick = t.sel;
            for (int i = 0; pick < 0 && i < Count(); ++i)
                if (EntryAt(t, i).isDir() == wantDir) pick = i;
        }
        if (pick >= 0) {
            std::wstring name = t.list->name(EntryAt(t, pick));
            if (t.path.empty()) name += L"\\";
            names.push_back(name);
            c.sample = Join(t.path, t.list->name(EntryAt(t, pick)));
        } else if (wantDir && !t.path.empty()) {
            folder = ParentOf(t.path);
            names.push_back(LeafOf(t.path));
            if (IsDriveRoot(t.path)) names.back() = t.path;
            c.sample = t.path;
        } else {
            c.sample = L"";
            return;
        }
    }
    c.sm = CreateShellMenu(folder, names, false);
    if (!c.sm) {
        c.sample = L"";
        return;
    }
    if (c.ctx == L"background/new") {
        for (const MenuItem& m : ReadMenu(*c.sm, c.sm->hmenu))
            if (m.key == L"new" && m.hsub) {
                c.items = NewSubmenuItems(*c.sm, m.hsub, true, &c.visible);
                return;
            }
        c.sample = L"";  // Windows offers no "New" here (a drive list, a virtual folder)
        return;
    }
    c.items = ApplyMenuCfg(c.ctx, WithCustomCommands(ReadMenu(*c.sm, c.sm->hmenu)), true, &c.visible);
}

// Before the first frame the panel has no size yet: then nothing needs scrolling.
float EditorListH() { return g_ed.box.bottom <= g_ed.box.top ? 1e6f : std::max(0.f, g_ed.box.bottom - g_ed.box.top - kEdTop - kEdFoot); }

void EditorClampScroll() {
    EditorCtx& c = g_ed.ctxs[g_ed.cur];
    float maxS = std::max(0.f, c.items.size() * kEdRow - EditorListH());
    g_ed.scroll = std::clamp(g_ed.scroll, 0.f, maxS);
}

int EditorRowAt(float y) {
    EditorCtx& c = g_ed.ctxs[g_ed.cur];
    const float top = g_ed.box.top + kEdTop;
    if (y < top || y >= top + EditorListH()) return -1;
    int r = static_cast<int>((y - top + g_ed.scroll) / kEdRow);
    return r >= 0 && r < static_cast<int>(c.items.size()) ? r : -1;
}

int EditorTabAt(float x, float y) {
    if (y < g_ed.box.top + 12 || y >= g_ed.box.top + 40) return -1;
    float tx = g_ed.box.left + kPad;
    for (int i = 0; i < kEdCtxCount; ++i) {
        float w = MeasureW(kEdTabNames[i], g_fmtName.Get()) + 26;
        if (x >= tx && x < tx + w) return i;
        tx += w + 6;
    }
    return -1;
}

D2D1_RECT_F EditorResetRect() {
    const D2D1_RECT_F& b = g_ed.box;
    return D2D1::RectF(b.right - 118, b.bottom - kEdFoot + 9, b.right - kPad, b.bottom - 9);
}
D2D1_RECT_F EditorCloseRect() {
    const D2D1_RECT_F& b = g_ed.box;
    return D2D1::RectF(b.right - 42, b.top + 12, b.right - 12, b.top + 40);
}
bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

void DrawMenuEditor(float W, float H) {
    if (!g_ed.open) return;
    const float pw = std::min(kEdW, W - 40), ph = std::min(kEdMaxH, H - kTabH - 24);
    const float x0 = std::floor((W - pw) / 2), y0 = std::floor(std::max(kTabH + 8, (H - ph) / 2));
    const float R = x0 + pw, B = y0 + ph;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_ed.box = D2D1::RectF(x0, y0, R, B);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x0 - i, y0 - i + 2, R + i, B + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_ed.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_ed.box, 12, 12), g_brush.Get(), 1.f);
    EditorCtx& c = g_ed.ctxs[g_ed.cur];

    // context selector, and the close button
    float x = x0 + kPad;
    for (int i = 0; i < kEdCtxCount; ++i) {
        float w = MeasureW(kEdTabNames[i], g_fmtName.Get()) + 26;
        D2D1_RECT_F r = D2D1::RectF(x, y0 + 12, x + w, y0 + 40);
        if (i == g_ed.cur)
            Round(r, 6, cAccent, 0.25f);
        else if (i == g_ed.hoverTab)
            Round(r, 6, 0xFFFFFF, 0.06f);
        Color(i == g_ed.cur ? cText : cDim);
        Text(kEdTabNames[i], wcslen(kEdTabNames[i]), g_fmtCrumb.Get(), r.left, r.top, r.right, r.bottom);
        x += w + 6;
    }
    const D2D1_RECT_F cb = EditorCloseRect();
    if (g_ed.hoverClose) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(g_ed.hoverClose ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);  // ×
    HLine(x0, R, y0 + kEdTop - 1);

    // list
    const float listTop = y0 + kEdTop, listBot = B - kEdFoot;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, listTop, R, listBot), D2D1_ANTIALIAS_MODE_ALIASED);
    if (!c.loaded) {
        Color(cFaint);
        Text(L"Reading the menu…", g_fmtCenter.Get(), x0 + kPad, listTop, R - kPad, listTop + 80);
    } else if (c.sample.empty()) {
        Color(cDim);
        Text(c.ctx == L"background/new" ? L"Windows offers no \"New\" submenu in this folder.\nOpen a normal folder and come back."
                                       : L"There is no sample item of this type in the current folder.\nOpen a folder that has one and come back.",
             g_fmtCenter.Get(), x0 + kPad, listTop, R - kPad, listTop + 80);
    } else {
        const int n = static_cast<int>(c.items.size());
        for (int i = std::max(0, static_cast<int>(g_ed.scroll / kEdRow)); i < n; ++i) {
            float y = listTop + i * kEdRow - g_ed.scroll;
            if (y >= listBot) break;
            if (g_ed.dragging && i == g_ed.drag) y = std::clamp(g_ed.dragY - kEdRow / 2, listTop, listBot - kEdRow);
            const MenuItem& m = c.items[i];
            const bool vis = c.visible[i];
            D2D1_RECT_F row = D2D1::RectF(x0 + 8, y + 1, R - 8, y + kEdRow - 1);
            if (g_ed.dragging && i == g_ed.drag)
                Round(row, 5, cAccent, 0.32f);
            else if (i == g_ed.sel)
                Round(row, 5, cAccent, 0.18f);
            else if (i == g_ed.hover)
                Round(row, 5, 0xFFFFFF, 0.05f);
            // checkbox
            Color(vis ? cAccent : cFaint);
            Glyph(vis ? 0xE73A : 0xE739, g_fmtIcon.Get(), x0 + 16, y, x0 + 40, y + kEdRow);
            const float a = vis ? 1.f : 0.45f;
            if (m.sep) {
                Color(cFaint, a);
                HLine(x0 + 72, R - 40, y + kEdRow / 2, cBorder);
                Text(L"separator", g_fmtRight.Get(), x0 + 44, y, x0 + 130, y + kEdRow);
            } else {
                if (m.hbm) {
                    auto it = g_ed.bmps.find(m.hbm);
                    if (it == g_ed.bmps.end()) it = g_ed.bmps.emplace(m.hbm, BitmapFromHBitmap(m.hbm)).first;
                    if (it->second) {
                        D2D1_SIZE_F bs = it->second->GetSize();
                        if (m.ownerDrawn)
                            g_rt->DrawBitmap(it->second.Get(), D2D1::RectF(x0 + 48, y + (kEdRow - bs.height) / 2, x0 + 48 + bs.width, y + (kEdRow + bs.height) / 2), a);
                        else
                            g_rt->DrawBitmap(it->second.Get(), D2D1::RectF(x0 + 48, y + 6, x0 + 64, y + 22), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                } else if (!m.iconPath.empty()) {  // Jolt's own entries (the templates): a folder, or the icon of the extension
                    if (m.icon == g_iconFolder) DrawIcon(m.iconPath, nullptr, nullptr, 0, false, x0 + 48, y + (kEdRow - kIcon) / 2, a);
                    else if (const int ix = IconForExt(m.text.c_str(), m.text.size()); ix >= 0)
                        if (ID2D1Bitmap* b = IconBitmap(ix)) g_rt->DrawBitmap(b, D2D1::RectF(x0 + 48, y + 6, x0 + 64, y + 22), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                if (!m.ownerDrawn) {
                    Color(cText, a);
                    Text(m.text, m.bold ? g_fmtNameBold.Get() : g_fmtName.Get(), x0 + 74, y, R - 120, y + kEdRow);
                }
                Color(cFaint, a);
                Text(m.key, g_fmtRight.Get(), R - 220, y, R - 40, y + kEdRow);
            }
            // handle
            Color(cFaint, 0.8f);
            Glyph(0xE700, g_fmtIconSm.Get(), R - 34, y, R - 14, y + kEdRow);
        }
        const float contentH = n * kEdRow, viewH = listBot - listTop;
        if (contentH > viewH) {
            const float thumbH = std::max(24.f, viewH * viewH / contentH);
            const float thumbY = listTop + (viewH - thumbH) * (g_ed.scroll / (contentH - viewH));
            Round(D2D1::RectF(R - 6, thumbY, R - 3, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.14f);
        }
    }
    g_rt->PopAxisAlignedClip();

    // footer
    HLine(x0, R, listBot);
    Color(cDim);
    Text(L"Checkbox: show or hide  ·  Drag or Ctrl+↑/↓ to reorder  ·  Space toggles  ·  Esc closes",
         g_fmtStatus.Get(), x0 + kPad, listBot, R - 130, B);
    const D2D1_RECT_F rb = EditorResetRect();
    Round(rb, 5, g_ed.hoverReset ? 0xFFFFFF : cBg, g_ed.hoverReset ? 0.12f : 1.f);
    Color(cText);
    Text(L"Reset", g_fmtCrumb.Get(), rb.left, rb.top, rb.right, rb.bottom);
}

void EditorMove(int from, int to) {
    EditorCtx& c = g_ed.ctxs[g_ed.cur];
    int n = static_cast<int>(c.items.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    MenuItem m = std::move(c.items[from]);
    bool v = c.visible[from];
    c.items.erase(c.items.begin() + from);
    c.visible.erase(c.visible.begin() + from);
    c.items.insert(c.items.begin() + to, std::move(m));
    c.visible.insert(c.visible.begin() + to, v);
    g_ed.sel = to;
    EditorSave(c);
}

void EditorSelectCtx(int i) {
    g_ed.cur = i;
    g_ed.sel = g_ed.hover = -1;
    g_ed.scroll = 0;
    // Windows builds the menu with every shell extension (tens of ms, hundreds the first time): first the
    // panel draws "Reading the menu…", then it reads it
    if (!g_ed.ctxs[i].loaded) PostMessageW(g_hwnd, WM_APP_MENUED, static_cast<WPARAM>(i), 0);
    Invalidate();
}

// WM_APP_MENUED: the panel is on screen, its menu can be read now.
void MenuEditorLoad(int i) {
    if (!g_ed.open || i < 0 || i >= kEdCtxCount || g_ed.ctxs[i].loaded) return;
    EditorLoadCtx(g_ed.ctxs[i]);
    Invalidate();
}

void MenuEditorClose() {
    if (!g_ed.open) return;
    if (g_ed.dragging && GetCapture() == g_hwnd) ReleaseCapture();
    g_ed = Editor{};  // the shell menus and their icons go with it
    Invalidate();
}

void ViewEditorClose();  // viewedit.inl
void PaletteClose();     // palette.inl

void OpenMenuEditor() {
    if (g_ed.open) return;
    PopupClose();
    PaletteClose();
    ViewEditorClose();
    CancelRename();
    g_ed = Editor{};
    g_ed.open = true;
    g_ed.ctxs[0].ctx = L"background";
    g_ed.ctxs[1].ctx = L"file";
    g_ed.ctxs[2].ctx = L"folder";
    g_ed.ctxs[3].ctx = L"background/new";
    EditorSelectCtx(0);
}

void MenuEditorMouseMove(float x, float y) {
    if (g_ed.dragging) {
        g_ed.dragY = y;
        const float top = g_ed.box.top + kEdTop;
        int to = EditorRowAt(std::clamp(y, top, top + EditorListH() - 1));
        if (to >= 0 && to != g_ed.drag) {
            EditorMove(g_ed.drag, to);
            g_ed.drag = to;
        }
        Invalidate();
        return;
    }
    const int ht = EditorTabAt(x, y), row = EditorRowAt(y);
    const bool hr = InRect(EditorResetRect(), x, y), hc = InRect(EditorCloseRect(), x, y);
    if (row != g_ed.hover || ht != g_ed.hoverTab || hr != g_ed.hoverReset || hc != g_ed.hoverClose) {
        g_ed.hover = row;
        g_ed.hoverTab = ht;
        g_ed.hoverReset = hr;
        g_ed.hoverClose = hc;
        Invalidate();
    }
}

void MenuEditorClick(float x, float y) {
    if (!InRect(g_ed.box, x, y) || InRect(EditorCloseRect(), x, y)) {  // outside, or the ×
        MenuEditorClose();
        return;
    }
    EditorCtx& c = g_ed.ctxs[g_ed.cur];
    int tab = EditorTabAt(x, y);
    if (tab >= 0) {
        EditorSelectCtx(tab);
        return;
    }
    if (InRect(EditorResetRect(), x, y)) {
        g_menuCfg.erase(c.ctx);
        SaveMenuCfg();
        EditorLoadCtx(c);
        g_ed.sel = -1;
        Invalidate();
        return;
    }
    int row = EditorRowAt(y);
    if (row < 0) return;
    g_ed.sel = row;
    if (x < g_ed.box.left + 44) {
        c.visible[row] = !c.visible[row];
        EditorSave(c);
    } else {
        g_ed.drag = row;
        g_ed.dragY = y;
        g_ed.dragging = true;
        SetCapture(g_hwnd);
    }
    Invalidate();
}

void MenuEditorMouseUp() {
    if (!g_ed.dragging) return;
    g_ed.dragging = false;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    Invalidate();
}

void MenuEditorWheel(int delta) {
    g_ed.scroll -= delta / 120.f * 3 * kEdRow;
    EditorClampScroll();
    Invalidate();
}

void MenuEditorKey(WPARAM vk) {
    EditorCtx& c = g_ed.ctxs[g_ed.cur];
    const bool ctrl = KeyDown(VK_CONTROL);
    const int n = static_cast<int>(c.items.size());
    switch (vk) {
        case VK_ESCAPE: MenuEditorClose(); return;
        case VK_UP:
            if (ctrl) EditorMove(g_ed.sel, g_ed.sel - 1);
            else if (n) g_ed.sel = std::max(0, g_ed.sel - 1);
            break;
        case VK_DOWN:
            if (ctrl) EditorMove(g_ed.sel, g_ed.sel + 1);
            else if (n) g_ed.sel = std::min(n - 1, g_ed.sel + 1);
            break;
        case VK_SPACE:
            if (g_ed.sel >= 0 && g_ed.sel < n) {
                c.visible[g_ed.sel] = !c.visible[g_ed.sel];
                EditorSave(c);
            }
            break;
        case VK_TAB: EditorSelectCtx((g_ed.cur + (KeyDown(VK_SHIFT) ? kEdCtxCount - 1 : 1)) % kEdCtxCount); return;
        default: return;
    }
    if (g_ed.sel >= 0) {
        float top = g_ed.sel * kEdRow, h = EditorListH();
        if (top < g_ed.scroll) g_ed.scroll = top;
        if (top + kEdRow > g_ed.scroll + h) g_ed.scroll = top + kEdRow - h;
        EditorClampScroll();
    }
    Invalidate();
}

// Editor de vistas: el panel que abre el botón de filtro del encabezado. Muestra las reglas de la
// vista como filas (estilo Notion): cada parte se cambia con un desplegable, los valores se escriben
// en el lugar, se reordenan arrastrando y se aplican al instante. Arriba se elige si se edita la vista
// de todas las carpetas o la de esta carpeta. Se incluye desde app.cpp después de palette.inl.

enum class VeK { ScopeAll, ScopeFolder, Sub, Label, Cell1, Cell2, Text, Delete, Handle, Add, Remove };
struct VeHit {
    D2D1_RECT_F r;
    VeK k;
    int step;
};
struct VeData {
    bool editAll = false;  // editando la vista general (si no, la de la carpeta)
    std::wstring folder;   // carpeta del panel al abrir
    std::vector<VeHit> hits;
    int hover = -1;
    D2D1_RECT_F panel{};
    std::vector<float> rowTops;  // para arrastrar
    int drag = -1, dropAt = -1;
    float dragY = 0, pressY = 0;
    bool dragging = false;
    int editStep = -1;  // paso cuyo valor se está escribiendo
    HWND edit = nullptr;
} g_ved;

constexpr float kVeW = 620.f, kVeTop = 44.f, kVeRow = 34.f;

// La vista que se está editando; nullptr = la carpeta todavía no tiene vista propia.
View* VeTarget() {
    if (g_ved.editAll) return &g_viewAll;
    View* v = ViewFor(g_ved.folder);
    return v == &g_viewAll ? nullptr : v;
}

void VeCommitText();

void ViewEditorClose() {
    if (!g_ve.open) return;
    VeCommitText();
    PopupClose();
    g_ve.open = false;
    g_ved.hover = -1;
    g_ved.drag = -1;
    Invalidate();
}

void ViewEditorOpen(int pane) {
    PaletteClose();
    CancelRename();
    ActivatePane(pane);
    g_ve.open = true;
    g_ve.pane = g_act;
    g_ved.folder = T().path;
    g_ved.editAll = g_ved.folder.empty() || ViewFor(g_ved.folder) == &g_viewAll;
    g_ved.hover = -1;
    g_ved.drag = -1;
    Invalidate();
}

std::wstring FolderTitle(const std::wstring& p) {
    if (p.empty()) return L"Este equipo";
    if (IsDriveRoot(p)) return DriveLabel(p[0]);
    return LeafOf(p);
}

// Textos de cada parte de una regla.
const wchar_t* kVeSortField[] = {L"Nombre", L"Fecha", L"Tamaño", L"Tipo de archivo"};
const wchar_t* kVeGroup[] = {L"Por día", L"Por mes", L"Por tipo", L"Por extensión"};
const wchar_t* kVeFolders[] = {L"Mezcladas con los archivos", L"Primero", L"Al final", L"Primero al ordenar por nombre"};
const wchar_t* kVeFold[] = {L"Plegados", L"Plegados menos hoy", L"Plegados menos el primero"};
const wchar_t* kVeCond[] = {L"Nombre contiene", L"Extensión es", L"Más nuevos que", L"Más viejos que",
                            L"Más grandes que", L"Más chicos que", L"Carpetas", L"Archivos", L"Ocultos"};
const wchar_t* kVeUnit[] = {L"", L"", L"días", L"días", L"MB", L"MB", L"", L"", L""};

std::wstring VeDirText(const Step& s) {
    bool asc = s.b != 0;
    switch (s.a) {
        case SfDate: return asc ? L"Más viejo primero" : L"Más nuevo primero";
        case SfSize: return asc ? L"Más chico primero" : L"Más grande primero";
        default: return asc ? L"A → Z" : L"Z → A";
    }
}

std::wstring VeLabel(const Step& s) {
    switch (s.kind) {
        case StepKind::Sort: return L"Ordenar por";
        case StepKind::Group: return L"Agrupar";
        case StepKind::Folders: return L"Carpetas";
        case StepKind::Fold: return L"Grupos";
        case StepKind::Show: return L"Mostrar solo";
        default: return L"Ocultar";
    }
}

std::wstring VeCell1(const Step& s) {
    switch (s.kind) {
        case StepKind::Sort: return kVeSortField[s.a];
        case StepKind::Group: return kVeGroup[s.a];
        case StepKind::Folders: return kVeFolders[s.a];
        case StepKind::Fold: return kVeFold[s.a];
        default: return kVeCond[s.a];
    }
}

// Un "botón" redondeado con desplegable.
float VePill(const std::wstring& text, float x, float y, VeK k, int step, bool dropdown = true) {
    float w = MeasureW(text, g_fmtName.Get()) + (dropdown ? 34 : 20);
    D2D1_RECT_F r = D2D1::RectF(x, y + 5, x + w, y + kVeRow - 5);
    int idx = static_cast<int>(g_ved.hits.size());
    Round(r, 5, 0xFFFFFF, g_ved.hover == idx ? 0.11f : 0.06f);
    Color(cText);
    Text(text, g_fmtName.Get(), r.left + 10, r.top, r.right, r.bottom);
    if (dropdown) {
        Color(cDim);
        Glyph(0xE70D, g_fmtIconSm.Get(), r.right - 22, r.top, r.right - 6, r.bottom);
    }
    g_ved.hits.push_back({r, k, step});
    return r.right + 8;
}

void DrawViewEditor(float W, float H) {
    (void)W;
    (void)H;
    if (!g_ve.open) return;
    g_ved.hits.clear();
    const PaneX g = PaneGeo(g_ve.pane);
    const float pw = std::min(kVeW, g.R - g.L - 16), px = g.L + 8, py = ListTop() - 4;
    View* v = VeTarget();
    const View& shown = v ? *v : *ViewFor(g_ved.folder);
    const int n = static_cast<int>(shown.steps.size());
    const bool folderView = !g_ved.editAll && v;
    const float ph = kVeTop + 6 + n * kVeRow + kVeRow + (folderView ? kVeRow : 0) + 30;
    g_ved.panel = D2D1::RectF(px, py, px + pw, py + ph);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(px - i, py - i + 2, px + pw + i, py + ph + i + 2), 10.f + i, 0x000000, 0.10f);
    Round(g_ved.panel, 10, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_ved.panel, 10, 10), g_brush.Get(), 1.f);

    // alcance: todas las carpetas / esta carpeta (o la carpeta de la que hereda)
    float x = px + 14, y = py + 6;
    Color(cDim);
    Glyph(0xE71C, g_fmtIconSm.Get(), x, y, x + 14, y + kVeRow);
    x += 22;
    auto chip = [&](const std::wstring& text, bool on, VeK k) {
        float w = MeasureW(text, g_fmtName.Get()) + 22;
        D2D1_RECT_F r = D2D1::RectF(x, y + 5, x + w, y + kVeRow - 5);
        int idx = static_cast<int>(g_ved.hits.size());
        if (on)
            Round(r, 12, cAccent, 0.30f);
        else
            Round(r, 12, 0xFFFFFF, g_ved.hover == idx ? 0.10f : 0.05f);
        Color(on ? cText : cDim);
        Text(text, g_fmtCenter.Get(), r.left, r.top, r.right, r.bottom);
        g_ved.hits.push_back({r, k, -1});
        x = r.right + 6;
    };
    chip(L"Todas las carpetas", g_ved.editAll, VeK::ScopeAll);
    if (!g_ved.folder.empty()) {
        View* eff = ViewFor(g_ved.folder);
        std::wstring label = L"Solo «" + FolderTitle(g_ved.folder) + L"»";
        if (eff != &g_viewAll && Key(eff->path) != Key(g_ved.folder)) label = L"«" + FolderTitle(eff->path) + L"» y subcarpetas";
        chip(label, !g_ved.editAll, VeK::ScopeFolder);
        if (folderView) {
            // casilla: también para las subcarpetas
            float cx = x + 8;
            D2D1_RECT_F box = D2D1::RectF(cx, y + kVeRow / 2 - 7, cx + 14, y + kVeRow / 2 + 7);
            Color(v->sub ? cAccent : cDim);
            g_rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3, 3), g_brush.Get(), 1.2f);
            if (v->sub) {
                Round(box, 3, cAccent);
                Color(0xFFFFFF);
                Glyph(0xE73E, g_fmtIconSm.Get(), box.left, box.top, box.right, box.bottom);
            }
            Color(cDim);
            Text(std::wstring(L"y sus subcarpetas"), g_fmtName.Get(), cx + 20, y, px + pw - 10, y + kVeRow);
            g_ved.hits.push_back({D2D1::RectF(cx - 4, y + 4, cx + 20 + MeasureW(L"y sus subcarpetas", g_fmtName.Get()), y + kVeRow - 4), VeK::Sub, -1});
        }
    }
    HLine(px + 10, px + pw - 10, py + kVeTop);

    // reglas
    y = py + kVeTop + 3;
    g_ved.rowTops.assign(n + 1, 0);
    for (int i = 0; i < n; ++i) {
        const Step& s = shown.steps[i];
        g_ved.rowTops[i] = y;
        const bool dragged = g_ved.dragging && i == g_ved.drag;
        if (dragged) Round(D2D1::RectF(px + 6, y + 2, px + pw - 6, y + kVeRow - 2), 6, cAccent, 0.12f);
        // agarradera: seis puntitos
        D2D1_RECT_F hr = D2D1::RectF(px + 8, y + 6, px + 24, y + kVeRow - 6);
        int hidx = static_cast<int>(g_ved.hits.size());
        for (int dy = 0; dy < 3; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                Round(D2D1::RectF(px + 13 + dx * 5, y + 11 + dy * 5, px + 15 + dx * 5, y + 13 + dy * 5), 1, 0xFFFFFF,
                      g_ved.hover == hidx || dragged ? 0.55f : 0.22f);
        g_ved.hits.push_back({hr, VeK::Handle, i});
        float cx = px + 30;
        if (s.kind == StepKind::Show || s.kind == StepKind::Hide) {
            cx = VePill(VeLabel(s), cx, y, VeK::Label, i);
        } else {
            Color(cDim);
            Text(VeLabel(s), g_fmtName.Get(), cx + 4, y, cx + 110, y + kVeRow);
            cx += 104;
        }
        cx = VePill(VeCell1(s), cx, y, VeK::Cell1, i);
        if (s.kind == StepKind::Sort) cx = VePill(VeDirText(s), cx, y, VeK::Cell2, i);
        if ((s.kind == StepKind::Show || s.kind == StepKind::Hide) && s.a <= CSmaller) {
            // valor escrito
            std::wstring val = s.text.empty() ? (s.a == CExt ? L"pdf, docx" : s.a == CName ? L"texto" : L"0") : s.text;
            float w = std::max(80.f, MeasureW(val, g_fmtName.Get()) + 24);
            D2D1_RECT_F r = D2D1::RectF(cx, y + 5, std::min(cx + w, px + pw - 80), y + kVeRow - 5);
            int idx = static_cast<int>(g_ved.hits.size());
            Round(r, 5, cBg);
            Color(g_ved.hover == idx ? cAccent : cBorder);
            g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5, 5), g_brush.Get(), 1.f);
            if (g_ved.editStep != i) {
                Color(s.text.empty() ? cFaint : cText);
                Text(val, g_fmtName.Get(), r.left + 9, r.top, r.right - 4, r.bottom);
            }
            g_ved.hits.push_back({r, VeK::Text, i});
            cx = r.right + 8;
            if (*kVeUnit[s.a]) {
                Color(cDim);
                Text(kVeUnit[s.a], wcslen(kVeUnit[s.a]), g_fmtName.Get(), cx, y, cx + 60, y + kVeRow);
            }
        }
        // quitar
        D2D1_RECT_F xr = D2D1::RectF(px + pw - 34, y + 7, px + pw - 12, y + kVeRow - 7);
        int xidx = static_cast<int>(g_ved.hits.size());
        if (g_ved.hover == xidx) Round(xr, 4, 0xFFFFFF, 0.10f);
        Color(g_ved.hover == xidx ? cText : cFaint);
        Glyph(gClose, g_fmtIconSm.Get(), xr.left, xr.top, xr.right, xr.bottom);
        g_ved.hits.push_back({xr, VeK::Delete, i});
        y += kVeRow;
    }
    g_ved.rowTops[n] = y;
    // indicador de dónde cae la regla que se arrastra
    if (g_ved.dragging && g_ved.dropAt >= 0) {
        float ly = g_ved.rowTops[g_ved.dropAt];
        Fill(D2D1::RectF(px + 10, ly - 1, px + pw - 10, ly + 1), cAccent);
    }
    // agregar
    {
        D2D1_RECT_F r = D2D1::RectF(px + 8, y + 4, px + 160, y + kVeRow - 4);
        int idx = static_cast<int>(g_ved.hits.size());
        if (g_ved.hover == idx) Round(r, 5, 0xFFFFFF, 0.07f);
        Color(g_ved.hover == idx ? cText : cDim);
        Glyph(gAdd, g_fmtIconSm.Get(), r.left + 6, r.top, r.left + 22, r.bottom);
        Text(L"Agregar regla", 13, g_fmtName.Get(), r.left + 28, r.top, r.right, r.bottom);
        g_ved.hits.push_back({r, VeK::Add, -1});
        y += kVeRow;
    }
    if (folderView) {
        std::wstring t = L"Quitar la vista de «" + FolderTitle(v->path) + L"»";
        float w = MeasureW(t, g_fmtName.Get()) + 16;
        D2D1_RECT_F r = D2D1::RectF(px + 8, y + 4, px + 8 + w, y + kVeRow - 4);
        int idx = static_cast<int>(g_ved.hits.size());
        if (g_ved.hover == idx) Round(r, 5, cError, 0.12f);
        Color(g_ved.hover == idx ? cError : cDim);
        Text(t, g_fmtName.Get(), r.left + 8, r.top, r.right, r.bottom);
        g_ved.hits.push_back({r, VeK::Remove, -1});
        y += kVeRow;
    }
    Color(cFaint);
    Text(L"Primero los grupos, después las carpetas, después el orden (el de más arriba manda).", g_fmtStatus.Get(),
         px + 14, y, px + pw - 10, y + 26);
    if (g_ved.editStep >= 0 && g_ved.edit) {
        // el cuadro de texto sigue a su celda
        for (const VeHit& h : g_ved.hits)
            if (h.k == VeK::Text && h.step == g_ved.editStep) {
                RECT sr = ScreenRect(h.r);
                POINT a{sr.left, sr.top};
                ScreenToClient(g_hwnd, &a);
                int pxl = MulDiv(9, g_dpi, 96), hh = MulDiv(18, g_dpi, 96);
                int top = a.y + (sr.bottom - sr.top - hh) / 2;
                SetWindowPos(g_ved.edit, HWND_TOP, a.x + pxl, top, sr.right - sr.left - pxl - 4, hh, SWP_SHOWWINDOW);
            }
    }
}

int VeHitAt(float x, float y) {
    for (int i = static_cast<int>(g_ved.hits.size()) - 1; i >= 0; --i) {
        const D2D1_RECT_F& r = g_ved.hits[i].r;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

// Asegura que exista la vista que se edita (al elegir "esta carpeta" se parte de la que se ve ahora).
View* VeEnsureTarget() {
    if (View* v = VeTarget()) return v;
    View nv = *ViewFor(g_ved.folder);
    nv.path = g_ved.folder;
    nv.sub = false;
    g_views[Key(g_ved.folder)] = nv;
    return &g_views[Key(g_ved.folder)];
}

void VeChanged() {
    ViewsChanged();
    Invalidate();
}

// Abre un desplegable debajo de una celda.
void VeDropdown(const D2D1_RECT_F& r, std::vector<MenuItem> items, std::function<void(UINT)> onSelect) {
    RECT sr = ScreenRect(r);
    sr.top = sr.bottom + MulDiv(2, g_dpi, 96);
    sr.bottom = sr.top;
    PopupOpen(std::move(items), sr, std::move(onSelect));
}

std::vector<MenuItem> VeItems(const wchar_t* const* names, int n, int current) {
    std::vector<MenuItem> out;
    for (int i = 0; i < n; ++i) {
        MenuItem m;
        m.text = names[i];
        m.id = static_cast<UINT>(i + 1);
        m.checked = i == current;
        out.push_back(m);
    }
    return out;
}

LRESULT CALLBACK VeEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_TAB)) {
        VeCommitText();
        SetFocus(g_hwnd);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        g_ved.editStep = -1;
        ShowWindow(h, SW_HIDE);
        SetFocus(g_hwnd);
        Invalidate();
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;
    if (m == WM_KILLFOCUS && g_ved.editStep >= 0) VeCommitText();
    return DefSubclassProc(h, m, w, l);
}

void VeCommitText() {
    if (g_ved.editStep < 0 || !g_ved.edit) return;
    int i = g_ved.editStep;
    g_ved.editStep = -1;
    ShowWindow(g_ved.edit, SW_HIDE);
    wchar_t buf[512];
    GetWindowTextW(g_ved.edit, buf, 512);
    View* v = VeTarget();
    if (v && i < static_cast<int>(v->steps.size()) && v->steps[i].text != Trim(buf)) {
        v->steps[i].text = Trim(buf);
        VeChanged();
    }
    Invalidate();
}

void VeBeginText(int step) {
    View* v = VeTarget();
    if (!v || step >= static_cast<int>(v->steps.size())) return;
    if (!g_ved.edit) {
        g_ved.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(g_ved.edit, VeEditProc, 3, 0);
    }
    SendMessageW(g_ved.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    SendMessageW(g_ved.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetWindowTextW(g_ved.edit, v->steps[step].text.c_str());
    g_ved.editStep = step;
    Invalidate();
    UpdateWindow(g_hwnd);  // ubica el cuadro sobre su celda
    SetFocus(g_ved.edit);
    SendMessageW(g_ved.edit, EM_SETSEL, 0, -1);
}

void VeClick(float x, float y) {
    VeCommitText();
    int hi = VeHitAt(x, y);
    const D2D1_RECT_F& pr = g_ved.panel;
    if (hi < 0) {
        if (!(x >= pr.left && x < pr.right && y >= pr.top && y < pr.bottom)) ViewEditorClose();
        return;
    }
    VeHit h = g_ved.hits[hi];
    View* v = VeTarget();
    switch (h.k) {
        case VeK::ScopeAll:
            g_ved.editAll = true;
            Invalidate();
            return;
        case VeK::ScopeFolder:
            g_ved.editAll = false;
            if (!VeTarget()) {
                VeEnsureTarget();
                VeChanged();
            }
            Invalidate();
            return;
        case VeK::Sub:
            if (v && !g_ved.editAll) {
                v->sub = !v->sub;
                VeChanged();
            }
            return;
        case VeK::Remove:
            if (v && !g_ved.editAll) {
                g_views.erase(Key(v->path));
                g_ved.editAll = true;
                VeChanged();
            }
            return;
        case VeK::Delete:
            v = VeEnsureTarget();
            if (h.step < static_cast<int>(v->steps.size())) v->steps.erase(v->steps.begin() + h.step);
            VeChanged();
            return;
        case VeK::Handle:
            g_ved.drag = h.step;
            g_ved.pressY = y;
            g_ved.dragging = false;
            SetCapture(g_hwnd);
            return;
        case VeK::Text:
            VeEnsureTarget();
            VeBeginText(h.step);
            return;
        case VeK::Add: {
            std::vector<MenuItem> items;
            const wchar_t* kinds[] = {L"Ordenar por…", L"Agrupar…", L"Carpetas primero o al final…", L"Mostrar solo…", L"Ocultar…",
                                      L"Grupos plegados al entrar…"};
            for (int i = 0; i < 6; ++i) {
                MenuItem m;
                m.text = kinds[i];
                m.id = static_cast<UINT>(i + 1);
                items.push_back(m);
            }
            VeDropdown(h.r, items, [](UINT id) {
                View* t = VeEnsureTarget();
                Step s;
                switch (id) {
                    case 1: s = {StepKind::Sort, SfName, 1, L""}; break;
                    case 2: s = {StepKind::Group, GbDay, 0, L""}; break;
                    case 3: s = {StepKind::Folders, FmFirst, 0, L""}; break;
                    case 4: s = {StepKind::Show, CExt, 0, L""}; break;
                    case 6: s = {StepKind::Fold, FdButToday, 0, L""}; break;
                    default: s = {StepKind::Hide, CName, 0, L""}; break;
                }
                t->steps.push_back(s);
                VeChanged();
                if (s.kind == StepKind::Show || s.kind == StepKind::Hide) VeBeginText(static_cast<int>(t->steps.size()) - 1);
            });
            return;
        }
        default: break;
    }
    // desplegables de una regla
    const View& shown = v ? *v : *ViewFor(g_ved.folder);
    if (h.step < 0 || h.step >= static_cast<int>(shown.steps.size())) return;
    const Step s = shown.steps[h.step];
    const int step = h.step;
    auto apply = [step](std::function<void(Step&)> f) {
        View* t = VeEnsureTarget();
        if (step < static_cast<int>(t->steps.size())) f(t->steps[step]);
        VeChanged();
    };
    if (h.k == VeK::Label) {
        const wchar_t* names[] = {L"Mostrar solo", L"Ocultar"};
        VeDropdown(h.r, VeItems(names, 2, s.kind == StepKind::Show ? 0 : 1), [apply](UINT id) {
            apply([id](Step& st) { st.kind = id == 1 ? StepKind::Show : StepKind::Hide; });
        });
    } else if (h.k == VeK::Cell1) {
        if (s.kind == StepKind::Sort)
            VeDropdown(h.r, VeItems(kVeSortField, SfCount, s.a), [apply](UINT id) {
                apply([id](Step& st) {
                    st.a = static_cast<int>(id - 1);
                    st.b = st.a == SfName || st.a == SfExt ? 1 : 0;  // fecha y tamaño: lo más nuevo/grande primero
                });
            });
        else if (s.kind == StepKind::Group)
            VeDropdown(h.r, VeItems(kVeGroup, GbCount, s.a), [apply](UINT id) { apply([id](Step& st) { st.a = static_cast<int>(id - 1); }); });
        else if (s.kind == StepKind::Fold)
            VeDropdown(h.r, VeItems(kVeFold, FdCount, s.a), [apply](UINT id) { apply([id](Step& st) { st.a = static_cast<int>(id - 1); }); });
        else if (s.kind == StepKind::Folders)
            VeDropdown(h.r, VeItems(kVeFolders, FmCount, s.a), [apply](UINT id) { apply([id](Step& st) { st.a = static_cast<int>(id - 1); }); });
        else
            VeDropdown(h.r, VeItems(kVeCond, CCount, s.a), [apply, step](UINT id) {
                apply([id](Step& st) {
                    st.a = static_cast<int>(id - 1);
                    st.text.clear();
                });
                if (static_cast<int>(id - 1) <= CSmaller) VeBeginText(step);
            });
    } else if (h.k == VeK::Cell2 && s.kind == StepKind::Sort) {
        Step a = s, d = s;
        a.b = 1;
        d.b = 0;
        std::wstring na = VeDirText(a), nd = VeDirText(d);
        const wchar_t* names[] = {na.c_str(), nd.c_str()};
        VeDropdown(h.r, VeItems(names, 2, s.b ? 0 : 1), [apply](UINT id) { apply([id](Step& st) { st.b = id == 1 ? 1 : 0; }); });
    }
}

void VeMouseMove(float x, float y) {
    if (g_ved.drag >= 0) {
        if (!g_ved.dragging && std::abs(y - g_ved.pressY) > 4) g_ved.dragging = true;
        if (g_ved.dragging) {
            g_ved.dragY = y;
            int at = static_cast<int>(g_ved.rowTops.size()) - 1;
            for (size_t i = 0; i + 1 < g_ved.rowTops.size(); ++i)
                if (y < g_ved.rowTops[i] + kVeRow / 2) {
                    at = static_cast<int>(i);
                    break;
                }
            g_ved.dropAt = at;
            Invalidate();
        }
        return;
    }
    int h = VeHitAt(x, y);
    if (h != g_ved.hover) {
        g_ved.hover = h;
        Invalidate();
    }
}

void VeMouseUp() {
    if (g_ved.drag < 0) return;
    int from = g_ved.drag, to = g_ved.dropAt;
    bool moved = g_ved.dragging && to >= 0 && to != from && to != from + 1;
    g_ved.drag = -1;
    g_ved.dragging = false;
    g_ved.dropAt = -1;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    if (moved) {
        View* v = VeEnsureTarget();
        if (from < static_cast<int>(v->steps.size())) {
            Step s = v->steps[from];
            v->steps.erase(v->steps.begin() + from);
            if (to > from) --to;
            v->steps.insert(v->steps.begin() + std::min(to, static_cast<int>(v->steps.size())), s);
            VeChanged();
        }
    }
    Invalidate();
}

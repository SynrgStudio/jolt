// Paleta de comandos (Ctrl+K): se dibuja sobre la ventana principal, sin ventana aparte.
// Se incluye desde app.cpp después de dragdrop.inl.

struct PalItem {
    std::wstring title, detail, shortcut;
    std::function<void()> run;
    Cmd cmd = Cmd::None;  // si es un comando con atajo configurable
    std::wstring path;    // Ir a: la carpeta o archivo
    bool pinned = false;  // siempre visible y arriba (la ruta escrita)
    int bonus = 0;        // Ir a: los recientes y lugares conocidos van antes que lo del índice
};

struct Palette {
    bool open = false;
    int mode = 0;        // 0 = comandos (Ctrl+K), 1 = Ir a (Ctrl+P)
    std::wstring text;
    std::wstring match;  // lo que se busca (en Ir a con una ruta escrita: lo que sigue a la última barra)
    std::vector<PalItem> all;
    std::vector<std::pair<int, int>> hits;  // (índice en all, puntaje) ya ordenados
    int sel = 0;
    int first = 0;  // primera fila visible (desplazamiento)
    int capture = -1;       // índice (en hits) del comando cuyo atajo se está capturando
    int hoverShortcut = -1; // fila visible cuyo atajo está bajo el mouse
    D2D1_RECT_F panel{};
    std::vector<D2D1_RECT_F> rowRects, shortcutRects;
} g_pal;

constexpr float kPalW = 600.f, kPalInputH = 40.f, kPalRow = 30.f;
constexpr int kPalMaxRows = 12;

// Texto legible de una tecla: "Ctrl+Shift+N"
std::wstring KeyText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & ModCtrl) s += L"Ctrl+";
    if (mods & ModShift) s += L"Shift+";
    if (mods & ModAlt) s += L"Alt+";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return s + static_cast<wchar_t>(vk);
    if (vk >= VK_F1 && vk <= VK_F24) return s + L"F" + std::to_wstring(vk - VK_F1 + 1);
    for (const KeyName& k : kKeyNames)  // el primer nombre de cada tecla es el castellano
        if (k.vk == vk) {
            std::wstring n = k.name;
            n[0] = towupper(n[0]);
            return s + n;
        }
    return s + L"?";
}

std::wstring ShortcutFor(Cmd cmd) {
    for (const KeyBind& k : g_cfg.keys)
        if (k.cmd == cmd) return KeyText(k.mods, k.vk);
    return L"";
}

void GotoBuild();

void PaletteFilter() {
    g_pal.hits.clear();
    g_pal.match = g_pal.text;
    if (g_pal.mode == 1) GotoBuild();
    std::wstring pat = Lower(g_pal.match);
    for (size_t i = 0; i < g_pal.all.size(); ++i) {
        const PalItem& it = g_pal.all[i];
        if (it.pinned) {
            g_pal.hits.push_back({static_cast<int>(i), 1 << 28});
            continue;
        }
        int s1 = 0, s2 = 0;
        bool m1 = FuzzyMatch(it.title.c_str(), it.title.size(), pat, s1, nullptr);
        bool m2 = !it.detail.empty() && FuzzyMatch(it.detail.c_str(), it.detail.size(), pat, s2, nullptr);
        if (!m1 && !m2) continue;
        g_pal.hits.push_back({static_cast<int>(i), std::max(m1 ? s1 : -1, m2 ? s2 - 10 : -1) + it.bonus});
    }
    if (!pat.empty())
        std::stable_sort(g_pal.hits.begin(), g_pal.hits.end(), [](auto& a, auto& b) { return a.second > b.second; });
    g_pal.sel = 0;
    g_pal.first = 0;
}

// Mantiene la fila seleccionada a la vista.
void PaletteEnsureVisible() {
    if (g_pal.sel < g_pal.first) g_pal.first = g_pal.sel;
    if (g_pal.sel >= g_pal.first + kPalMaxRows) g_pal.first = g_pal.sel - kPalMaxRows + 1;
    int maxFirst = std::max(0, static_cast<int>(g_pal.hits.size()) - kPalMaxRows);
    g_pal.first = std::clamp(g_pal.first, 0, maxFirst);
}

void PaletteWheel(int delta) {
    int maxFirst = std::max(0, static_cast<int>(g_pal.hits.size()) - kPalMaxRows);
    g_pal.first = std::clamp(g_pal.first - delta * 3, 0, maxFirst);
    g_pal.sel = std::clamp(g_pal.sel, g_pal.first, g_pal.first + kPalMaxRows - 1);
    Invalidate();
}

void PaletteClose() {
    g_pal.open = false;
    g_pal.all.clear();
    g_pal.hits.clear();
    Invalidate();
}

void PaletteBuild() {
    g_pal.all.clear();
    auto add = [&](std::wstring title, std::wstring detail, std::wstring shortcut, std::function<void()> run) {
        g_pal.all.push_back({std::move(title), std::move(detail), std::move(shortcut), std::move(run)});
    };
    // comandos con atajo
    for (const CmdDef& c : kCommands) {
        if (c.cmd == Cmd::Paleta) continue;
        std::wstring title = c.help;
        title[0] = towupper(title[0]);
        Cmd cmd = c.cmd;
        add(title, c.name, ShortcutFor(c.cmd), [cmd] { RunCommand(cmd); });
        g_pal.all.back().cmd = cmd;
    }
    // orden
    add(L"Ordenar por nombre", L"orden", L"", [] { SetSortExplicit(SortBy::Name, true); });
    add(L"Ordenar por fecha, lo más nuevo primero", L"orden", L"", [] { SetSortExplicit(SortBy::Date, false); });
    add(L"Ordenar por fecha, lo más viejo primero", L"orden", L"", [] { SetSortExplicit(SortBy::Date, true); });
    add(L"Ordenar por tamaño, lo más grande primero", L"orden", L"", [] { SetSortExplicit(SortBy::Size, false); });
    add(L"Ordenar por tamaño, lo más chico primero", L"orden", L"", [] { SetSortExplicit(SortBy::Size, true); });
    add(L"Invertir el orden", L"orden", L"", [] {
        SortBy by;
        bool asc;
        if (ViewPrimarySort(CurView(), by, asc)) SetSortExplicit(by, !asc);
        else if (const Step* s = FirstStep(CurView(), StepKind::Sort)) SetSortField(s->a, s->b == 0);
    });
    {
        const Step* f = FirstStep(CurView(), StepKind::Folders);
        int mode = !f ? 0 : f->a == FmFirst ? 1 : f->a == FmFirstByName ? 2 : 0;
        const wchar_t* ffNames[3] = {L"mezcladas con los archivos", L"siempre primero", L"primero solo al ordenar por nombre"};
        add(std::wstring(L"Carpetas: ") + ffNames[mode] + L"  →  cambiar", L"orden carpetas primero", L"",
            [mode] { SetFoldersFirst((mode + 1) % 3); });
    }
    add(L"Vista de esta carpeta: ordenar, agrupar, filtrar…", L"vista filtro agrupar reglas", L"", [] { ViewEditorOpen(g_act); });
    // favoritos y pestañas
    for (size_t i = 0; i < g_favs.size(); ++i) {
        std::wstring p = g_favs[i].path;
        add(L"Ir a: " + g_favs[i].name, p, i < 9 ? L"Ctrl+" + std::to_wstring(i + 1) : L"", [p] { Navigate(T(), p, true); });
    }
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        int idx = static_cast<int>(i);
        add(L"Pestaña: " + TabTitle(*g_tabs[i]), g_tabs[i]->path.empty() ? L"Este equipo" : g_tabs[i]->path, L"",
            [idx] { SelectTab(idx); });
    }
    add(L"Ir a: Este equipo", L"unidades", L"", [] { Navigate(T(), L"", true); });
    // app
    add(L"Reindexar los discos ya (rápido, pide permiso de administrador)", L"índice everything mft buscar disco", L"",
        [] { IdxReindexElevated(); });
    add(L"Editar config.txt", L"configuración", L"", [] {
        std::wstring f = ConfigFile();
        if (!f.empty()) ShellExecuteW(nullptr, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    add(L"Abrir la carpeta de configuración", L"configuración", L"", [] { NewTab(ConfigDir()); });
    add(L"Editor del menú contextual", L"menú", L"", [] { OpenMenuEditor(); });
    add(L"Acerca de Jolt", L"", L"", [] {
        MessageBoxW(g_hwnd, L"Jolt — explorador de archivos.\n\nC++ · Win32 · Direct2D.", L"Acerca de Jolt",
                    MB_OK | MB_ICONINFORMATION);
    });
}

void PaletteOpen(int mode = 0) {
    PopupClose();
    CancelRename();
    g_pal.open = true;
    g_pal.mode = mode;
    g_pal.capture = -1;
    g_pal.hoverShortcut = -1;
    g_pal.text.clear();
    if (mode == 0) PaletteBuild();
    PaletteFilter();
    Invalidate();
}

// ---- Ir a (Ctrl+P): recientes, favoritos, lugares del sistema, unidades, o una ruta escrita
void GotoOpen(const std::wstring& p, bool newTab) {
    DWORD a = p.empty() ? FILE_ATTRIBUTE_DIRECTORY : GetFileAttributesW(ToExtendedPath(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        S.message = L"Ya no existe: " + p;
        Invalidate();
        return;
    }
    std::wstring dir = p;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        dir = ParentOf(p);
        g_lastSel[Key(dir)] = LeafOf(p);
        AddRecent(p);
    }
    if (newTab)
        NewTab(dir);
    else
        Navigate(T(), dir, true);
}

std::wstring PlaceTitle(const std::wstring& p) {
    if (p.empty()) return L"Este equipo";
    if (IsDriveRoot(p)) return DriveLabel(p[0]);
    return LeafOf(p);
}

void GotoBuild() {
    g_pal.all.clear();
    std::unordered_set<std::wstring> seen;
    auto add = [&](const std::wstring& title, const std::wstring& path, bool pinned = false, int bonus = 15) {
        if (!seen.insert(Key(path)).second) return;
        PalItem it;
        it.title = title;
        it.detail = path.empty() ? L"unidades" : path;
        it.path = path;
        it.pinned = pinned;
        it.bonus = bonus;
        it.run = [path] { GotoOpen(path, false); };
        g_pal.all.push_back(std::move(it));
    };
    std::wstring text = Trim(g_pal.text);
    // una ruta escrita: la carpeta y su contenido, filtrado por lo que sigue a la última barra
    if (text.find_first_of(L"\\/:") != std::wstring::npos || (!text.empty() && (text[0] == L'%' || text[0] == L'~'))) {
        std::wstring t = text;
        for (wchar_t& c : t)
            if (c == L'/') c = L'\\';
        if (t[0] == L'~') {
            wchar_t prof[MAX_PATH];
            if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) t = prof + t.substr(1);
        }
        wchar_t ex[4096];
        if (ExpandEnvironmentStringsW(t.c_str(), ex, 4096)) t = ex;
        size_t bs = t.find_last_of(L'\\');
        std::wstring dir = bs == std::wstring::npos ? t : t.substr(0, bs + 1);
        g_pal.match = bs == std::wstring::npos ? L"" : t.substr(bs + 1);
        if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
        dir = Normalize(dir);
        DWORD a = GetFileAttributesW(ToExtendedPath(dir).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring whole = Normalize(t);
            DWORD wa = GetFileAttributesW(ToExtendedPath(whole).c_str());
            // lo escrito tal cual, si existe; la carpeta, solo mientras no se escribió nada después de la barra
            if (wa != INVALID_FILE_ATTRIBUTES && Key(whole) != Key(dir)) add(L"Ir a " + whole, whole, true);
            if (g_pal.match.empty()) add(L"Ir a " + dir, dir, true);
            std::shared_ptr<const Listing> l;
            auto it = g_cache.find(Key(dir));
            if (it != g_cache.end()) {
                l = it->second;
            } else if (IsLocalFixed(dir)) {
                auto nl = std::make_shared<Listing>();
                EnumerateDir(dir, EnumMethod::Nt, *nl);
                SortListing(*nl, SortBy::Name, true, true);
                if (!nl->error) g_cache[Key(dir)] = nl;
                l = nl;
            }
            if (l && !l->error)
                for (const Entry& e : l->entries) {
                    if (!g_cfg.showHidden && (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                    std::wstring n(l->name(e), e.nameLen);
                    add(n, Join(dir, n.c_str()));
                }
        }
        return;
    }
    for (const std::wstring& r : g_recents) add(PlaceTitle(r), r);
    for (const Favorite& f : g_favs) add(f.name, f.path);
    struct K {
        const wchar_t* name;
        const KNOWNFOLDERID* id;
    } known[] = {{L"Escritorio", &FOLDERID_Desktop},       {L"Descargas", &FOLDERID_Downloads},
                 {L"Documentos", &FOLDERID_Documents},     {L"Imágenes", &FOLDERID_Pictures},
                 {L"Música", &FOLDERID_Music},             {L"Videos", &FOLDERID_Videos},
                 {L"Usuario", &FOLDERID_Profile},          {L"OneDrive", &FOLDERID_SkyDrive},
                 {L"AppData", &FOLDERID_RoamingAppData},   {L"AppData Local", &FOLDERID_LocalAppData},
                 {L"Archivos de programa", &FOLDERID_ProgramFiles},
                 {L"Archivos de programa (x86)", &FOLDERID_ProgramFilesX86},
                 {L"Inicio (programas al arrancar)", &FOLDERID_Startup},
                 {L"Menú Inicio", &FOLDERID_Programs},     {L"Enviar a", &FOLDERID_SendTo},
                 {L"Fuentes", &FOLDERID_Fonts},            {L"Windows", &FOLDERID_Windows},
                 {L"System32", &FOLDERID_System}};
    for (const K& k : known) {
        std::wstring p = Known(*k.id);
        if (!p.empty()) add(k.name, Normalize(p));
    }
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp)) add(L"Temporales", Normalize(tmp));
    add(L"Este equipo", L"");
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            add(DriveLabel(root[0]), root);
        }
    // todo el disco (índice global): llega un momento después y se suma a la lista
    IdxRequest(text);
    if (text.size() >= 2 && g_ixRes.query == text)
        for (auto& [p, dir] : g_ixRes.hits) add(PlaceTitle(p), p, false, 0);
}

void PaletteRun(int hit) {
    if (hit < 0 || hit >= static_cast<int>(g_pal.hits.size())) return;
    auto run = g_pal.all[g_pal.hits[hit].first].run;
    PaletteClose();
    if (run) run();
}

// Nombre del comando (config) para un Cmd.
const wchar_t* CmdName(Cmd cmd) {
    for (const CmdDef& c : kCommands)
        if (c.cmd == cmd) return c.name;
    return L"";
}

// Aplica una tecla capturada al comando: reemplaza su primer atajo, conserva los demás, y lo escribe en config.txt.
void PaletteAssignKey(Cmd cmd, UINT mods, UINT vk, bool remove) {
    std::vector<KeyBind> mine;
    for (const KeyBind& k : g_cfg.keys)
        if (k.cmd == cmd) mine.push_back(k);
    if (!mine.empty()) mine.erase(mine.begin());
    if (!remove) mine.insert(mine.begin(), {mods, vk, cmd});
    std::wstring specs;
    for (size_t i = 0; i < mine.size(); ++i) {
        bool dup = false;
        for (size_t j = 0; j < i; ++j) dup |= mine[j].mods == mine[i].mods && mine[j].vk == mine[i].vk;
        if (dup) continue;
        specs += (specs.empty() ? L"" : L", ") + KeyText(mine[i].mods, mine[i].vk);
    }
    ConfigWriteShortcut(CmdName(cmd), specs, mods, remove ? 0 : vk);
    // reflejar ya mismo en memoria (el archivo también dispara la recarga)
    if (!remove)
        g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(),
                                        [&](const KeyBind& k) { return k.mods == mods && k.vk == vk; }),
                         g_cfg.keys.end());
    g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(), [&](const KeyBind& k) { return k.cmd == cmd; }),
                     g_cfg.keys.end());
    for (const KeyBind& k : mine) g_cfg.keys.push_back(k);
    // reconstruir la paleta conservando texto, selección y desplazamiento
    std::wstring text = g_pal.text;
    int sel = g_pal.sel, first = g_pal.first;
    PaletteBuild();
    g_pal.text = text;
    PaletteFilter();
    g_pal.sel = std::min(sel, std::max(0, static_cast<int>(g_pal.hits.size()) - 1));
    g_pal.first = first;
    S.message = remove ? L"Atajo quitado" : L"Atajo guardado: " + KeyText(mods, vk);
}

bool PaletteCaptureKey(WPARAM vk) {
    if (g_pal.capture < 0) return false;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return true;
    int hit = g_pal.capture;
    g_pal.capture = -1;
    if (vk == VK_ESCAPE) {
        Invalidate();
        return true;
    }
    Cmd cmd = hit < static_cast<int>(g_pal.hits.size()) ? g_pal.all[g_pal.hits[hit].first].cmd : Cmd::None;
    if (cmd == Cmd::None) return true;
    UINT mods = (KeyDown(VK_CONTROL) ? ModCtrl : 0) | (KeyDown(VK_SHIFT) ? ModShift : 0) |
                (KeyDown(VK_MENU) ? ModAlt : 0);
    if ((vk == VK_DELETE || vk == VK_BACK) && mods == 0) {
        PaletteAssignKey(cmd, 0, 0, true);
        Invalidate();
        return true;
    }
    // las teclas de cursor sin modificadores son fijas
    if (mods == 0 && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END)) {
        S.message = L"Esa tecla es fija; probá con Ctrl, Shift o Alt";
        Invalidate();
        return true;
    }
    PaletteAssignKey(cmd, mods, static_cast<UINT>(vk), false);
    Invalidate();
    return true;
}

void PaletteKey(WPARAM vk) {
    if (PaletteCaptureKey(vk)) return;
    int n = static_cast<int>(g_pal.hits.size());
    switch (vk) {
        case VK_ESCAPE: PaletteClose(); return;
        case VK_RETURN:
            if (g_pal.mode == 1 && KeyDown(VK_CONTROL) && g_pal.sel < n) {
                std::wstring p = g_pal.all[g_pal.hits[g_pal.sel].first].path;
                PaletteClose();
                GotoOpen(p, true);  // Ctrl+Enter: en una pestaña nueva
                return;
            }
            PaletteRun(g_pal.sel);
            return;
        case VK_DOWN: if (n) g_pal.sel = (g_pal.sel + 1) % n; break;
        case VK_UP: if (n) g_pal.sel = (g_pal.sel + n - 1) % n; break;
        case VK_NEXT: if (n) g_pal.sel = std::min(n - 1, g_pal.sel + kPalMaxRows - 1); break;
        case VK_PRIOR: if (n) g_pal.sel = std::max(0, g_pal.sel - (kPalMaxRows - 1)); break;
        case VK_HOME: g_pal.sel = 0; break;
        case VK_END: if (n) g_pal.sel = n - 1; break;
        case VK_BACK:
            if (!g_pal.text.empty()) {
                g_pal.text.pop_back();
                PaletteFilter();
            }
            break;
        default: return;
    }
    PaletteEnsureVisible();
    Invalidate();
}

void PaletteChar(wchar_t c) {
    if (c < 32 || g_pal.capture >= 0) return;
    g_pal.text += c;
    PaletteFilter();
    Invalidate();
}

// Dibuja un texto resaltando las posiciones coincidentes.
void TextHighlighted(const std::wstring& s, const std::wstring& patLower, IDWriteTextFormat* fmt, float l, float t,
                     float r, float b, UINT32 color, UINT32 hiColor, float alpha) {
    std::vector<int> pos;
    int score;
    if (patLower.empty() || !FuzzyMatch(s.c_str(), s.size(), patLower, score, &pos) || pos.empty()) {
        Color(color, alpha);
        Text(s, fmt, l, t, r, b);
        return;
    }
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, std::max(1.f, r - l), b - t, &lay))) return;
    g_brushAccent->SetColor(D2D1::ColorF(hiColor, alpha));
    for (int p : pos) {
        DWRITE_TEXT_RANGE range{static_cast<UINT32>(p), 1};
        lay->SetDrawingEffect(g_brushAccent.Get(), range);
        lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
    }
    Color(color, alpha);
    g_rt->DrawTextLayout(D2D1::Point2F(l, t), lay.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawPalette(float W, float H) {
    if (!g_pal.open) return;
    (void)H;
    const float pw = std::min(kPalW, W - 40), px = (W - pw) / 2, py = kTabH + kToolH + 14;
    const int total = static_cast<int>(g_pal.hits.size());
    const int rows = std::min(kPalMaxRows, total - g_pal.first);
    const float ph = kPalInputH + rows * kPalRow + (rows ? 8 : 0);
    g_pal.panel = D2D1::RectF(px, py, px + pw, py + ph);
    // sombra suave y panel
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(px - i, py - i + 2, px + pw + i, py + ph + i + 2), 10.f + i, 0x000000, 0.10f);
    Round(g_pal.panel, 10, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_pal.panel, 10, 10), g_brush.Get(), 1.f);
    // entrada
    Color(cDim);
    Glyph(0xE721, g_fmtIcon.Get(), px + 10, py, px + 34, py + kPalInputH);  // lupa
    std::wstring shown = g_pal.text;
    float tw = MeasureW(shown, g_fmtName.Get());
    if (shown.empty()) {
        Color(cFaint);
        Text(g_pal.mode == 1 ? L"Ir a…  recientes, favoritos, lugares, unidades o una ruta" : L"Escribí un comando…",
             g_fmtName.Get(), px + 38, py, px + pw - 12, py + kPalInputH);
    } else {
        Color(cText);
        Text(shown, g_fmtName.Get(), px + 38, py, px + pw - 12, py + kPalInputH);
    }
    Fill(D2D1::RectF(px + 38 + tw + 1, py + 12, px + 38 + tw + 2, py + kPalInputH - 12), cText);  // caret
    if (g_pal.mode == 1 && (g_ixCount > 0 || g_ixBuilding)) {
        // estado del índice de los discos
        std::wstring st = g_ixBuilding && g_ixCount == 0 ? L"indexando los discos…"
                          : Thousands(static_cast<int>(std::min<uint64_t>(g_ixCount, INT_MAX))) + (g_ixBuilding ? L" en el índice · actualizando" : L" en el índice");
        Color(cFaint);
        Text(st, g_fmtRight.Get(), px + 38 + tw + 20, py, px + pw - 14, py + kPalInputH);
    }
    if (rows) HLine(px + 10, px + pw - 10, py + kPalInputH);
    g_pal.rowRects.assign(rows, {});
    g_pal.shortcutRects.assign(rows, {});
    std::wstring pat = Lower(g_pal.match);
    float y = py + kPalInputH + 4;
    for (int i = 0; i < rows; ++i) {
        const int hi = g_pal.first + i;
        const PalItem& it = g_pal.all[g_pal.hits[hi].first];
        D2D1_RECT_F r = D2D1::RectF(px + 6, y + 1, px + pw - 6, y + kPalRow - 1);
        g_pal.rowRects[i] = r;
        if (hi == g_pal.sel) Round(r, 6, cAccent, 0.22f);
        float right = r.right - 10;
        if (it.cmd != Cmd::None) {
            // celda del atajo: clickeable para cambiarlo
            const bool capturing = hi == g_pal.capture;
            std::wstring label = capturing ? L"Presioná una tecla…  (Esc cancela, Supr quita)"
                                 : it.shortcut.empty() ? L"sin atajo" : it.shortcut;
            float w = MeasureW(label, g_fmtRight.Get()) + 16;
            D2D1_RECT_F cell = D2D1::RectF(right - w, y + 4, right + 2, y + kPalRow - 4);
            g_pal.shortcutRects[i] = cell;
            if (capturing) {
                Round(cell, 4, cAccent, 0.25f);
                Color(cAccent);
                g_rt->DrawRoundedRectangle(D2D1::RoundedRect(cell, 4, 4), g_brush.Get(), 1.f);
            } else if (i == g_pal.hoverShortcut) {
                Round(cell, 4, 0xFFFFFF, 0.10f);
            }
            Color(capturing ? cText : it.shortcut.empty() ? cFaint : cDim);
            Text(label, g_fmtRight.Get(), cell.left, y, cell.right - 8, y + kPalRow);
            right = cell.left - 8;
        } else if (!it.shortcut.empty()) {
            Color(cDim);
            Text(it.shortcut, g_fmtRight.Get(), right - 160, y, right, y + kPalRow);
            right -= MeasureW(it.shortcut, g_fmtRight.Get()) + 16;
        }
        float dw = it.detail.empty() ? 0 : std::min(MeasureW(it.detail, g_fmtRight.Get()), (right - r.left) * 0.55f);
        float tx = r.left + 10;
        if (g_pal.mode == 1) {
            DrawIcon(it.path, nullptr, nullptr, 0, IsDriveRoot(it.path), tx, y + (kPalRow - kIcon) / 2, 1.f);
            tx += 26;
        }
        TextHighlighted(it.title, it.pinned ? L"" : pat, g_fmtName.Get(), tx, y, right - dw - 12, y + kPalRow, cText, cAccent);
        if (dw > 0) TextHighlighted(it.detail, pat, g_fmtRight.Get(), right - dw, y, right, y + kPalRow, cFaint, cAccent);
        y += kPalRow;
    }
    if (total > kPalMaxRows) {
        // barra de desplazamiento finita
        const float top = py + kPalInputH + 4, viewH = kPalMaxRows * kPalRow;
        const float thumbH = std::max(20.f, viewH * kPalMaxRows / total);
        const float thumbY = top + (viewH - thumbH) * (static_cast<float>(g_pal.first) / (total - kPalMaxRows));
        Round(D2D1::RectF(px + pw - 7, thumbY, px + pw - 4, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.18f);
    }
}

int PaletteShortcutAt(float x, float y) {
    for (size_t i = 0; i < g_pal.shortcutRects.size(); ++i) {
        const D2D1_RECT_F& c = g_pal.shortcutRects[i];
        if (c.right > c.left && x >= c.left && x < c.right && y >= c.top && y < c.bottom) return static_cast<int>(i);
    }
    return -1;
}

void PaletteHover(float x, float y) {
    int h = PaletteShortcutAt(x, y);
    if (h != g_pal.hoverShortcut) {
        g_pal.hoverShortcut = h;
        Invalidate();
    }
}

// Devuelve true si el click fue consumido por la paleta.
bool PaletteClick(float x, float y) {
    if (!g_pal.open) return false;
    int sc = PaletteShortcutAt(x, y);
    if (sc >= 0) {
        g_pal.capture = g_pal.first + sc;
        g_pal.sel = g_pal.capture;
        Invalidate();
        return true;
    }
    if (g_pal.capture >= 0) {
        g_pal.capture = -1;
        Invalidate();
    }
    for (size_t i = 0; i < g_pal.rowRects.size(); ++i) {
        const D2D1_RECT_F& r = g_pal.rowRects[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            PaletteRun(g_pal.first + static_cast<int>(i));
            return true;
        }
    }
    const D2D1_RECT_F& p = g_pal.panel;
    if (!(x >= p.left && x < p.right && y >= p.top && y < p.bottom)) PaletteClose();
    return true;
}

// Command palette (Ctrl+K): drawn over the main window, with no separate window.
// Included from app.cpp after dragdrop.inl.

struct PalItem {
    std::wstring title, detail, shortcut;
    std::function<void()> run;
    Cmd cmd = Cmd::None;  // if it's a command with a configurable shortcut
    std::wstring path;    // Go to: the folder or file
    bool pinned = false;  // always visible, at the top (the typed path)
    int bonus = 0;        // Go to: recents and known places rank above index results
};

struct Palette {
    bool open = false;
    int mode = 0;        // 0 = commands (Ctrl+K), 1 = Go to (Ctrl+P), 2 = tags for the selection
    std::wstring text;
    std::wstring match;  // what's being searched for (in Go to with a typed path: whatever follows the last slash)
    std::vector<PalItem> all;
    std::vector<std::pair<int, int>> hits;  // (index into all, score), already sorted
    int sel = 0;
    int first = 0;  // first visible row (scroll offset)
    int capture = -1;       // index (into hits) of the command whose shortcut is being captured
    int hoverShortcut = -1; // visible row whose shortcut is under the mouse
    D2D1_RECT_F panel{};
    std::vector<D2D1_RECT_F> rowRects, shortcutRects;
} g_pal;

constexpr float kPalW = 600.f, kPalInputH = 40.f, kPalRow = 30.f;
constexpr int kPalMaxRows = 12;

// Readable text for a key: "Ctrl+Shift+N"
std::wstring KeyText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & ModCtrl) s += L"Ctrl+";
    if (mods & ModShift) s += L"Shift+";
    if (mods & ModAlt) s += L"Alt+";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return s + static_cast<wchar_t>(vk);
    if (vk >= VK_F1 && vk <= VK_F24) return s + L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk == VK_PRIOR) return s + L"PageUp";
    if (vk == VK_NEXT) return s + L"PageDown";
    for (const KeyName& k : kKeyNames)  // the first name listed for each key is the one displayed
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
void TagBuild();

void PaletteFilter() {
    g_pal.hits.clear();
    g_pal.match = g_pal.text;
    if (g_pal.mode == 1) GotoBuild();
    if (g_pal.mode == 2) TagBuild();
    std::wstring pat = Lower(g_pal.match);
    const NearPattern close(pat, g_cfg.typoMin);
    for (size_t i = 0; i < g_pal.all.size(); ++i) {
        const PalItem& it = g_pal.all[i];
        if (it.pinned) {
            g_pal.hits.push_back({static_cast<int>(i), 1 << 28});
            continue;
        }
        int s1 = 0, s2 = 0;
        bool m1 = FuzzyMatch(it.title.c_str(), it.title.size(), pat, s1, nullptr);
        bool m2 = !it.detail.empty() && FuzzyMatch(it.detail.c_str(), it.detail.size(), pat, s2, nullptr);
        const int real = std::max(m1 ? s1 : INT_MIN / 2, m2 ? s2 - 10 : INT_MIN / 2);
        if (real <= kNearFloor / 2) {  // nothing, or scattered letters: a title that is close says more
            if (const int percent = NearMatch(it.title.c_str(), it.title.size(), close)) {
                g_pal.hits.push_back({static_cast<int>(i), NearScore(percent, it.title.size())});
                continue;
            }
            if (!m1 && !m2) continue;
        }
        g_pal.hits.push_back({static_cast<int>(i), real + it.bonus + (g_pal.mode == 1 ? NameRank(it.title.c_str(), it.title.size(), pat) : 0)});
    }
    if (!pat.empty())
        std::stable_sort(g_pal.hits.begin(), g_pal.hits.end(), [](auto& a, auto& b) { return a.second > b.second; });
    g_pal.sel = 0;
    g_pal.first = 0;
}

// How close a row is to what was typed, or 0 if it's a real match.
int PalNear(const PalItem& it, const std::wstring& pat) {
    int sc;
    if (it.pinned || pat.empty() || (FuzzyMatch(it.title.c_str(), it.title.size(), pat, sc, nullptr) && sc > kNearFloor / 2) ||
        (!it.detail.empty() && FuzzyMatch(it.detail.c_str(), it.detail.size(), pat, sc, nullptr) && sc > kNearFloor / 2))
        return 0;
    return NearMatch(it.title.c_str(), it.title.size(), NearPattern(pat, g_cfg.typoMin));
}

// Keeps the selected row in view.
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
    ++g_ixGen;  // what is still being searched or checked is no longer needed
    g_ixAsked.clear();
    Invalidate();
}

// The results changed under the list: it stays where it was.
void PaletteRefresh() {
    if (!g_pal.open || g_pal.mode != 1) return;
    const int first = g_pal.first;
    const std::wstring keep = g_pal.sel > 0 && g_pal.sel < static_cast<int>(g_pal.hits.size()) ? g_pal.all[g_pal.hits[g_pal.sel].first].path : L"\x01";
    PaletteFilter();
    for (int i = 0; i < static_cast<int>(g_pal.hits.size()); ++i)
        if (g_pal.all[g_pal.hits[i].first].path == keep) g_pal.sel = i;
    g_pal.first = first;
    PaletteEnsureVisible();
    Invalidate();
}

void PaletteBuild() {
    g_pal.all.clear();
    auto add = [&](std::wstring title, std::wstring detail, std::wstring shortcut, std::function<void()> run) {
        g_pal.all.push_back({std::move(title), std::move(detail), std::move(shortcut), std::move(run)});
    };
    // commands with shortcuts
    for (const CmdDef& c : kCommands) {
        if (c.cmd == Cmd::Palette) continue;
        std::wstring title = c.help;
        title[0] = towupper(title[0]);
        Cmd cmd = c.cmd;
        add(title, c.name, ShortcutFor(c.cmd), [cmd] { RunCommand(cmd); });
        g_pal.all.back().cmd = cmd;
    }
    // the user's own commands ([commands] in config.txt)
    for (size_t i = 0; i < g_cfg.custom.size(); ++i) {
        Cmd cmd = CustomCmd(i);
        add(g_cfg.custom[i].name, L"your command", ShortcutFor(cmd), [cmd] { RunCommand(cmd); });
        g_pal.all.back().cmd = cmd;
    }
    // sort
    add(L"Sort by name", L"sort", L"", [] { SetSortExplicit(SortBy::Name, true); });
    add(L"Sort by date, newest first", L"sort", L"", [] { SetSortExplicit(SortBy::Date, false); });
    add(L"Sort by date, oldest first", L"sort", L"", [] { SetSortExplicit(SortBy::Date, true); });
    add(L"Sort by size, largest first", L"sort", L"", [] { SetSortExplicit(SortBy::Size, false); });
    add(L"Sort by size, smallest first", L"sort", L"", [] { SetSortExplicit(SortBy::Size, true); });
    add(L"Reverse the sort order", L"sort", L"", [] {
        SortBy by;
        bool asc;
        if (ViewPrimarySort(CurView(), by, asc)) SetSortExplicit(by, !asc);
        else if (const Step* s = FirstStep(CurView(), StepKind::Sort)) SetSortField(s->a, s->b == 0);
    });
    {
        const Step* f = FirstStep(CurView(), StepKind::Folders);
        int mode = !f ? 0 : f->a == FmFirst ? 1 : f->a == FmFirstByName ? 2 : 0;
        const wchar_t* ffNames[3] = {L"mixed with files", L"always first", L"first only when sorting by name"};
        add(std::wstring(L"Folders: ") + ffNames[mode] + L"  →  change", L"sort folders first", L"",
            [mode] { SetFoldersFirst((mode + 1) % 3); });
    }
    add(L"View for this folder: sort, group, filter…", L"view filter group rules", L"", [] { ViewEditorOpen(g_act); });
    // favorites and tabs
    for (size_t i = 0; i < g_favs.size(); ++i) {
        std::wstring p = g_favs[i].path;
        add(L"Go to: " + g_favs[i].name, p, i < 9 ? L"Ctrl+" + std::to_wstring(i + 1) : L"", [p] { Navigate(T(), p, true); });
    }
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        int idx = static_cast<int>(i);
        add(L"Tab: " + TabTitle(*g_tabs[i]), g_tabs[i]->path.empty() ? L"This PC" : g_tabs[i]->path, L"",
            [idx] { SelectTab(idx); });
    }
    add(L"Go to: This PC", L"drives", L"", [] { Navigate(T(), L"", true); });
    // app
    add(L"Reindex the drives now (fast, asks for admin permission)", L"index everything mft search drive", L"",
        [] { IdxReindexElevated(); });
    add(L"Edit config.txt", L"settings", L"", [] {
        std::wstring f = ConfigFile();
        if (!f.empty()) ShellExecuteW(nullptr, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    add(L"Open the config folder", L"settings", L"", [] { NewTab(ConfigDir()); });
    add(L"Context menu editor", L"menu", L"", [] { OpenMenuEditor(); });
    add(L"About Jolt", L"", L"", [] {
        MessageBoxW(g_hwnd, L"Jolt " JOLT_VERSION_W L" — file explorer.\n\nC++ · Win32 · Direct2D.", L"About Jolt",
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

// ---- tags: the palette lists the known tags (checked when the whole selection has one) and offers
// to create whatever is typed.
void TagPaletteOpen() {
    g_tagTargets = SelectedFullPaths(T());
    if (g_tagTargets.empty()) {
        S.message = L"Select something to tag";
        Invalidate();
        return;
    }
    PaletteOpen(2);
}

void TagBuild() {
    g_pal.all.clear();
    const std::wstring text = Trim(g_pal.text);
    bool exists = false;
    for (const std::wstring& name : AllTags()) {
        bool all = !g_tagTargets.empty();
        for (const std::wstring& p : g_tagTargets) all &= HasTag(p, name);
        PalItem it;
        it.title = (all ? L"\u2713  " : L"") + name;
        it.detail = all ? L"on the selection: Enter takes it off" : L"tag: Enter puts it on";
        it.run = [name] { TagToggle(g_tagTargets, name); };
        if (_wcsicmp(name.c_str(), text.c_str()) == 0) exists = true;
        g_pal.all.push_back(std::move(it));
    }
    if (!text.empty() && !exists) {
        PalItem it;
        it.title = L"New tag: " + text;
        it.detail = L"creates it and puts it on the selection";
        it.bonus = -50;  // the tags that match what is typed come first; this one only when none does
        it.run = [text] { TagToggle(g_tagTargets, text); };
        g_pal.all.push_back(std::move(it));
    }
}

// ---- Go to (Ctrl+P): recents, favorites, system places, drives, or a typed path
void GotoOpen(const std::wstring& p, bool newTab) {
    DWORD a = p.empty() || IsVirtual(p) ? FILE_ATTRIBUTE_DIRECTORY : GetFileAttributesW(ToExtendedPath(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        S.message = L"No longer exists: " + p;
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
    if (p.empty()) return L"This PC";
    if (IsVirtual(p)) return VirtualTitle(p);
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
        it.detail = path.empty() ? L"drives" : IsVirtual(path) ? L"" : path;
        it.path = path;
        it.pinned = pinned;
        it.bonus = bonus;
        it.run = [path] { GotoOpen(path, false); };
        g_pal.all.push_back(std::move(it));
    };
    std::wstring text = Trim(g_pal.text);
    // a typed path: the folder and its contents, filtered by whatever follows the last slash
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
            // the typed text as is, if it exists; the folder, only while nothing is typed after the slash
            if (wa != INVALID_FILE_ATTRIBUTES && Key(whole) != Key(dir)) add(L"Go to " + whole, whole, true);
            if (g_pal.match.empty()) add(L"Go to " + dir, dir, true);
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
    } known[] = {{L"Desktop", &FOLDERID_Desktop},          {L"Downloads", &FOLDERID_Downloads},
                 {L"Documents", &FOLDERID_Documents},      {L"Pictures", &FOLDERID_Pictures},
                 {L"Music", &FOLDERID_Music},              {L"Videos", &FOLDERID_Videos},
                 {L"User", &FOLDERID_Profile},             {L"OneDrive", &FOLDERID_SkyDrive},
                 {L"AppData", &FOLDERID_RoamingAppData},   {L"AppData Local", &FOLDERID_LocalAppData},
                 {L"Program Files", &FOLDERID_ProgramFiles},
                 {L"Program Files (x86)", &FOLDERID_ProgramFilesX86},
                 {L"Startup (programs at boot)", &FOLDERID_Startup},
                 {L"Start Menu", &FOLDERID_Programs},      {L"Send To", &FOLDERID_SendTo},
                 {L"Fonts", &FOLDERID_Fonts},              {L"Windows", &FOLDERID_Windows},
                 {L"System32", &FOLDERID_System}};
    for (const K& k : known) {
        std::wstring p = Known(*k.id);
        if (!p.empty()) add(k.name, Normalize(p));
    }
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp)) add(L"Temp", Normalize(tmp));
    add(L"This PC", L"");
    add(VirtualTitle(kBinPath), kBinPath);
    add(VirtualTitle(kNetPath), kNetPath);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
            add(DriveLabel(root[0]), root);
        }
    // the whole drive (global index): arrives a moment later and joins the list
    IdxRequest(text);
    // while the new search runs, the last one stays, filtered: what matches the longer text is in it, and nothing blinks
    if (text.size() >= 2 && !g_ixRes.query.empty() && text.rfind(g_ixRes.query, 0) == 0)
        for (const std::wstring& p : g_ixRes.hits) add(PlaceTitle(p), p, false, 0);
}

void PaletteRun(int hit) {
    if (hit < 0 || hit >= static_cast<int>(g_pal.hits.size())) return;
    auto run = g_pal.all[g_pal.hits[hit].first].run;
    PaletteClose();
    if (run) run();
}

// Command name (config) for a Cmd.
const wchar_t* CmdName(Cmd cmd) {
    if (IsCustom(cmd)) return CustomIndex(cmd) < static_cast<int>(g_cfg.custom.size()) ? g_cfg.custom[CustomIndex(cmd)].name.c_str() : L"";
    for (const CmdDef& c : kCommands)
        if (c.cmd == cmd) return c.name;
    return L"";
}

// Applies a captured key to the command: replaces its first shortcut, keeps the rest, and writes it to config.txt.
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
    // reflect it in memory right away (the file also triggers a reload)
    if (!remove)
        g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(),
                                        [&](const KeyBind& k) { return k.mods == mods && k.vk == vk; }),
                         g_cfg.keys.end());
    g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(), [&](const KeyBind& k) { return k.cmd == cmd; }),
                     g_cfg.keys.end());
    for (const KeyBind& k : mine) g_cfg.keys.push_back(k);
    // rebuild the palette, keeping text, selection and scroll
    std::wstring text = g_pal.text;
    int sel = g_pal.sel, first = g_pal.first;
    PaletteBuild();
    g_pal.text = text;
    PaletteFilter();
    g_pal.sel = std::min(sel, std::max(0, static_cast<int>(g_pal.hits.size()) - 1));
    g_pal.first = first;
    S.message = remove ? L"Shortcut removed" : L"Shortcut saved: " + KeyText(mods, vk);
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
    // cursor keys without modifiers are fixed
    if (mods == 0 && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END)) {
        S.message = L"That key is fixed; try Ctrl, Shift or Alt";
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
                GotoOpen(p, true);  // Ctrl+Enter: in a new tab
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

// Draws text with the matching positions highlighted.
void TextHighlighted(const std::wstring& s, const std::wstring& patLower, IDWriteTextFormat* fmt, float l, float t,
                     float r, float b, UINT32 color, UINT32 hiColor, float alpha) {
    std::vector<int> pos;
    int score = 0;
    bool found = !patLower.empty() && FuzzyMatch(s.c_str(), s.size(), patLower, score, &pos);
    if (!patLower.empty() && (!found || score <= kNearFloor / 2)) {  // scattered letters say less than the stretch that is close
        std::vector<int> closePos;
        if (NearPositions(s.c_str(), s.size(), patLower, g_cfg.typoMin, closePos)) {
            pos = std::move(closePos);
            found = true;
        }
    }
    if (!found || pos.empty()) {
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
    // soft shadow and panel
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(px - i, py - i + 2, px + pw + i, py + ph + i + 2), 10.f + i, 0x000000, 0.10f);
    Round(g_pal.panel, 10, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_pal.panel, 10, 10), g_brush.Get(), 1.f);
    // input
    Color(cDim);
    Glyph(0xE721, g_fmtIcon.Get(), px + 10, py, px + 34, py + kPalInputH);  // magnifier
    std::wstring shown = g_pal.text;
    float tw = MeasureW(shown, g_fmtName.Get());
    if (shown.empty()) {
        Color(cFaint);
        Text(g_pal.mode == 1 ? L"Go to…  recents, favorites, places, drives or a path" : g_pal.mode == 2 ? L"Tag…  pick one, or type a new name" : L"Type a command…",
             g_fmtName.Get(), px + 38, py, px + pw - 12, py + kPalInputH);
    } else {
        Color(cText);
        Text(shown, g_fmtName.Get(), px + 38, py, px + pw - 12, py + kPalInputH);
    }
    Fill(D2D1::RectF(px + 38 + tw + 1, py + 12, px + 38 + tw + 2, py + kPalInputH - 12), cText);  // caret
    if (g_pal.mode == 1 && (g_ixCount > 0 || g_ixBuilding)) {
        // drive index status
        std::wstring st = g_ixBuilding && g_ixCount == 0 ? L"indexing the drives…"
                          : Thousands(static_cast<int>(std::min<uint64_t>(g_ixCount, INT_MAX))) + (g_ixBuilding ? L" in the index · updating" : L" in the index");
        if (g_ixRes.query == Trim(g_pal.text) && g_ixRes.total > 0) {  // how much of it is in the list
            const int found = static_cast<int>(std::min<uint64_t>(g_ixRes.total, INT_MAX)), listed = static_cast<int>(g_ixRes.hits.size());
            st = listed < found ? Thousands(listed) + L" of " + Thousands(found) : Thousands(found) + (found == 1 ? L" result" : L" results");
        }
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
            // shortcut cell: click it to change the shortcut
            const bool capturing = hi == g_pal.capture;
            std::wstring label = capturing ? L"Press a key…  (Esc cancels, Del removes)"
                                 : it.shortcut.empty() ? L"no shortcut" : it.shortcut;
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
        if (const int percent = PalNear(it, pat)) {
            const float px0 = tx + MeasureW(it.title, g_fmtName.Get()) + 8;
            Color(cFaint);
            if (px0 + 30 < right - dw - 12) Text(std::to_wstring(percent) + L"%", g_fmtName.Get(), px0, y, right - dw - 12, y + kPalRow);
        }
        y += kPalRow;
    }
    if (g_pal.mode == 1 && g_pal.first + kPalMaxRows + 40 >= total) IdxMore();
    if (total > kPalMaxRows) {
        // thin scrollbar
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

// Returns true if the click was consumed by the palette.
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

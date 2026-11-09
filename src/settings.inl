// Settings (Ctrl+,): a panel over the main window, like the rename panel, that shows what the config files hold
// and changes it in place. The files stay the source of truth: every change is written to config.txt (keeping
// its comments), tags.txt or the favorites file, and applied at once, so editing them by hand keeps working.
// Included from app.cpp after viewedit.inl.

void ReloadConfig();  // app.cpp
void SetUiZoom(int px);
void SetRowHeight(float h);
void ViewEditorOpen(int pane);

enum class SrK { Head, Note, Toggle, Stepper, Choice, Text, Color, Key, Pair, Add, Button };

// One row of the panel. The getters read what is in effect now, so the panel always shows the files.
struct SetRow {
    SrK k = SrK::Note;
    std::wstring label, help;
    std::wstring id, id2;  // typed fields: the same while the panel is open
    std::function<bool()> getB;
    std::function<void(bool)> setB;
    std::function<int()> getI;
    std::function<void(int)> setI;
    std::function<std::wstring(int)> showI;
    int lo = 0, hi = 0, step = 1;
    std::vector<std::wstring> opts;
    std::function<std::wstring()> getS, getS2;
    std::function<void(const std::wstring&)> setS, setS2;
    std::function<UINT32()> swatch;  // Color, and Pair's second field
    UINT32 def = 0;                  // Color: the default, offered when it differs
    bool hasDef = false;
    float w1 = 0;  // Pair: width of the first field (0: it takes what is left)
    std::function<void()> act, del;  // Button / Add, and the × of list rows
    Cmd cmd = Cmd::None;             // Key
};

enum class SetK { Nav, Close, Toggle, Minus, Plus, Choice, Field, Key, Del, Act, Reset };
struct SetHit {
    D2D1_RECT_F r;
    SetK k;
    int row = -1, part = -1;
    std::wstring id;
};

const wchar_t* const kSetSections[] = {L"General", L"Appearance", L"Shortcuts", L"File colors", L"Commands", L"Tags", L"Favorites", L"More"};
constexpr int kSetCount = 8;
constexpr float kSetW = 800.f, kSetMaxH = 680.f, kSetNavW = 170.f, kSetHead = 56.f, kSetRowH = 40.f, kSetHelpH = 52.f;

struct SettingsData {
    bool open = false;
    int section = 0;
    float scroll = 0, contentH = 0;
    D2D1_RECT_F box{}, content{};
    std::vector<SetHit> hits;
    SetHit hoverHit{};  // what the mouse is over, from the last frame drawn
    bool hovering = false;
    std::wstring editId;  // field being typed in
    HWND edit = nullptr;
    Cmd capture = Cmd::None;  // command whose key is being captured
    bool captureOn = false;
    std::wstring search;      // Shortcuts: what is typed filters the list
    bool draftOn = false;     // a new row being filled in (File colors, Commands)
    std::wstring draftA, draftB;
} g_set;

void SettingsSaved(const std::wstring& what) {
    ReloadConfig();  // the file changed: everything that depends on it is applied now
    S.message = what;
    Invalidate();
}

void SetGeneral(const wchar_t* key, const std::wstring& value) {
    ConfigSetValue(L"general", key, value);
    SettingsSaved(std::wstring(L"Saved: ") + key + L" = " + value);
}

// "4F8CF7", "#4f8cf7" -> the color; false if it is not RRGGBB.
bool ParseHexColor(std::wstring s, UINT32& out) {
    s = Trim(s);
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6 || s.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) {
        S.message = L"Colors are RRGGBB in hexadecimal, like 4F8CF7";
        Invalidate();
        return false;
    }
    out = static_cast<UINT32>(wcstoul(s.c_str(), nullptr, 16));
    return true;
}

std::wstring Pct(int px) { return std::to_wstring((px * 100 + 6) / 12) + L"%"; }

// [file_colors] and [commands] are written whole, in the order shown.
void SaveFileColors(const std::vector<FileColorRule>& rules) {
    std::vector<std::pair<std::wstring, std::wstring>> e;
    for (const FileColorRule& r : rules) {
        std::wstring pats;
        for (const std::wstring& p : r.patterns) pats += (pats.empty() ? L"" : L" ") + p;
        e.push_back({pats, Hex(r.color)});
    }
    ConfigWriteSection(L"file_colors", e);
    SettingsSaved(L"Saved: file colors");
}
void SaveCommands(const std::vector<Config::Custom>& cmds) {
    std::vector<std::pair<std::wstring, std::wstring>> e;
    for (const Config::Custom& c : cmds) e.push_back({c.name, c.line});
    ConfigWriteSection(L"commands", e);
    SettingsSaved(L"Saved: commands");
}
std::vector<std::wstring> SplitSpaces(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (size_t p = 0; p < s.size();) {
        size_t sp = s.find(L' ', p);
        if (sp == std::wstring::npos) sp = s.size();
        if (sp > p) out.push_back(Lower(s.substr(p, sp - p)));
        p = sp + 1;
    }
    return out;
}

// ---- the rows of each section
std::vector<SetRow> SettingsRows(int section) {
    std::vector<SetRow> rows;
    auto head = [&](std::wstring t) {
        SetRow r;
        r.k = SrK::Head;
        r.label = std::move(t);
        rows.push_back(std::move(r));
    };
    auto note = [&](std::wstring t) {
        SetRow r;
        r.k = SrK::Note;
        r.label = std::move(t);
        rows.push_back(std::move(r));
    };
    auto toggle = [&](std::wstring label, std::wstring help, std::function<bool()> get, std::function<void(bool)> set) {
        SetRow r;
        r.k = SrK::Toggle;
        r.label = std::move(label);
        r.help = std::move(help);
        r.getB = std::move(get);
        r.setB = std::move(set);
        rows.push_back(std::move(r));
    };
    auto stepper = [&](std::wstring label, std::wstring help, int lo, int hi, int step, std::function<int()> get,
                       std::function<void(int)> set, std::function<std::wstring(int)> show = nullptr) {
        SetRow r;
        r.k = SrK::Stepper;
        r.label = std::move(label);
        r.help = std::move(help);
        r.lo = lo;
        r.hi = hi;
        r.step = step;
        r.getI = std::move(get);
        r.setI = std::move(set);
        r.showI = show ? std::move(show) : [](int v) { return std::to_wstring(v); };
        rows.push_back(std::move(r));
    };
    auto choice = [&](std::wstring label, std::wstring help, std::vector<std::wstring> opts, std::function<int()> get,
                      std::function<void(int)> set) {
        SetRow r;
        r.k = SrK::Choice;
        r.label = std::move(label);
        r.help = std::move(help);
        r.opts = std::move(opts);
        r.getI = std::move(get);
        r.setI = std::move(set);
        rows.push_back(std::move(r));
    };
    auto text = [&](std::wstring id, std::wstring label, std::wstring help, std::function<std::wstring()> get,
                    std::function<void(const std::wstring&)> set) -> SetRow& {
        SetRow r;
        r.k = SrK::Text;
        r.id = std::move(id);
        r.label = std::move(label);
        r.help = std::move(help);
        r.getS = std::move(get);
        r.setS = std::move(set);
        rows.push_back(std::move(r));
        return rows.back();
    };
    auto color = [&](std::wstring id, std::wstring label, std::wstring help, std::function<UINT32()> get,
                     std::function<void(UINT32)> set, bool hasDef, UINT32 def) {
        SetRow r;
        r.k = SrK::Color;
        r.id = std::move(id);
        r.label = std::move(label);
        r.help = std::move(help);
        r.swatch = get;
        r.getS = [get] { return Hex(get()); };
        r.setS = [set](const std::wstring& s) {
            UINT32 c;
            if (ParseHexColor(s, c)) set(c);
        };
        r.hasDef = hasDef;
        r.def = def;
        rows.push_back(std::move(r));
    };
    auto add = [&](std::wstring label, std::function<void()> act) {
        SetRow r;
        r.k = SrK::Add;
        r.label = std::move(label);
        r.act = std::move(act);
        rows.push_back(std::move(r));
    };
    auto button = [&](std::wstring label, std::wstring help, std::function<void()> act) {
        SetRow r;
        r.k = SrK::Button;
        r.label = std::move(label);
        r.help = std::move(help);
        r.act = std::move(act);
        rows.push_back(std::move(r));
    };
    auto yesNo = [](bool b) { return std::wstring(b ? L"yes" : L"no"); };

    switch (section) {
        case 0: {  // General
            toggle(L"Show hidden and system files", L"They appear dimmed. Ctrl+H switches it.", [] { return g_cfg.showHidden; },
                   [=](bool b) { SetGeneral(L"show_hidden", yesNo(b)); });
            choice(L"Selection checkboxes", L"Select with the mouse, without Ctrl. The one in the header: all.",
                   {L"Off", L"On hover", L"Always"}, [] { return g_cfg.checkboxes; },
                   [](int i) { SetGeneral(L"checkboxes", i == 0 ? L"no" : i == 1 ? L"hover" : L"always"); });
            choice(L"Mark what changed lately", L"A thin bar at the left of the files changed, fading with the hours. A number of hours can go in config.txt.",
                   {L"Off", L"Today", L"Last 24 hours"},
                   [] { return g_cfg.recentMark < 0 ? 0 : g_cfg.recentMark == 0 ? 1 : g_cfg.recentMark == 24 ? 2 : -1; },
                   [](int i) { SetGeneral(L"recent_mark", i == 0 ? L"no" : i == 1 ? L"today" : L"24"); });
            choice(L"Folders above files", L"Sorting by date or size mixes them unless this says always.",
                   {L"Mixed", L"Always", L"When sorting by name"}, [] { return g_foldersFirst.load(); },
                   [](int i) { SetGeneral(L"folders_first", i == 0 ? L"no" : i == 1 ? L"yes" : L"name"); });
            toggle(L"Close names in searches", L"Names close to what you type appear under the real matches, with how close they are.",
                   [] { return g_cfg.typoMin > 0; }, [](bool b) { SetGeneral(L"filter_typos", b ? L"75" : L"no"); });
            if (g_cfg.typoMin > 0)
                stepper(L"How close, at least", L"100: only swapped letters.", 50, 100, 5, [] { return g_cfg.typoMin; },
                        [](int v) { SetGeneral(L"filter_typos", std::to_wstring(v)); }, [](int v) { return std::to_wstring(v) + L"%"; });
            stepper(L"Folders in Recents", L"Sorted by use. 0 hides the section.", 0, kHotCountMax, 1, [] { return g_cfg.hotCount; },
                    [](int v) { SetGeneral(L"recents", std::to_wstring(v)); });
            toggle(L"Cloud status", L"A cloud next to what is online only, a check next to what is always on this PC. Jolt never downloads them to list or preview.",
                   [] { return g_cfg.cloudStatus; }, [=](bool b) { SetGeneral(L"cloud_status", yesNo(b)); });
            toggle(L"Wrap long lines in the text preview", L"", [] { return g_cfg.previewWrap; }, [=](bool b) { SetGeneral(L"preview_wrap", yesNo(b)); });
            toggle(L"Render Markdown in the preview", L"Off: its source, highlighted.", [] { return g_cfg.previewMarkdown; },
                   [=](bool b) { SetGeneral(L"preview_markdown", yesNo(b)); });
            toggle(L"Open drives as they are connected", L"USB sticks and drives open in a tab.", [] { return g_cfg.openDrives; },
                   [=](bool b) { SetGeneral(L"open_drives", yesNo(b)); });
            head(L"Columns");
            {
                auto has = [](int k) { return ColShown(k); };
                auto set = [](int k, bool on) {
                    const std::wstring s = ColShow(k, on);
                    SettingsSaved(L"Saved: columns = " + (s.empty() ? std::wstring(L"name only") : s));
                };
                const wchar_t* help[ColKinds] = {L"", L"Width × height of images and videos.", L"How long songs and videos last.",
                                                 L"R read-only, H hidden, S system, A archive, C compressed, E encrypted, L link.",
                                                 L"How many items each folder has.", L""};
                for (int k = 0; k < ColKinds; ++k)
                    toggle(kColDefs[k].label, help[k], [=] { return has(k); }, [=](bool b) { set(k, b); });
                toggle(L"Size bars", L"A bar behind each size, next to the largest in the folder. Folders count once their size is calculated (Alt+R).",
                       [] { return g_cfg.sizeBars; }, [](bool b) {
                           SetSizeBars(b);
                           SettingsSaved(std::wstring(L"Saved: size_bars = ") + (b ? L"yes" : L"no"));
                       });
            }
            head(L"Always ready");
            toggle(L"Keep running when the window closes", L"Closing hides it, and it comes back at once. ☰ → Quit really closes.",
                   [] { return g_cfg.background; }, [=](bool b) { SetGeneral(L"run_in_background", yesNo(b)); });
            toggle(L"Icon in the system tray", L"", [] { return g_cfg.tray; }, [=](bool b) { SetGeneral(L"tray_icon", yesNo(b)); });
            text(L"global_hotkey", L"Shortcut from any app", L"Opens Jolt, and hides it if it is in front. Empty: none. Like Win+J or Ctrl+Alt+J.",
                 [] { return g_cfg.globalKey; }, [](const std::wstring& s) { SetGeneral(L"global_hotkey", Trim(s)); });
            break;
        }
        case 1: {  // Appearance
            text(L"font", L"Font", L"The whole interface. Any installed font, like Segoe UI or Inter.", [] { return g_cfg.font; },
                 [](const std::wstring& s) {
                     if (!Trim(s).empty()) SetGeneral(L"font", Trim(s));
                 });
            stepper(L"Font size", L"In pixels at 100%.", 8, 32, 1, [] { return static_cast<int>(std::lround(g_cfg.fontSize)); },
                    [](int v) { SetGeneral(L"font_size", std::to_wstring(v)); });
            stepper(L"Interface size", L"Ctrl + and Ctrl - change it too.", kUiPxMin, kUiPxMax, 1, [] { return g_uiPx; },
                    [](int v) { SetUiZoom(v); }, Pct);
            stepper(L"Row height", L"", 18, 60, 1, [] { return static_cast<int>(kRow); }, [](int v) { SetRowHeight(static_cast<float>(v)); },
                    [](int v) { return std::to_wstring(v) + L" px"; });
            stepper(L"Sidebar width", L"", 120, 500, 8, [] { return static_cast<int>(kSideW); },
                    [](int v) { SetGeneral(L"sidebar_width", std::to_wstring(v)); }, [](int v) { return std::to_wstring(v) + L" px"; });
            stepper(L"Preview width", L"Dragging its edge changes it too.", static_cast<int>(kPreviewMinW), 1600, 20,
                    [] { return static_cast<int>(std::lround(g_previewW)); }, [](int v) { SetGeneral(L"preview_width", std::to_wstring(v)); },
                    [](int v) { return std::to_wstring(v) + L" px"; });
            head(L"Colors");
            for (const ColorDef& c : kColors) {
                std::wstring help = c.help;
                help[0] = towupper(help[0]);
                UINT32* var = c.var;
                const std::wstring name = c.name;
                std::wstring label = name;
                for (wchar_t& ch : label)
                    if (ch == L'_') ch = L' ';
                label[0] = towupper(label[0]);
                color(L"color." + name, label, help, [var] { return *var; },
                      [name](UINT32 v) {
                          ConfigSetValue(L"colors", name, Hex(v));
                          SettingsSaved(L"Saved: " + name + L" = " + Hex(v));
                      },
                      true, c.def);
            }
            break;
        }
        case 2: {  // Shortcuts
            note(g_set.search.empty() ? L"Type to search. Click a shortcut and press the new key: Del removes it, Esc cancels."
                                      : L"Searching: " + g_set.search + L"   ·   Backspace erases");
            const std::wstring q = Lower(g_set.search);
            auto keysOf = [](Cmd cmd) {
                std::wstring s;
                for (const KeyBind& k : g_cfg.keys)
                    if (k.cmd == cmd) s += (s.empty() ? L"" : L", ") + KeyText(k.mods, k.vk);
                return s;
            };
            auto addKey = [&](std::wstring label, std::wstring name, Cmd cmd) {
                const std::wstring keys = keysOf(cmd);
                if (!q.empty() && Lower(label).find(q) == std::wstring::npos && Lower(name).find(q) == std::wstring::npos &&
                    Lower(keys).find(q) == std::wstring::npos)
                    return;
                SetRow r;
                r.k = SrK::Key;
                r.label = std::move(label);
                r.help = std::move(name);
                r.cmd = cmd;
                r.getS = [keys] { return keys; };
                rows.push_back(std::move(r));
            };
            for (const CmdDef& c : kCommands) {
                std::wstring t = c.help;
                t[0] = towupper(t[0]);
                addKey(t, c.name, c.cmd);
            }
            for (size_t i = 0; i < g_cfg.custom.size(); ++i) addKey(g_cfg.custom[i].name, L"your command", CustomCmd(i));
            if (rows.size() == 1) note(L"No command matches");
            break;
        }
        case 3: {  // File colors
            note(L"Name colors by extension or pattern: .jpg .png, *backup*, folders. The first rule that fits wins.");
            const std::vector<FileColorRule> rules = g_cfg.fileColors;
            for (size_t i = 0; i < rules.size(); ++i) {
                SetRow r;
                r.k = SrK::Pair;
                r.id = L"fc." + std::to_wstring(i) + L".a";
                r.id2 = L"fc." + std::to_wstring(i) + L".b";
                std::wstring pats;
                for (const std::wstring& p : rules[i].patterns) pats += (pats.empty() ? L"" : L" ") + p;
                r.getS = [pats] { return pats; };
                r.getS2 = [c = rules[i].color] { return Hex(c); };
                r.swatch = [c = rules[i].color] { return c; };
                r.setS = [rules, i](const std::wstring& s) {
                    std::vector<FileColorRule> v = rules;
                    v[i].patterns = SplitSpaces(s);
                    if (v[i].patterns.empty()) v.erase(v.begin() + i);
                    SaveFileColors(v);
                };
                r.setS2 = [rules, i](const std::wstring& s) {
                    UINT32 c;
                    if (!ParseHexColor(s, c)) return;
                    std::vector<FileColorRule> v = rules;
                    v[i].color = c;
                    SaveFileColors(v);
                };
                r.del = [rules, i] {
                    std::vector<FileColorRule> v = rules;
                    v.erase(v.begin() + i);
                    SaveFileColors(v);
                };
                rows.push_back(std::move(r));
            }
            if (g_set.draftOn) {
                SetRow r;
                r.k = SrK::Pair;
                r.id = L"draft.a";
                r.id2 = L"draft.b";
                r.getS = [] { return g_set.draftA; };
                r.getS2 = [] { return g_set.draftB; };
                r.swatch = [] {
                    UINT32 c = 0x7FB3FF;
                    std::wstring h = Trim(g_set.draftB);
                    if (!h.empty() && h[0] == L'#') h.erase(0, 1);
                    if (h.size() == 6) c = static_cast<UINT32>(wcstoul(h.c_str(), nullptr, 16));
                    return c;
                };
                auto finish = [rules] {
                    UINT32 c = 0x7FB3FF;
                    if (!Trim(g_set.draftB).empty() && !ParseHexColor(g_set.draftB, c)) return;
                    if (SplitSpaces(g_set.draftA).empty()) return;
                    std::vector<FileColorRule> v = rules;
                    v.push_back({SplitSpaces(g_set.draftA), c});
                    g_set.draftOn = false;
                    SaveFileColors(v);
                };
                r.setS = [finish](const std::wstring& s) {
                    g_set.draftA = s;
                    finish();
                };
                r.setS2 = [finish](const std::wstring& s) {
                    g_set.draftB = s;
                    finish();
                };
                r.del = [] { g_set.draftOn = false; };
                rows.push_back(std::move(r));
            } else {
                add(L"Add a rule", [] {
                    g_set.draftOn = true;
                    g_set.draftA = L"";
                    g_set.draftB = L"7FB3FF";
                });
            }
            break;
        }
        case 4: {  // Commands
            note(L"In Ctrl+K and the right-click menu. {file} the item, {files} the selection, {dir} the folder, {name} its name.");
            const std::vector<Config::Custom> cmds = g_cfg.custom;
            for (size_t i = 0; i < cmds.size(); ++i) {
                SetRow r;
                r.k = SrK::Pair;
                r.w1 = 210;
                r.id = L"cmd." + std::to_wstring(i) + L".a";
                r.id2 = L"cmd." + std::to_wstring(i) + L".b";
                r.getS = [n = cmds[i].name] { return n; };
                r.getS2 = [l = cmds[i].line] { return l; };
                r.setS = [cmds, i](const std::wstring& s) {
                    std::vector<Config::Custom> v = cmds;
                    if (Trim(s).empty()) v.erase(v.begin() + i);
                    else v[i].name = Trim(s);
                    SaveCommands(v);
                };
                r.setS2 = [cmds, i](const std::wstring& s) {
                    std::vector<Config::Custom> v = cmds;
                    if (Trim(s).empty()) v.erase(v.begin() + i);
                    else v[i].line = Trim(s);
                    SaveCommands(v);
                };
                r.del = [cmds, i] {
                    std::vector<Config::Custom> v = cmds;
                    v.erase(v.begin() + i);
                    SaveCommands(v);
                };
                rows.push_back(std::move(r));
            }
            if (g_set.draftOn) {
                SetRow r;
                r.k = SrK::Pair;
                r.w1 = 210;
                r.id = L"draft.a";
                r.id2 = L"draft.b";
                r.getS = [] { return g_set.draftA; };
                r.getS2 = [] { return g_set.draftB; };
                auto finish = [cmds] {
                    if (Trim(g_set.draftA).empty() || Trim(g_set.draftB).empty()) return;
                    std::vector<Config::Custom> v = cmds;
                    v.push_back({Trim(g_set.draftA), Trim(g_set.draftB)});
                    g_set.draftOn = false;
                    SaveCommands(v);
                };
                r.setS = [finish](const std::wstring& s) {
                    g_set.draftA = s;
                    finish();
                };
                r.setS2 = [finish](const std::wstring& s) {
                    g_set.draftB = s;
                    finish();
                };
                r.del = [] { g_set.draftOn = false; };
                rows.push_back(std::move(r));
            } else {
                add(L"Add a command", [] {
                    g_set.draftOn = true;
                    g_set.draftA = g_set.draftB = L"";
                });
            }
            note(L"A command gets a shortcut in Shortcuts, under its name.");
            break;
        }
        case 5: {  // Tags
            if (g_tagDefs.empty()) note(L"No tags yet. Right click → Tags, or Ctrl+K → \"tag\", puts one on the selection.");
            else note(L"Right click → Tags puts them on files and folders; tag:name in the filter finds them.");
            for (size_t i = 0; i < g_tagDefs.size(); ++i) {
                const std::wstring name = g_tagDefs[i].name;
                color(L"tag." + name, name, L"", [name] { return TagColor(name); },
                      [name](UINT32 v) {
                          for (TagDef& d : g_tagDefs)
                              if (d.name == name) d.color = v;
                          SaveTags();
                          S.message = L"Saved: " + name + L" = " + Hex(v);
                          Invalidate();
                      },
                      true, TagAutoColor(name));
            }
            break;
        }
        case 6: {  // Favorites
            if (g_favs.empty()) note(L"No favorites yet. Ctrl+D adds the current folder.");
            else note(L"In the sidebar, in this order; drag them there to reorder. Ctrl+1..9 opens the first nine.");
            for (size_t i = 0; i < g_favs.size(); ++i) {
                const std::wstring path = g_favs[i].path;
                SetRow& r = text(L"fav." + std::to_wstring(i), path.empty() ? L"This PC" : path, L"", [i] {
                    return i < g_favs.size() ? g_favs[i].name : L"";
                }, [i, path](const std::wstring& s) {
                    if (i >= g_favs.size() || g_favs[i].path != path || Trim(s).empty()) return;
                    g_favs[i].name = Trim(s);
                    SaveFavs();
                    S.message = L"Saved: " + Trim(s);
                    Invalidate();
                });
                r.del = [i, path] {
                    if (i >= g_favs.size() || g_favs[i].path != path) return;
                    g_favs.erase(g_favs.begin() + i);
                    SaveFavs();
                    S.message = L"Removed from favorites: " + path;
                    Invalidate();
                };
            }
            break;
        }
        case 7: {  // More
            button(L"Context menu…", L"Hide and reorder the entries of the right-click menu.", [] { OpenMenuEditor(); });
            button(L"View of this folder…", L"Sort, group and filter rules, for this folder or for all of them.", [] { ViewEditorOpen(g_act); });
            button(L"Edit config.txt", L"Everything here, as text, with a comment on each option.", [] {
                std::wstring f = ConfigFile();
                if (!f.empty()) ShellExecuteW(nullptr, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            });
            button(L"Templates folder", L"Files and folders put there become entries of New, Ctrl+K lists them as \"New: name\".",
                   [] { OpenTemplatesFolder(); });
            button(L"Open the config folder", L"config.txt, views.txt, menu.txt, tags.txt and the favorites, in a new tab.",
                   [] { NewTab(ConfigDir()); });
            break;
        }
    }
    return rows;
}

float SetRowH(const SetRow& r) {
    switch (r.k) {
        case SrK::Head: return 38;
        case SrK::Note: return 34;
        case SrK::Add: return 36;
        case SrK::Pair: case SrK::Key: return 36;
        default: return r.help.empty() ? kSetRowH : kSetHelpH;
    }
}

// ---- open, close, typing
void SettingsCommitText();

void SettingsClose() {
    if (!g_set.open) return;
    SettingsCommitText();
    if (g_set.edit) ShowWindow(g_set.edit, SW_HIDE);
    g_set.open = false;
    g_set.captureOn = false;
    g_set.draftOn = false;
    g_set.hits.clear();
    Invalidate();
}

void SettingsOpen(int section) {
    PopupClose();
    PaletteClose();
    ViewEditorClose();
    MenuEditorClose();
    CancelRename();
    g_set.open = true;
    if (section >= 0) g_set.section = section;
    g_set.scroll = 0;
    g_set.hovering = false;
    g_set.captureOn = false;
    g_set.draftOn = false;
    g_set.search.clear();
    Invalidate();
}

void SettingsSection(int i) {
    SettingsCommitText();
    g_set.section = std::clamp(i, 0, kSetCount - 1);
    g_set.scroll = 0;
    g_set.hovering = false;
    g_set.captureOn = false;
    g_set.draftOn = false;
    g_set.search.clear();
    Invalidate();
}

// Tests send their keys straight to the box (TestKeyTarget): taking the focus could take it from the user.
void SettingsFocus(HWND h) {
    if (!g_test) SetFocus(h);
}

LRESULT CALLBACK SetEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_TAB)) {
        SettingsCommitText();
        if (g_set.editId.empty()) SettingsFocus(g_hwnd);  // unless the next field of a new row is being typed in now
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        if (g_set.editId.rfind(L"draft.", 0) == 0 && Trim(g_set.draftA).empty()) g_set.draftOn = false;  // a new row left empty goes
        g_set.editId.clear();
        ShowWindow(h, SW_HIDE);
        SettingsFocus(g_hwnd);
        Invalidate();
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;
    if (m == WM_KILLFOCUS && !g_set.editId.empty()) SettingsCommitText();
    return DefSubclassProc(h, m, w, l);
}

void SettingsBeginText(const std::wstring& id, const std::wstring& value) {
    if (!g_set.edit) {
        g_set.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(g_set.edit, SetEditProc, 4, 0);
    }
    SendMessageW(g_set.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    SendMessageW(g_set.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetWindowTextW(g_set.edit, value.c_str());
    g_set.editId = id;
    Invalidate();
    UpdateWindow(g_hwnd);  // places the box over its field
    SettingsFocus(g_set.edit);
    SendMessageW(g_set.edit, EM_SETSEL, 0, -1);
}

void SettingsCommitText() {
    if (g_set.editId.empty() || !g_set.edit) return;
    const std::wstring id = g_set.editId;
    g_set.editId.clear();
    ShowWindow(g_set.edit, SW_HIDE);
    const int n = GetWindowTextLengthW(g_set.edit);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(g_set.edit, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    for (const SetRow& r : SettingsRows(g_set.section)) {
        if (r.id == id && r.setS) {
            if (r.getS && r.getS() == s && id.rfind(L"draft.", 0) != 0) break;  // unchanged
            r.setS(s);
            break;
        }
        if (r.id2 == id && r.setS2) {
            if (r.getS2 && r.getS2() == s && id.rfind(L"draft.", 0) != 0) break;
            r.setS2(s);
            break;
        }
    }
    // a new command: after its name comes its command line
    if (id == L"draft.a" && g_set.draftOn && !Trim(g_set.draftA).empty() && Trim(g_set.draftB).empty()) {
        SettingsBeginText(L"draft.b", L"");
        return;
    }
    Invalidate();
}

// ---- drawing
void SetSwitch(const D2D1_RECT_F& r, bool on) {
    Round(r, (r.bottom - r.top) / 2, on ? cAccent : 0x45454F);
    const float d = r.bottom - r.top - 6, x = on ? r.right - 3 - d : r.left + 3;
    Round(D2D1::RectF(x, r.top + 3, x + d, r.top + 3 + d), d / 2, 0xFFFFFF, on ? 1.f : 0.75f);
}

void SetField(const D2D1_RECT_F& r, const std::wstring& id, const std::wstring& value, int row, bool hover, const wchar_t* placeholder = L"") {
    Round(r, 5, cBg);
    Color(hover ? cDim : cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5, 5), g_brush.Get(), 1.f);
    if (g_set.editId != id) {
        Color(value.empty() ? cFaint : cText);
        Text(value.empty() ? std::wstring(placeholder) : value, g_fmtName.Get(), r.left + 9, r.top, r.right - 6, r.bottom);
    }
    g_set.hits.push_back({r, SetK::Field, row, -1, id});
}

void DrawSettings(float W, float H) {
    if (!g_set.open) return;
    g_set.hits.clear();
    const float pw = std::min(kSetW, W - 40), ph = std::min(kSetMaxH, H - kTabH - 24);
    const float x0 = std::floor((W - pw) / 2), y0 = std::floor(std::max(kTabH + 8, (H - ph) / 2));
    const float R = x0 + pw, B = y0 + ph;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_set.box = D2D1::RectF(x0, y0, R, B);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x0 - i, y0 - i + 2, R + i, B + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_set.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_set.box, 12, 12), g_brush.Get(), 1.f);
    auto isHover = [&](SetK k, int row, int part) {
        return g_set.hovering && g_set.hoverHit.k == k && g_set.hoverHit.row == row && g_set.hoverHit.part == part;
    };

    // title and close
    Color(cAccent);
    Glyph(0xE713, g_fmtIcon.Get(), x0 + 18, y0 + 14, x0 + 42, y0 + 42);  // gear
    Color(cText);
    Text(L"Settings", g_fmtNameBold.Get(), x0 + 48, y0 + 8, x0 + kSetNavW + 200, y0 + 30);
    Color(cDim);
    Text(L"Saved in the config files as you change it", g_fmtStatus.Get(), x0 + 48, y0 + 27, R - 60, y0 + 48);
    const D2D1_RECT_F cb = D2D1::RectF(R - 42, y0 + 14, R - 12, y0 + 42);
    if (isHover(SetK::Close, -1, -1)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(SetK::Close, -1, -1) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_set.hits.push_back({cb, SetK::Close});
    HLine(x0, R, y0 + kSetHead);

    // sections
    float ny = y0 + kSetHead + 10;
    for (int i = 0; i < kSetCount; ++i) {
        D2D1_RECT_F r = D2D1::RectF(x0 + 10, ny, x0 + kSetNavW - 10, ny + 32);
        if (i == g_set.section) Round(r, 6, cAccent, 0.22f);
        else if (isHover(SetK::Nav, -1, i)) Round(r, 6, 0xFFFFFF, 0.06f);
        Color(i == g_set.section ? cText : cDim);
        Text(kSetSections[i], g_fmtName.Get(), r.left + 12, r.top, r.right - 6, r.bottom);
        g_set.hits.push_back({r, SetK::Nav, -1, i});
        ny += 34;
    }
    Fill(D2D1::RectF(x0 + kSetNavW, y0 + kSetHead, x0 + kSetNavW + 1, B - 12), cBorder);

    // the section's rows
    const float cl = x0 + kSetNavW + 20, cr = R - 22, ct = y0 + kSetHead + 1, cbm = B - 1;
    g_set.content = D2D1::RectF(x0 + kSetNavW + 1, ct, R, cbm);
    const std::vector<SetRow> rows = SettingsRows(g_set.section);
    float total = 12;
    for (const SetRow& r : rows) total += SetRowH(r);
    g_set.contentH = total + 12;
    g_set.scroll = std::clamp(g_set.scroll, 0.f, std::max(0.f, g_set.contentH - (cbm - ct)));
    g_rt->PushAxisAlignedClip(g_set.content, D2D1_ANTIALIAS_MODE_ALIASED);
    const size_t hitsBefore = g_set.hits.size();
    float y = ct + 12 - g_set.scroll;
    D2D1_RECT_F editRect{};
    bool editShown = false;
    for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
        const SetRow& r = rows[ri];
        const float h = SetRowH(r);
        if (y > cbm) break;
        if (y + h < ct) {
            y += h;
            continue;
        }
        const float mid = y + (r.k == SrK::Toggle || r.k == SrK::Stepper || r.k == SrK::Choice || r.k == SrK::Text || r.k == SrK::Color ?
                               (r.help.empty() ? h / 2 : 20) : h / 2);
        auto label = [&](float right) {
            Color(cText);
            if (r.help.empty()) {
                Text(r.label, g_fmtName.Get(), cl, y, right, y + h);
            } else {
                Text(r.label, g_fmtName.Get(), cl, y + 8, right, y + 30);
                Color(cDim);
                Text(r.help, g_fmtStatus.Get(), cl, y + 27, right, y + 47);
            }
        };
        switch (r.k) {
            case SrK::Head:
                Color(cFaint);
                Text(r.label, g_fmtStatus.Get(), cl, y + 12, cr, y + h);
                HLine(cl + MeasureW(r.label, g_fmtStatus.Get()) + 10, cr, y + 12 + (h - 12) / 2);
                break;
            case SrK::Note:
                Color(cDim);
                Text(r.label, g_fmtStatus.Get(), cl, y, cr, y + h);
                break;
            case SrK::Toggle: {
                const D2D1_RECT_F sw = D2D1::RectF(cr - 38, mid - 10, cr, mid + 10);
                label(cr - 60);
                SetSwitch(sw, r.getB());
                g_set.hits.push_back({D2D1::RectF(cl, y, cr, y + h), SetK::Toggle, ri});
                break;
            }
            case SrK::Stepper: {
                const int v = r.getI();
                const D2D1_RECT_F plus = D2D1::RectF(cr - 28, mid - 13, cr, mid + 13), minus = D2D1::RectF(cr - 128, mid - 13, cr - 100, mid + 13);
                label(cr - 150);
                for (int pi = 0; pi < 2; ++pi) {
                    const D2D1_RECT_F& b = pi ? plus : minus;
                    const bool can = pi ? v < r.hi : v > r.lo;
                    Round(b, 6, isHover(pi ? SetK::Plus : SetK::Minus, ri, -1) && can ? 0x3A3A44 : cBg);
                    Color(can ? cText : cFaint);
                    Text(pi ? L"+" : L"−", g_fmtCenter.Get(), b.left, b.top, b.right, b.bottom);
                    g_set.hits.push_back({b, pi ? SetK::Plus : SetK::Minus, ri});
                }
                Color(cText);
                Text(r.showI(v), g_fmtCenter.Get(), minus.right, mid - 13, plus.left, mid + 13);
                break;
            }
            case SrK::Choice: {
                const int cur = r.getI();
                float px = cr;
                std::vector<D2D1_RECT_F> pills(r.opts.size());
                for (int oi = static_cast<int>(r.opts.size()) - 1; oi >= 0; --oi) {
                    const float w = MeasureW(r.opts[oi], g_fmtName.Get()) + 22;
                    pills[oi] = D2D1::RectF(px - w, mid - 13, px, mid + 13);
                    px -= w + 4;
                }
                label(px - 12);
                for (size_t oi = 0; oi < r.opts.size(); ++oi) {
                    const bool on = static_cast<int>(oi) == cur;
                    Round(pills[oi], 6, on ? cAccent : cBg, on ? 0.30f : 1.f);
                    if (!on && isHover(SetK::Choice, ri, static_cast<int>(oi))) Round(pills[oi], 6, 0xFFFFFF, 0.06f);
                    Color(on ? cText : cDim);
                    Text(r.opts[oi], g_fmtCenter.Get(), pills[oi].left, pills[oi].top, pills[oi].right, pills[oi].bottom);
                    g_set.hits.push_back({pills[oi], SetK::Choice, ri, static_cast<int>(oi)});
                }
                break;
            }
            case SrK::Text: {
                const float fw = std::min(280.f, (cr - cl) * 0.5f);
                const float right = r.del ? cr - 34 : cr;
                const D2D1_RECT_F f = D2D1::RectF(right - fw, mid - 14, right, mid + 14);
                label(f.left - 14);
                SetField(f, r.id, r.getS(), ri, isHover(SetK::Field, ri, -1));
                if (g_set.editId == r.id) editRect = f, editShown = true;
                if (r.del) {
                    const D2D1_RECT_F d = D2D1::RectF(cr - 28, mid - 13, cr, mid + 13);
                    if (isHover(SetK::Del, ri, -1)) Round(d, 6, 0xFFFFFF, 0.08f);
                    Color(isHover(SetK::Del, ri, -1) ? cError : cFaint);
                    Glyph(0xE711, g_fmtIconSm.Get(), d.left, d.top, d.right, d.bottom);
                    g_set.hits.push_back({d, SetK::Del, ri});
                }
                break;
            }
            case SrK::Color: {
                const UINT32 c = r.swatch();
                const D2D1_RECT_F f = D2D1::RectF(cr - 92, mid - 14, cr, mid + 14);
                const D2D1_RECT_F sw = D2D1::RectF(f.left - 34, mid - 11, f.left - 12, mid + 11);
                const bool showDef = r.hasDef && c != r.def;
                const D2D1_RECT_F rd = D2D1::RectF(sw.left - 34, mid - 13, sw.left - 6, mid + 13);
                label(rd.left - 8);
                Round(sw, 5, c);
                Color(cBorder);
                g_rt->DrawRoundedRectangle(D2D1::RoundedRect(sw, 5, 5), g_brush.Get(), 1.f);
                SetField(f, r.id, r.getS(), ri, isHover(SetK::Field, ri, -1));
                if (g_set.editId == r.id) editRect = f, editShown = true;
                if (showDef) {
                    if (isHover(SetK::Reset, ri, -1)) Round(rd, 6, 0xFFFFFF, 0.08f);
                    Color(isHover(SetK::Reset, ri, -1) ? cText : cFaint);
                    Glyph(0xE7A7, g_fmtIconSm.Get(), rd.left, rd.top, rd.right, rd.bottom);  // undo: back to the default
                    g_set.hits.push_back({rd, SetK::Reset, ri});
                }
                break;
            }
            case SrK::Key: {
                const bool capturing = g_set.captureOn && g_set.capture == r.cmd;
                const std::wstring keys = r.getS();
                const std::wstring shown = capturing ? L"Press a key…  (Esc cancels, Del removes)" : keys.empty() ? L"no shortcut" : keys;
                const float w = std::max(110.f, MeasureW(shown, g_fmtRight.Get()) + 22);
                const D2D1_RECT_F cell = D2D1::RectF(cr - w, mid - 13, cr, mid + 13);
                Color(cText);
                Text(r.label, g_fmtName.Get(), cl, y, cell.left - 120, y + h);
                Color(cFaint);
                Text(r.help, g_fmtStatus.Get(), cell.left - 116, y, cell.left - 10, y + h);
                if (capturing) {
                    Round(cell, 5, cAccent, 0.25f);
                    Color(cAccent);
                    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(cell, 5, 5), g_brush.Get(), 1.f);
                } else {
                    Round(cell, 5, isHover(SetK::Key, ri, -1) ? 0x3A3A44 : cBg);
                }
                Color(capturing ? cText : keys.empty() ? cFaint : cDim);
                Text(shown, g_fmtCenter.Get(), cell.left, cell.top, cell.right, cell.bottom);
                g_set.hits.push_back({cell, SetK::Key, ri});
                break;
            }
            case SrK::Pair: {
                const float right = cr - 34;
                const bool sw = static_cast<bool>(r.swatch);
                const float w2 = sw ? 92.f : 0.f;
                D2D1_RECT_F f1, f2;
                if (sw) {  // patterns, swatch, color
                    f2 = D2D1::RectF(right - w2, mid - 14, right, mid + 14);
                    f1 = D2D1::RectF(cl, mid - 14, f2.left - 46, mid + 14);
                    const D2D1_RECT_F s = D2D1::RectF(f2.left - 34, mid - 11, f2.left - 12, mid + 11);
                    Round(s, 5, r.swatch());
                    Color(cBorder);
                    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(s, 5, 5), g_brush.Get(), 1.f);
                } else {  // name, command line
                    f1 = D2D1::RectF(cl, mid - 14, cl + r.w1, mid + 14);
                    f2 = D2D1::RectF(f1.right + 10, mid - 14, right, mid + 14);
                }
                SetField(f1, r.id, r.getS(), ri, isHover(SetK::Field, ri, -1), sw ? L".jpg .png  *backup*  folders" : L"Name");
                SetField(f2, r.id2, r.getS2(), ri, isHover(SetK::Field, ri, -1), sw ? L"RRGGBB" : L"program \"{file}\"");
                if (g_set.editId == r.id) editRect = f1, editShown = true;
                if (g_set.editId == r.id2) editRect = f2, editShown = true;
                const D2D1_RECT_F d = D2D1::RectF(cr - 28, mid - 13, cr, mid + 13);
                if (isHover(SetK::Del, ri, -1)) Round(d, 6, 0xFFFFFF, 0.08f);
                Color(isHover(SetK::Del, ri, -1) ? cError : cFaint);
                Glyph(0xE711, g_fmtIconSm.Get(), d.left, d.top, d.right, d.bottom);
                g_set.hits.push_back({d, SetK::Del, ri});
                break;
            }
            case SrK::Add: {
                const float w = MeasureW(r.label, g_fmtName.Get()) + 44;
                const D2D1_RECT_F b = D2D1::RectF(cl, mid - 14, cl + w, mid + 14);
                Round(b, 6, isHover(SetK::Act, ri, -1) ? 0x3A3A44 : cBg);
                Color(cAccent);
                Glyph(0xE710, g_fmtIconSm.Get(), b.left + 6, b.top, b.left + 28, b.bottom);  // +
                Color(cText);
                Text(r.label, g_fmtName.Get(), b.left + 30, b.top, b.right, b.bottom);
                g_set.hits.push_back({b, SetK::Act, ri});
                break;
            }
            case SrK::Button: {
                const D2D1_RECT_F b = D2D1::RectF(cl - 8, y + 2, cr, y + h - 2);
                if (isHover(SetK::Act, ri, -1)) Round(b, 8, 0xFFFFFF, 0.05f);
                label(cr - 40);
                Color(cDim);
                Glyph(0xE76C, g_fmtIconSm.Get(), cr - 30, y, cr - 6, y + h);  // chevron
                g_set.hits.push_back({b, SetK::Act, ri});
                break;
            }
        }
        y += h;
    }
    g_rt->PopAxisAlignedClip();
    // what was drawn outside the visible part can't be clicked
    for (size_t i = hitsBefore; i < g_set.hits.size(); ++i) {
        D2D1_RECT_F& r = g_set.hits[i].r;
        r.top = std::max(r.top, ct);
        r.bottom = std::min(r.bottom, cbm);
    }
    if (g_set.contentH > cbm - ct) {
        const float viewH = cbm - ct, thumbH = std::max(24.f, viewH * viewH / g_set.contentH);
        const float thumbY = ct + (viewH - thumbH) * (g_set.scroll / (g_set.contentH - viewH));
        Round(D2D1::RectF(R - 7, thumbY, R - 4, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.14f);
    }
    // the text box follows its field, and hides when it scrolls out
    if (g_set.edit && !g_set.editId.empty()) {
        if (editShown && editRect.top >= ct && editRect.bottom <= cbm) {
            RECT sr = ScreenRect(editRect);
            POINT a{sr.left, sr.top};
            ScreenToClient(g_hwnd, &a);
            const int pxl = MulDiv(9, g_dpi, 96), hh = MulDiv(18, g_dpi, 96);
            const int top = a.y + (sr.bottom - sr.top - hh) / 2;
            SetWindowPos(g_set.edit, HWND_TOP, a.x + pxl, top, sr.right - sr.left - pxl - 6, hh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        } else {
            ShowWindow(g_set.edit, SW_HIDE);
        }
    }
}

// ---- mouse and keyboard
int SettingsHitAt(float x, float y) {
    for (int i = static_cast<int>(g_set.hits.size()) - 1; i >= 0; --i) {
        const D2D1_RECT_F& r = g_set.hits[i].r;
        if (r.bottom > r.top && x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

void SettingsMouseMove(float x, float y) {
    const int h = SettingsHitAt(x, y);
    const SetHit now = h >= 0 ? g_set.hits[h] : SetHit{};
    const bool same = (h >= 0) == g_set.hovering && (h < 0 || (now.k == g_set.hoverHit.k && now.row == g_set.hoverHit.row && now.part == g_set.hoverHit.part));
    if (same) return;
    g_set.hovering = h >= 0;
    g_set.hoverHit = now;
    Invalidate();
}

void SettingsClick(float x, float y) {
    SettingsCommitText();
    const D2D1_RECT_F& b = g_set.box;
    if (!(x >= b.left && x < b.right && y >= b.top && y < b.bottom)) {
        SettingsClose();
        return;
    }
    const int hi = SettingsHitAt(x, y);
    if (hi < 0) {
        g_set.captureOn = false;
        Invalidate();
        return;
    }
    const SetHit h = g_set.hits[hi];
    if (h.k == SetK::Close) {
        SettingsClose();
        return;
    }
    if (h.k == SetK::Nav) {
        SettingsSection(h.part);
        return;
    }
    std::vector<SetRow> rows = SettingsRows(g_set.section);
    if (h.row < 0 || h.row >= static_cast<int>(rows.size())) return;
    const SetRow& r = rows[h.row];
    if (h.k != SetK::Key) g_set.captureOn = false;
    switch (h.k) {
        case SetK::Toggle: r.setB(!r.getB()); break;
        case SetK::Minus: r.setI(std::max(r.lo, r.getI() - r.step)); break;
        case SetK::Plus: r.setI(std::min(r.hi, r.getI() + r.step)); break;
        case SetK::Choice:
            if (h.part != r.getI()) r.setI(h.part);
            break;
        case SetK::Field:
            if (h.id == r.id) SettingsBeginText(r.id, r.getS());
            else if (h.id == r.id2) SettingsBeginText(r.id2, r.getS2());
            return;
        case SetK::Key:
            g_set.capture = r.cmd;
            g_set.captureOn = true;
            break;
        case SetK::Del:
            if (r.del) r.del();
            break;
        case SetK::Reset:
            if (r.setS) r.setS(Hex(r.def));
            break;
        case SetK::Act: {
            const std::function<void()> act = r.act;
            const bool leaves = r.k == SrK::Button;  // the buttons open something else: the panel steps aside
            if (leaves) SettingsClose();
            if (act) act();
            if (!leaves && g_set.draftOn) {  // a new row: its first field is ready to type in
                Invalidate();
                UpdateWindow(g_hwnd);
                SettingsBeginText(L"draft.a", g_set.draftA);
            }
            return;
        }
        default: break;
    }
    Invalidate();
}

void SettingsWheel(int delta) {
    g_set.scroll -= delta / 120.f * 3 * 36.f;
    g_set.scroll = std::clamp(g_set.scroll, 0.f, std::max(0.f, g_set.contentH - (g_set.content.bottom - g_set.content.top)));
    Invalidate();
}

// Keys while a shortcut is being captured: the next one is the shortcut.
bool SettingsCaptureKey(WPARAM vk) {
    if (!g_set.captureOn) return false;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return true;
    const UINT mods = (KeyDown(VK_CONTROL) ? ModCtrl : 0) | (KeyDown(VK_SHIFT) ? ModShift : 0) | (KeyDown(VK_MENU) ? ModAlt : 0);
    g_set.captureOn = false;
    if (vk == VK_ESCAPE) {
        Invalidate();
        return true;
    }
    if ((vk == VK_DELETE || vk == VK_BACK) && mods == 0) {
        S.message = AssignShortcut(g_set.capture, 0, 0, true);
    } else if (mods == 0 && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END)) {
        S.message = L"That key is fixed; try Ctrl, Shift or Alt";
    } else {
        S.message = AssignShortcut(g_set.capture, mods, static_cast<UINT>(vk), false);
    }
    RefreshConfigSig();  // written and applied already
    Invalidate();
    return true;
}

void SettingsKey(WPARAM vk) {
    if (SettingsCaptureKey(vk)) return;
    const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT);
    const float page = std::max(36.f, g_set.content.bottom - g_set.content.top - 40);
    switch (vk) {
        case VK_ESCAPE:
            if (!g_set.search.empty()) g_set.search.clear();
            else SettingsClose();
            break;
        case VK_TAB:
            if (ctrl) SettingsSection((g_set.section + (shift ? kSetCount - 1 : 1)) % kSetCount);
            break;
        case VK_UP: if (ctrl) SettingsSection((g_set.section + kSetCount - 1) % kSetCount); else g_set.scroll -= 36; break;
        case VK_DOWN: if (ctrl) SettingsSection((g_set.section + 1) % kSetCount); else g_set.scroll += 36; break;
        case VK_PRIOR: g_set.scroll -= page; break;
        case VK_NEXT: g_set.scroll += page; break;
        case VK_HOME: g_set.scroll = 0; break;
        case VK_END: g_set.scroll = g_set.contentH; break;
        case VK_BACK:
            if (g_set.section == 2 && !g_set.search.empty()) {
                g_set.search.pop_back();
                g_set.scroll = 0;
            }
            break;
        default: return;
    }
    g_set.scroll = std::clamp(g_set.scroll, 0.f, std::max(0.f, g_set.contentH - (g_set.content.bottom - g_set.content.top)));
    Invalidate();
}

void SettingsChar(wchar_t c) {
    if (c < 32 || g_set.captureOn || g_set.section != 2) return;  // typing searches the shortcuts
    g_set.search += c;
    g_set.scroll = 0;
    Invalidate();
}

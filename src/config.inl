// Settings: %APPDATA%\Jolt\config.txt. Generated with the default values the first time
// and reloaded automatically when saved. Included from app.cpp after the favorites section.

enum class Cmd {
    None, Up, Enter, Open, OpenTab, Back, Forward, Rename, Delete, NewFolder, Copy, Cut,
    Paste, SelectAll, Favorite, NewTab, CloseTab, NextTab, PreviousTab, Path,
    Sidebar, Reload, Timings, ContextMenu, AppMenu, Palette, GoTo, SearchSubfolders, Expand, Collapse,
    GoToLocation, Preview, Split, OtherPane, CopyToOther, MoveToOther, CopyPath, CopyName, CopyPathUnix, NewFile, Duplicate, GroupInFolder,
    Properties, FolderSizes, ShowFolders, ShowFiles, ShowHidden, ShowExtensions, ReopenTab, CloseOthers,
    Terminal, OpenExplorer, RealLocation, CollapseGroups, ExpandGroups, GroupByDay, GroupByType, NoGroups,
    SortByType, ZoomIn, ZoomOut, ZoomReset, RowsTaller, RowsShorter, EmptyRecycleBin,
    Undo, Redo, OpenAsAdmin, PinTab, Restore,
    Install, Quit, RenameMany, FolderSiblings, ViewList, ViewGrid, ViewBigger, ViewSmaller, Tag, PreviewWrap,
    FindDuplicates, SelectExtraCopies, SelectPattern, SelectSameExtension, InvertSelection,
    CustomFirst = 1000  // + index into g_cfg.custom: the user's own commands
};
inline bool IsCustom(Cmd c) { return static_cast<int>(c) >= static_cast<int>(Cmd::CustomFirst); }
inline int CustomIndex(Cmd c) { return static_cast<int>(c) - static_cast<int>(Cmd::CustomFirst); }
inline Cmd CustomCmd(size_t i) { return static_cast<Cmd>(static_cast<int>(Cmd::CustomFirst) + static_cast<int>(i)); }
enum : UINT { ModCtrl = 1, ModShift = 2, ModAlt = 4 };

struct KeyBind {
    UINT mods, vk;
    Cmd cmd;
};

constexpr int kHotCountMax = 30;

struct FileColorRule {
    std::vector<std::wstring> patterns;  // ".png", "*backup*", "folders"
    UINT32 color;
};
struct Config {
    std::wstring font = L"Segoe UI";
    float fontSize = 12;
    bool showHidden = true;
    bool previewWrap = true;  // long lines wrap in the text preview
    bool cloudStatus = true;  // marks the files of cloud folders that are online only, or always on this PC
    int typoMin = 75;         // searches also show names this close to what was typed, in percent; 0: only real matches
    bool background = true, tray = true, openDrives = false;
    int hotCount = 10;  // folders in Recents (0: no section)
    std::wstring globalKey = L"Win+J";
    bool colDate = true, colSize = true;
    std::vector<KeyBind> keys;
    std::vector<FileColorRule> fileColors;  // [file_colors]
    struct Custom {
        std::wstring name, line;  // "Open in VS Code", "code \"{dir}\""
    };
    std::vector<Custom> custom;  // [commands], in file order
} g_cfg;

struct CmdDef {
    const wchar_t* name;
    Cmd cmd;
    const wchar_t* defaults;
    const wchar_t* help;
};
const CmdDef kCommands[] = {
    {L"up", Cmd::Up, L"Backspace, Left, Alt+Up", L"go to the parent folder"},
    {L"folder_siblings", Cmd::FolderSiblings, L"Alt+Down", L"list the folders next to this one, to jump to one"},
    {L"enter", Cmd::Enter, L"Right", L"enter the highlighted folder (does not open files)"},
    {L"open", Cmd::Open, L"Enter", L"open the highlighted item (folder or file)"},
    {L"open_tab", Cmd::OpenTab, L"Ctrl+Enter", L"open the highlighted folder in a new tab"},
    {L"open_as_admin", Cmd::OpenAsAdmin, L"Ctrl+Shift+Enter", L"open the highlighted folder (or this one) in a window as administrator"},
    {L"back", Cmd::Back, L"Alt+Left", L"back"},
    {L"forward", Cmd::Forward, L"Alt+Right", L"forward"},
    {L"rename", Cmd::Rename, L"F2", L"rename (several selected: the rename panel)"},
    {L"rename_many", Cmd::RenameMany, L"Ctrl+F2", L"rename several at once: pattern, find and replace, with a preview"},
    {L"delete", Cmd::Delete, L"Del, Shift+Del", L"move to the Recycle Bin (with Shift, permanently)"},
    {L"new_folder", Cmd::NewFolder, L"Ctrl+Shift+N", L"new folder"},
    {L"copy", Cmd::Copy, L"Ctrl+C", L"copy"},
    {L"cut", Cmd::Cut, L"Ctrl+X", L"cut"},
    {L"paste", Cmd::Paste, L"Ctrl+V", L"paste"},
    {L"select_all", Cmd::SelectAll, L"Ctrl+A", L"select all"},
    {L"select_pattern", Cmd::SelectPattern, L"Ctrl+Alt+A", L"select by pattern: *.png, report*, >10mb, ext:pdf, type:folder, !tmp"},
    {L"select_same_extension", Cmd::SelectSameExtension, L"Ctrl+Alt+X", L"select every file with the extension of the highlighted one"},
    {L"invert_selection", Cmd::InvertSelection, L"Ctrl+I", L"invert the selection"},
    {L"favorite", Cmd::Favorite, L"Ctrl+D", L"add or remove the current folder from favorites"},
    {L"new_tab", Cmd::NewTab, L"Ctrl+T", L"new tab"},
    {L"close_tab", Cmd::CloseTab, L"Ctrl+W", L"close tab"},
    {L"next_tab", Cmd::NextTab, L"Ctrl+Tab", L"next tab"},
    {L"previous_tab", Cmd::PreviousTab, L"Ctrl+Shift+Tab", L"previous tab"},
    {L"path", Cmd::Path, L"Ctrl+L", L"type a path"},
    {L"sidebar", Cmd::Sidebar, L"Ctrl+B", L"show or hide the sidebar"},
    {L"reload", Cmd::Reload, L"Ctrl+R", L"reload the folder"},
    {L"timings", Cmd::Timings, L"F12", L"show or hide timings"},
    {L"context_menu", Cmd::ContextMenu, L"Shift+F10, Menu", L"context menu"},
    {L"app_menu", Cmd::AppMenu, L"Alt+M", L"Jolt menu"},
    {L"palette", Cmd::Palette, L"Ctrl+K", L"command palette"},
    {L"go_to", Cmd::GoTo, L"Ctrl+P", L"go to a recent, favorite or system folder, or a path"},
    {L"search_subfolders", Cmd::SearchSubfolders, L"Ctrl+Shift+F", L"search this folder and all its subfolders"},
    {L"find_duplicates", Cmd::FindDuplicates, L"Ctrl+Alt+F", L"find duplicate files in this folder and its subfolders"},
    {L"select_extra_copies", Cmd::SelectExtraCopies, L"Ctrl+Shift+A", L"in duplicates: select every copy but the oldest of each set"},
    {L"expand", Cmd::Expand, L"Shift+Right", L"expand or collapse the highlighted folder (tree)"},
    {L"collapse", Cmd::Collapse, L"Shift+Left", L"collapse the folder (or go to the one above in the tree)"},
    {L"go_to_location", Cmd::GoToLocation, L"Ctrl+Shift+E", L"open the folder containing the highlighted item"},
    {L"preview", Cmd::Preview, L"Space", L"show or hide the preview"},
    {L"preview_wrap", Cmd::PreviewWrap, L"", L"wrap long lines in the text preview (toggle)"},
    {L"split", Cmd::Split, L"Ctrl+S", L"split view with two panes"},
    {L"other_pane", Cmd::OtherPane, L"Tab", L"switch to the other pane"},
    {L"copy_to_other", Cmd::CopyToOther, L"F5", L"copy the selection to the other pane"},
    {L"move_to_other", Cmd::MoveToOther, L"F6", L"move the selection to the other pane"},
    {L"copy_path", Cmd::CopyPath, L"Ctrl+Shift+C", L"copy the path of the selection (or of the folder)"},
    {L"copy_name", Cmd::CopyName, L"Ctrl+Alt+C", L"copy the name of the selection (or of the folder)"},
    {L"copy_path_unix", Cmd::CopyPathUnix, L"", L"copy the path with forward slashes (C:/Users/...)"},
    {L"new_file", Cmd::NewFile, L"Ctrl+N", L"new file"},
    {L"duplicate", Cmd::Duplicate, L"Ctrl+Shift+D", L"duplicate the selection"},
    {L"group_in_folder", Cmd::GroupInFolder, L"Ctrl+Shift+G", L"move the selection into a new folder"},
    {L"properties", Cmd::Properties, L"Alt+Enter", L"Windows properties"},
    {L"folder_sizes", Cmd::FolderSizes, L"Alt+R", L"calculate folder sizes"},
    {L"show_folders", Cmd::ShowFolders, L"", L"show or hide folders (toggle folders)"},
    {L"show_files", Cmd::ShowFiles, L"", L"show or hide files (toggle files)"},
    {L"show_hidden", Cmd::ShowHidden, L"Ctrl+H", L"show or hide hidden files (toggle hidden files)"},
    {L"show_extensions", Cmd::ShowExtensions, L"", L"show or hide extensions (toggle file extensions)"},
    {L"reopen_tab", Cmd::ReopenTab, L"Ctrl+Shift+T", L"reopen the last closed tab"},
    {L"close_others", Cmd::CloseOthers, L"Ctrl+Alt+W", L"close the other tabs"},
    {L"pin_tab", Cmd::PinTab, L"Ctrl+Alt+P", L"pin the tab, or unpin it: it stays at the left, small, and doesn't close"},
    {L"terminal", Cmd::Terminal, L"Ctrl+Alt+T", L"open the terminal in this folder (console, cmd, powershell)"},
    {L"open_explorer", Cmd::OpenExplorer, L"Ctrl+Alt+E", L"open in Windows File Explorer"},
    {L"real_location", Cmd::RealLocation, L"Ctrl+Alt+L", L"go to the real location of a shortcut"},
    {L"collapse_groups", Cmd::CollapseGroups, L"", L"collapse all groups"},
    {L"expand_groups", Cmd::ExpandGroups, L"", L"expand all groups"},
    {L"group_by_day", Cmd::GroupByDay, L"", L"group this folder by day"},
    {L"group_by_type", Cmd::GroupByType, L"", L"group this folder by type"},
    {L"no_groups", Cmd::NoGroups, L"", L"remove the groups from this folder"},
    {L"sort_by_type", Cmd::SortByType, L"", L"sort by type (extension)"},
    {L"zoom_in", Cmd::ZoomIn, L"Ctrl++, Ctrl+Shift++, Ctrl+Num+", L"enlarge the interface (zoom)"},
    {L"zoom_out", Cmd::ZoomOut, L"Ctrl+-, Ctrl+Num-", L"shrink the interface (zoom)"},
    {L"zoom_reset", Cmd::ZoomReset, L"Ctrl+0", L"reset the interface to normal size (zoom)"},
    {L"rows_taller", Cmd::RowsTaller, L"", L"more space between rows"},
    {L"rows_shorter", Cmd::RowsShorter, L"", L"less space between rows"},
    {L"view_list", Cmd::ViewList, L"", L"show this folder as a list (details)"},
    {L"view_grid", Cmd::ViewGrid, L"", L"show this folder as a grid of thumbnails"},
    {L"view_bigger", Cmd::ViewBigger, L"", L"bigger cells in the grid (Ctrl+Alt+wheel up)"},
    {L"view_smaller", Cmd::ViewSmaller, L"", L"smaller cells, down to the list (Ctrl+Alt+wheel down)"},
    {L"tag", Cmd::Tag, L"", L"tag the selection: add or remove a tag"},
    {L"empty_recycle_bin", Cmd::EmptyRecycleBin, L"", L"empty the Recycle Bin"},
    {L"restore", Cmd::Restore, L"Ctrl+Shift+R", L"in the Recycle Bin: put the selection back where it was"},
    {L"undo", Cmd::Undo, L"Ctrl+Z", L"undo the last action (rename, move, copy, create, Recycle Bin)"},
    {L"redo", Cmd::Redo, L"Ctrl+Y, Ctrl+Shift+Z", L"redo what was undone"},
    {L"install", Cmd::Install, L"", L"install Jolt, add it to PATH, make it the default"},
    {L"quit", Cmd::Quit, L"", L"really quit Jolt"},
};

struct KeyName {
    const wchar_t* name;
    UINT vk;
};
const KeyName kKeyNames[] = {
    {L"enter", VK_RETURN},    {L"esc", VK_ESCAPE},     {L"escape", VK_ESCAPE},   {L"del", VK_DELETE},
    {L"delete", VK_DELETE},   {L"backspace", VK_BACK}, {L"tab", VK_TAB},         {L"space", VK_SPACE},
    {L"left", VK_LEFT},       {L"right", VK_RIGHT},    {L"up", VK_UP},           {L"down", VK_DOWN},
    {L"home", VK_HOME},       {L"end", VK_END},        {L"pageup", VK_PRIOR},    {L"pagedown", VK_NEXT},
    {L"menu", VK_APPS},       {L"apps", VK_APPS},      {L"insert", VK_INSERT},   {L"ins", VK_INSERT},
    {L"+", VK_OEM_PLUS},      {L"-", VK_OEM_MINUS},    {L",", VK_OEM_COMMA},     {L".", VK_OEM_PERIOD},
    {L"num+", VK_ADD},        {L"num-", VK_SUBTRACT},
};

struct ColorDef {
    const wchar_t* name;
    UINT32* var;
    UINT32 def;
    const wchar_t* help;
};
const ColorDef kColors[] = {
    {L"background", &cBg, 0x1A1A1E, L"list background"},
    {L"toolbar", &cTop, 0x202024, L"navigation bar, active tab and menus"},
    {L"tabs", &cTabBar, 0x0F0F12, L"tab bar (title)"},
    {L"sidebar", &cSide, 0x151518, L"sidebar and status bar"},
    {L"border", &cBorder, 0x2B2B31, L"lines and borders"},
    {L"text", &cText, 0xE9E9EE, L"main text"},
    {L"text_secondary", &cDim, 0x8C8C97, L"dates, sizes, inactive tabs"},
    {L"text_faint", &cFaint, 0x4C4C55, L"section titles, disabled items"},
    {L"accent", &cAccent, 0x4F8CF7, L"highlights and drop target borders"},
    {L"selection", &cSel, 0x2C4C86, L"selected row"},
    {L"error", &cError, 0xF07178, L"error messages"},
    {L"disk_usage", &cDisk, 0x3B8EEA, L"disk usage bar"},
    {L"disk_usage_full", &cDiskFull, 0xE5484D, L"disk usage bar at 90% or more"},
};

std::wstring ConfigDir() {
    std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\'));
}
std::wstring ConfigFile() {
    std::wstring d = ConfigDir();
    return d.empty() ? L"" : d + L"\\config.txt";
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t c = s.find(L',', start);
        std::wstring part = Trim(s.substr(start, c == std::wstring::npos ? std::wstring::npos : c - start));
        if (!part.empty()) out.push_back(part);
        if (c == std::wstring::npos) break;
        start = c + 1;
    }
    return out;
}

// "Ctrl+Shift+N", "F2", "Alt+Left", "Del"...
bool ParseKey(const std::wstring& spec, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    std::wstring s = Lower(Trim(spec));
    size_t start = 0;
    for (;;) {
        size_t plus = s.find(L'+', start);
        bool last = plus == std::wstring::npos || plus + 1 >= s.size();  // a "+" alone at the end is the "+" key
        std::wstring part = Trim(last ? s.substr(start) : s.substr(start, plus - start));
        if (!last) {
            if (part == L"ctrl" || part == L"control") mods |= ModCtrl;
            else if (part == L"shift") mods |= ModShift;
            else if (part == L"alt") mods |= ModAlt;
            else return false;
            start = plus + 1;
            continue;
        }
        if (part.size() == 1) {
            wchar_t c = part[0];
            if (c >= L'a' && c <= L'z') vk = static_cast<UINT>(c - L'a' + 'A');
            else if (c >= L'0' && c <= L'9') vk = static_cast<UINT>(c);
        }
        if (!vk && part.size() >= 2 && part[0] == L'f') {
            int n = _wtoi(part.c_str() + 1);
            if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
        }
        if (!vk)
            for (const KeyName& k : kKeyNames)
                if (part == k.name) vk = k.vk;
        return vk != 0;
    }
}

void AddBindings(Cmd cmd, const std::wstring& list) {
    for (const std::wstring& spec : SplitList(list)) {
        UINT mods, vk;
        if (ParseKey(spec, mods, vk)) g_cfg.keys.push_back({mods, vk, cmd});
    }
}

std::wstring Hex(UINT32 c) {
    wchar_t b[8];
    swprintf(b, 8, L"%06X", c & 0xFFFFFF);
    return b;
}

void WriteDefaultConfig() {
    std::wstring f = ConfigFile();
    if (f.empty()) return;
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF";  // BOM: so Notepad opens it as UTF-8
    std::string s =
        "# Jolt settings. Reloads automatically when you save this file.\n"
        "# Lines starting with # are comments. If you delete an option, it goes back to its default value.\n"
        "\n[general]\n"
        "# Font and size (in pixels at 100%) of the whole interface.\n"
        "font = Segoe UI\n"
        "font_size = 12\n"
        "# Height of each list row.\n"
        "row_height = 25\n"
        "# Size of the whole interface, as a percentage (Ctrl + and Ctrl - change it).\n"
        "scale = 100\n"
        "# Sidebar width.\n"
        "sidebar_width = 208\n"
        "# How many folders to show in Recents, sorted by use. With 0 the section does not appear.\n"
        "recents = 10\n"
        "# Preview width (Space).\n"
        "preview_width = 360\n"
        "# Wrap long lines in the text preview: yes / no\n"
        "preview_wrap = yes\n"
        "# Cloud folders (OneDrive and the like): a cloud next to what is online only, a check next to what is always\n"
        "# on this PC: yes / no. Jolt never downloads them to list or preview them, whatever this says.\n"
        "cloud_status = yes\n"
        "# Typos in searches: names close to what you typed appear under the real matches, with how close they are.\n"
        "# The lowest percentage to show, from 50 to 100 (100: only swapped letters), or no.\n"
        "filter_typos = 75\n"
        "# Show hidden and system files (dimmed): yes / no\n"
        "show_hidden = yes\n"
        "# Folders above files: yes / no / name (only when sorting by name; by date and size they are mixed)\n"
        "folders_first = name\n"
        "# Closing the window hides it and Jolt stays ready to come back instantly: yes / no\n"
        "run_in_background = yes\n"
        "# Shortcut from any app: opens Jolt, and hides it if it is already in front. Empty: no shortcut.\n"
        "# With File Explorer in front, opens its folder. Windows does not give up Win+E.\n"
        "global_hotkey = Win+J\n"
        "# Icon in the system tray: yes / no\n"
        "tray_icon = yes\n"
        "# Open drives and USB sticks as soon as they are connected: yes / no\n"
        "open_drives = no\n"
        "\n[columns]\n"
        "# Visible columns besides the name: modified, size\n"
        "show = modified, size\n"
        "modified_width = 118\n"
        "size_width = 72\n"
        "\n[colors]\n"
        "# RRGGBB in hexadecimal.\n";
    for (const ColorDef& c : kColors) s += Utf8(std::wstring(c.name) + L" = " + Hex(c.def) + L"    # " + c.help + L"\n");
    s +=
        "\n[file_colors]\n"
        "# Name colors in the list, by extension or pattern (and \"folders\"): patterns = RRGGBB. The first rule that fits wins.\n"
        "# .jpg .png .gif .webp = 7FB3FF\n"
        "# .zip .7z .rar = C9A86A\n"
        "# *backup* *.bak = E06C75\n"
        "\n[shortcuts]\n"
        "# Several keys per command, separated by commas. Modifiers: Ctrl, Shift, Alt.\n"
        "# Keys: letters, digits, F1..F24, Enter, Esc, Del, Backspace, Tab, Space, Left, Right,\n"
        "# Up, Down, Home, End, PageUp, PageDown, Menu, Insert, + - , .\n"
        "# The Up/Down arrows, PageUp/PageDown, Home/End (with Shift they extend the selection), Esc,\n"
        "# Ctrl+Space, Ctrl+1..9 (favorites) and Alt+1..9 (tabs) are fixed.\n";
    for (const CmdDef& c : kCommands) s += Utf8(std::wstring(c.name) + L" = " + c.defaults + L"    # " + c.help + L"\n");
    s +=
        "\n[commands]\n"
        "# Your own commands: name = program and arguments. They appear in Ctrl+K and in the right-click menu,\n"
        "# and get a shortcut by their name in [shortcuts] (open in vs code = Ctrl+Alt+V).\n"
        "# {file} the highlighted item, {files} everything selected (quoted), {dir} the folder, {name} the item's name.\n"
        "# The program runs in the folder; a name alone is found in PATH.\n"
        "# Open in VS Code = code \"{dir}\"\n"
        "# Edit with Notepad = notepad \"{file}\"\n"
        "# PowerShell here = wt -d \"{dir}\" powershell\n";
    out << s;
}

bool ParseBool(const std::wstring& v) {
    std::wstring s = Lower(Trim(v));
    return s == L"yes" || s == L"true" || s == L"1" || s == L"on";
}

void LoadConfig() {
    // default values
    g_cfg = Config{};
    for (const ColorDef& c : kColors) *c.var = c.def;
    kRow = 25;
    g_uiPx = 12;
    kSideW = 208;
    g_previewW = kPreviewDefaultW;
    g_foldersFirst = 2;
    kDateW = 118;
    kSizeW = 72;
    for (const CmdDef& c : kCommands) AddBindings(c.cmd, c.defaults);

    std::wstring f = ConfigFile();
    if (f.empty()) return;
    std::ifstream in(f, std::ios::binary);
    if (!in) {
        WriteDefaultConfig();
        return;
    }
    std::vector<KeyBind> userKeys;
    bool anyKeys = false;
    std::vector<std::pair<std::wstring, std::wstring>> customKeys;  // [shortcuts] lines naming a custom command
    // the configured command replaces its default keys; the user's keys win over any other default
    auto bindUser = [&](Cmd cmd, const std::wstring& val) {
        anyKeys = true;
        std::vector<KeyBind> saved = std::move(g_cfg.keys);
        g_cfg.keys.clear();
        AddBindings(cmd, val);
        userKeys.insert(userKeys.end(), g_cfg.keys.begin(), g_cfg.keys.end());
        g_cfg.keys = std::move(saved);
        g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(), [&](const KeyBind& k) { return k.cmd == cmd; }),
                         g_cfg.keys.end());
    };
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw), section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        size_t hash = line.find(L'#');
        if (hash != std::wstring::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Lower(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring rawKey = Trim(line.substr(0, eq)), key = Lower(rawKey), val = Trim(line.substr(eq + 1));
        if (section == L"commands") {
            if (!rawKey.empty() && !val.empty()) g_cfg.custom.push_back({rawKey, val});
        } else if (section == L"general") {
            if (key == L"font" && !val.empty()) g_cfg.font = val;
            else if (key == L"font_size") g_cfg.fontSize = std::clamp(static_cast<float>(_wtof(val.c_str())), 8.f, 32.f);
            else if (key == L"row_height") kRow = std::clamp(static_cast<float>(_wtof(val.c_str())), 16.f, 60.f);
            else if (key == L"scale") g_uiPx = std::clamp(static_cast<int>(std::lround(_wtof(val.c_str()) * 12 / 100)), kUiPxMin, kUiPxMax);
            else if (key == L"sidebar_width") kSideW = std::clamp(static_cast<float>(_wtof(val.c_str())), 120.f, 500.f);
            else if (key == L"recents") g_cfg.hotCount = std::clamp(_wtoi(val.c_str()), 0, kHotCountMax);
            else if (key == L"show_hidden") g_cfg.showHidden = ParseBool(val);
            else if (key == L"preview_wrap") g_cfg.previewWrap = ParseBool(val);
            else if (key == L"cloud_status") g_cfg.cloudStatus = ParseBool(val);
            else if (key == L"filter_typos") g_cfg.typoMin = _wtoi(val.c_str()) > 0 ? std::clamp(_wtoi(val.c_str()), 50, 100) : ParseBool(val) ? 75 : 0;
            else if (key == L"run_in_background") g_cfg.background = ParseBool(val);
            else if (key == L"global_hotkey") g_cfg.globalKey = val;
            else if (key == L"tray_icon") g_cfg.tray = ParseBool(val);
            else if (key == L"open_drives") g_cfg.openDrives = ParseBool(val);
            else if (key == L"preview_width") g_previewW = std::clamp(static_cast<float>(_wtof(val.c_str())), kPreviewMinW, 4000.f);
            else if (key == L"folders_first") {
                std::wstring v = Lower(Trim(val));
                g_foldersFirst = v == L"name" ? 2 : ParseBool(v) ? 1 : 0;
            }
        } else if (section == L"columns") {
            if (key == L"show") {
                g_cfg.colDate = g_cfg.colSize = false;
                for (const std::wstring& c : SplitList(Lower(val))) {
                    if (c == L"modified") g_cfg.colDate = true;
                    if (c == L"size") g_cfg.colSize = true;
                }
            } else if (key == L"modified_width") kDateW = std::clamp(static_cast<float>(_wtof(val.c_str())), kDateMin, kDateMax);
            else if (key == L"size_width") kSizeW = std::clamp(static_cast<float>(_wtof(val.c_str())), kSizeMin, kSizeMax);
        } else if (section == L"colors") {
            for (const ColorDef& c : kColors)
                if (key == c.name) {
                    std::wstring h = val;
                    if (!h.empty() && h[0] == L'#') h.erase(0, 1);
                    if (h.size() == 6) *c.var = static_cast<UINT32>(wcstoul(h.c_str(), nullptr, 16));
                }
        } else if (section == L"file_colors") {
            std::wstring h = val;
            if (!h.empty() && h[0] == L'#') h.erase(0, 1);
            FileColorRule r;
            size_t p = 0;
            while (p < key.size()) {
                size_t sp = key.find(L' ', p);
                std::wstring w = key.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p);
                p = sp == std::wstring::npos ? key.size() : sp + 1;
                if (!w.empty()) r.patterns.push_back(w);
            }
            if (h.size() == 6 && !r.patterns.empty()) {
                r.color = static_cast<UINT32>(wcstoul(h.c_str(), nullptr, 16));
                g_cfg.fileColors.push_back(std::move(r));
            }
        } else if (section == L"shortcuts") {
            bool builtin = false;
            for (const CmdDef& c : kCommands)
                if (key == c.name) {
                    builtin = true;
                    bindUser(c.cmd, val);
                }
            if (!builtin) customKeys.push_back({key, val});
        }
    }
    for (const auto& [key, val] : customKeys)
        for (size_t i = 0; i < g_cfg.custom.size(); ++i)
            if (Lower(g_cfg.custom[i].name) == key) bindUser(CustomCmd(i), val);
    if (anyKeys) {
        // the user's keys take priority over any other default binding
        for (const KeyBind& u : userKeys)
            g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(),
                                            [&](const KeyBind& k) { return k.mods == u.mods && k.vk == u.vk; }),
                             g_cfg.keys.end());
        g_cfg.keys.insert(g_cfg.keys.begin(), userKeys.begin(), userKeys.end());
    }
}

// Writes a command's keys to config.txt (and takes that key away from any other command).
// Keeps comments and the rest of the file. If the section is missing, adds it.
void ConfigWriteShortcut(const std::wstring& cmdName, const std::wstring& specs, UINT mods, UINT vk) {
    std::wstring f = ConfigFile();
    if (f.empty()) return;
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaultConfig();
    std::ifstream in(f, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw);
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::wstring section;
    int sectionEnd = -1;  // last line of [shortcuts]
    bool written = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring body = lines[i], comment;
        size_t hash = body.find(L'#');
        if (hash != std::wstring::npos) {
            comment = body.substr(hash);
            body = body.substr(0, hash);
        }
        std::wstring tb = Trim(body);
        if (tb.empty()) continue;
        if (tb.front() == L'[' && tb.back() == L']') {
            section = Lower(tb.substr(1, tb.size() - 2));
            continue;
        }
        if (section != L"shortcuts") continue;
        sectionEnd = static_cast<int>(i);
        size_t eq = tb.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(tb.substr(0, eq))), val = Trim(tb.substr(eq + 1));
        std::wstring pad = comment.empty() ? L"" : L"    " + comment;
        if (key == Lower(cmdName)) {
            lines[i] = cmdName + L" = " + specs + pad;
            written = true;
            continue;
        }
        // another command with the same key: take it away
        std::vector<std::wstring> keep;
        bool changed = false;
        for (const std::wstring& spec : SplitList(val)) {
            UINT m, k;
            if (ParseKey(spec, m, k) && m == mods && k == vk) changed = true;
            else keep.push_back(spec);
        }
        if (changed) {
            std::wstring joined;
            for (size_t j = 0; j < keep.size(); ++j) joined += (j ? L", " : L"") + keep[j];
            lines[i] = key + L" = " + joined + pad;
        }
    }
    // commands that use the key by default and are not in the file: pin them without it
    for (const CmdDef& c : kCommands) {
        if (c.name == cmdName) continue;
        bool inFile = false;
        std::wstring sec;
        for (const std::wstring& l : lines) {
            std::wstring tb = Trim(l.substr(0, l.find(L'#')));
            if (!tb.empty() && tb.front() == L'[' && tb.back() == L']') sec = Lower(tb.substr(1, tb.size() - 2));
            else if (sec == L"shortcuts" && tb.find(L'=') != std::wstring::npos && Lower(Trim(tb.substr(0, tb.find(L'=')))) == c.name) inFile = true;
        }
        if (inFile) continue;
        std::vector<std::wstring> keep;
        bool changed = false;
        for (const std::wstring& spec : SplitList(c.defaults)) {
            UINT m, k;
            if (ParseKey(spec, m, k) && m == mods && k == vk) changed = true;
            else keep.push_back(spec);
        }
        if (!changed) continue;
        std::wstring joined;
        for (size_t j = 0; j < keep.size(); ++j) joined += (j ? L", " : L"") + keep[j];
        std::wstring nl = std::wstring(c.name) + L" = " + joined + L"    # " + c.help;
        if (sectionEnd >= 0) lines.insert(lines.begin() + (++sectionEnd), nl);
        else {
            lines.push_back(L"");
            lines.push_back(L"[shortcuts]");
            lines.push_back(nl);
            sectionEnd = static_cast<int>(lines.size()) - 1;
        }
    }
    if (!written) {
        std::wstring nl = cmdName + L" = " + specs;
        for (const CmdDef& c : kCommands)
            if (c.name == cmdName) nl += std::wstring(L"    # ") + c.help;
        if (sectionEnd >= 0) lines.insert(lines.begin() + sectionEnd + 1, nl);
        else {
            lines.push_back(L"");
            lines.push_back(L"[shortcuts]");
            lines.push_back(nl);
        }
    }
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF";
    for (const std::wstring& l : lines) out << Utf8(l) << "\r\n";
}

// Changes an option in config.txt (keeps comments); if it is missing, adds it at the start of the section.
void ConfigSetValue(const std::wstring& sectionName, const std::wstring& key, const std::wstring& value) {
    std::wstring f = ConfigFile();
    if (f.empty()) return;
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaultConfig();
    std::ifstream in(f, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw);
    std::vector<std::wstring> lines;
    for (size_t pos = 0;;) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    std::wstring section;
    int header = -1;
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        std::wstring body = lines[i], comment;
        size_t hash = body.find(L'#');
        if (hash != std::wstring::npos) {
            comment = body.substr(hash);
            body = body.substr(0, hash);
        }
        std::wstring tb = Trim(body);
        if (!tb.empty() && tb.front() == L'[' && tb.back() == L']') {
            section = Lower(tb.substr(1, tb.size() - 2));
            if (section == sectionName) header = static_cast<int>(i);
            continue;
        }
        size_t eq = tb.find(L'=');
        if (section == sectionName && eq != std::wstring::npos && Lower(Trim(tb.substr(0, eq))) == key) {
            lines[i] = key + L" = " + value + (comment.empty() ? L"" : L"    " + comment);
            done = true;
        }
    }
    if (!done) {
        if (header < 0) {
            lines.push_back(L"[" + sectionName + L"]");
            header = static_cast<int>(lines.size()) - 1;
        }
        lines.insert(lines.begin() + header + 1, key + L" = " + value);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF";
    for (const std::wstring& l : lines) out << Utf8(l) << "\r\n";
}

Cmd FindCommand(UINT mods, UINT vk) {
    for (const KeyBind& k : g_cfg.keys)
        if (k.mods == mods && k.vk == vk) return k.cmd;
    return Cmd::None;
}

// Watches the config folder: any change notifies the main window.
constexpr UINT WM_APP_CONFIG = WM_APP + 4;
void ConfigWatcherMain() {
    std::wstring dir = ConfigDir();
    if (dir.empty()) return;
    HANDLE ch = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                 FILE_NOTIFY_CHANGE_SIZE);
    if (ch == INVALID_HANDLE_VALUE) return;
    while (WaitForSingleObject(ch, INFINITE) == WAIT_OBJECT_0) {
        PostMessageW(g_hwnd, WM_APP_CONFIG, 0, 0);
        if (!FindNextChangeNotification(ch)) break;
    }
    FindCloseChangeNotification(ch);
}

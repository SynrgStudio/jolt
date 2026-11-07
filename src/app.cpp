// Jolt — a file explorer for Windows, built to feel instant.
//
// Core ideas:
//  - Listing with NtQueryDirectoryFile in 64 KiB batches (on a separate thread).
//  - Every visited folder stays in memory: going back to it is instant,
//    and a background check looks for changes.
//  - The current folder is watched; if anything changes, it updates itself.
//  - The selected folder is prefetched so Enter is instant.
//  - Real Windows icons are resolved on another thread, only for visible rows.
//  - Only what's visible is drawn (Direct2D + DirectWrite).
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dirlist.h"
#include "version.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "oleaut32.lib")  // UI Automation for test mode (SysFreeString)

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- layout (DIPs)
constexpr float kTabH = 36.f, kToolH = 42.f, kHeadH = 28.f, kStatus = 26.f;
float kRow = 25.f;  // configurable (config.txt)
constexpr float kCapBtnW = 46.f;  // minimize / maximize / close
constexpr float kTitleLeft = 118.f;  // fixed area left of the tabs: future menu + sidebar button
float kSideW = 208.f;  // configurable
bool g_preview = false;  // preview on the right
constexpr float kPreviewDefaultW = 360.f, kPreviewMinW = 240.f;
float g_previewW = kPreviewDefaultW;  // configurable
bool g_pvDrag = false;  // dragging the preview's left edge
float g_pvDragX = 0, g_pvDragW = 0;
constexpr float kSideRow = 25.f;
float kDateW = 118.f, kSizeW = 72.f;  // configurable (and draggable from the header)
constexpr float kDateMin = 64.f, kDateMax = 400.f, kSizeMin = 48.f, kSizeMax = 300.f;
constexpr float kPad = 12.f, kTabMaxW = 190.f, kTabMinW = 70.f;
constexpr float kIcon = 16.f;

// Colors (configurable in config.txt; these are the defaults)
UINT32 cBg = 0x1A1A1E, cTabBar = 0x0F0F12, cTop = 0x202024, cSide = 0x151518, cBorder = 0x2B2B31, cText = 0xE9E9EE,
       cDim = 0x8C8C97, cFaint = 0x4C4C55, cAccent = 0x4F8CF7, cSel = 0x2C4C86, cError = 0xF07178, cDisk = 0x3B8EEA,
       cDiskFull = 0xE5484D;

constexpr wchar_t gBack = 0xE72B, gFwd = 0xE72A, gUp = 0xE74A, gAdd = 0xE710, gClose = 0xE8BB, gChevron = 0xE76C,
                  gPc = 0xE977, gStar = 0xE734, gSortUp = 0xE96D, gSortDown = 0xE96E, gFolder = 0xE8B7,
                  gFile = 0xE7C3, gDrive = 0xEDA2, gMin = 0xE921, gMax = 0xE922, gRestore = 0xE923,
                  gPaneClose = 0xE89F, gPaneOpen = 0xE8A0;

constexpr UINT WM_APP_RESULT = WM_APP + 1, WM_APP_CHANGED = WM_APP + 2, WM_APP_ICON = WM_APP + 3;
constexpr UINT_PTR kTimerRevalidate = 1, kTimerPrefetch = 2, kTimerTabSlide = 12;

HWND g_hwnd = nullptr;
HWND g_edit = nullptr;
// g_dpi is the DPI used for drawing: the monitor's times the interface zoom. Since the whole layout
// is in DIPs, changing it scales the entire app. Zoom is in font pixels over 12 so that text
// lands on whole sizes (12 = 100%).
UINT g_dpi = 96, g_sysDpi = 96;
constexpr int kUiPxMin = 9, kUiPxMax = 24;
int g_uiPx = 12;
void ApplyDpi() { g_dpi = static_cast<UINT>(MulDiv(static_cast<int>(g_sysDpi), g_uiPx, 12)); }

// Optional log: with JOLT_DEBUG=1 it writes to %TEMP%\jolt.log.
void Log(const wchar_t* fmt, ...) {
    static FILE* f = [] {
        wchar_t v[8];
        if (!GetEnvironmentVariableW(L"JOLT_DEBUG", v, 8)) return static_cast<FILE*>(nullptr);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        FILE* out = nullptr;
        _wfopen_s(&out, (std::wstring(tmp) + L"jolt.log").c_str(), L"a, ccs=UTF-8");
        return out;
    }();
    if (!f) return;
    fwprintf(f, L"[%8.1f] ", NowMs());
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fflush(f);
}
// Test mode (testmode.inl): the window isn't shown and Ctrl/Shift/Alt are simulated.
bool g_test = false;
UINT g_testMods = 0;      // 1 = Ctrl, 2 = Shift, 4 = Alt
bool g_testActive = true; // whether the window counts as "active" in test mode (set by a script step)
std::wstring g_dataDir;   // data folder (empty = %APPDATA%\Jolt)
bool KeyDown(int vk) {
    if (g_test)
        return (vk == VK_CONTROL && (g_testMods & 1)) || (vk == VK_SHIFT && (g_testMods & 2)) || (vk == VK_MENU && (g_testMods & 4));
    return GetKeyState(vk) < 0;
}

std::atomic<int> g_sortBy{static_cast<int>(SortBy::Name)};
std::atomic<bool> g_sortAsc{true};
std::atomic<int> g_foldersFirst{2};  // 0 = mixed, 1 = always first, 2 = first only when sorting by name
bool FoldersFirstFor(SortBy by) {
    int m = g_foldersFirst.load();
    return m == 1 || (m == 2 && by == SortBy::Name);
}

// ---------------------------------------------------------------- background work
enum class JobKind { Load, Revalidate, Prefetch };

struct Job {
    std::wstring path;
    JobKind kind = JobKind::Load;
    uint64_t gen = 0;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::shared_ptr<Listing> result;
    double enumMs = 0, sortMs = 0;
    std::atomic<bool> taken{false};
    ~Job() { CloseHandle(done); }
};

std::mutex g_qm;
std::condition_variable g_qcv;
std::deque<std::shared_ptr<Job>> g_high, g_low;
std::atomic<uint64_t> g_navGen{0};

void WorkerMain() {
    for (;;) {
        std::shared_ptr<Job> j;
        {
            std::unique_lock lk(g_qm);
            g_qcv.wait(lk, [] { return !g_high.empty() || !g_low.empty(); });
            auto& q = !g_high.empty() ? g_high : g_low;
            j = q.front();
            q.pop_front();
        }
        // An old prefetch is useless if the user has moved elsewhere.
        bool stale = j->kind == JobKind::Prefetch && j->gen != g_navGen.load();
        if (!stale) {
            auto l = std::make_shared<Listing>();
            double t0 = NowMs();
            EnumerateDir(j->path, EnumMethod::Nt, *l);
            double t1 = NowMs();
            SortBy by = static_cast<SortBy>(g_sortBy.load());
            SortListing(*l, by, g_sortAsc.load(), FoldersFirstFor(by));
            j->enumMs = t1 - t0;
            j->sortMs = NowMs() - t1;
            j->result = std::move(l);
        }
        SetEvent(j->done);
        if (stale) continue;
        auto* hold = new std::shared_ptr<Job>(j);
        if (!PostMessageW(g_hwnd, WM_APP_RESULT, 0, reinterpret_cast<LPARAM>(hold))) delete hold;
    }
}

std::shared_ptr<Job> Enqueue(const std::wstring& path, JobKind kind) {
    auto j = std::make_shared<Job>();
    j->path = path;
    j->kind = kind;
    j->gen = g_navGen.load();
    {
        std::lock_guard lk(g_qm);
        if (kind == JobKind::Load)
            g_high.push_front(j);
        else if (kind == JobKind::Revalidate)
            g_high.push_back(j);
        else
            g_low.push_back(j);
    }
    g_qcv.notify_one();
    return j;
}

// ---------------------------------------------------------------- change watching
// The visible folders are watched (one per pane); with the tree open, their subfolders too.
std::mutex g_wm;
std::vector<std::pair<std::wstring, bool>> g_watchList;
HANDLE g_watchEvt = nullptr;

void SetWatchList(const std::vector<std::pair<std::wstring, bool>>& v) {
    {
        std::lock_guard lk(g_wm);
        if (v == g_watchList) return;
        g_watchList = v;
    }
    SetEvent(g_watchEvt);
}

void WatcherMain() {
    std::vector<HANDLE> hs;
    const DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    for (;;) {
        std::vector<HANDLE> waits{g_watchEvt};
        waits.insert(waits.end(), hs.begin(), hs.end());
        DWORD r = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            for (HANDLE h : hs) FindCloseChangeNotification(h);
            hs.clear();
            std::vector<std::pair<std::wstring, bool>> list;
            {
                std::lock_guard lk(g_wm);
                list = g_watchList;
            }
            for (auto& [p, sub] : list) {
                HANDLE h = FindFirstChangeNotificationW(ToExtendedPath(p).c_str(), sub, flags);
                if (h != INVALID_HANDLE_VALUE) hs.push_back(h);
            }
        } else if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + waits.size()) {
            size_t i = r - WAIT_OBJECT_0 - 1;
            PostMessageW(g_hwnd, WM_APP_CHANGED, 0, 0);
            if (!FindNextChangeNotification(hs[i])) {
                FindCloseChangeNotification(hs[i]);
                hs.erase(hs.begin() + i);
            }
        } else {
            return;
        }
    }
}

// ---------------------------------------------------------------- paths
std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring Key(const std::wstring& p) { return Lower(p); }

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return std::wstring(name) + L"\\";  // "C:" -> "C:\"
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

bool IsDriveRoot(const std::wstring& p) { return p.size() == 3 && p[1] == L':' && p[2] == L'\\'; }

std::wstring ParentOf(const std::wstring& p) {
    if (p.empty() || IsDriveRoot(p)) return L"";
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    if (p.rfind(L"\\\\", 0) == 0 && p.find(L'\\', 2) == pos) return L"";  // \\server\share
    std::wstring par = p.substr(0, pos);
    if (par.size() == 2 && par[1] == L':') par += L'\\';
    return par;
}

std::wstring LeafOf(const std::wstring& p) {
    if (IsDriveRoot(p)) return p.substr(0, 2);
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

std::wstring Normalize(std::wstring p) {
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    return p;
}

bool IsLocalFixed(const std::wstring& p) {
    if (p.size() < 3 || p[1] != L':') return false;
    wchar_t root[4] = {p[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_FIXED;
}

std::wstring ExtOf(const wchar_t* name, size_t len) {
    for (size_t i = len; i > 0; --i) {
        if (name[i - 1] == L'.') return Lower(std::wstring(name + i - 1, len - i + 1));
    }
    return L"";
}

// Drives: "Stuff (D:)". Computed once per letter.
std::unordered_map<wchar_t, std::wstring> g_driveLabels;
const std::wstring& DriveLabel(wchar_t letter) {
    auto it = g_driveLabels.find(letter);
    if (it != g_driveLabels.end()) return it->second;
    wchar_t root[4] = {letter, L':', L'\\', 0};
    UINT type = GetDriveTypeW(root);
    std::wstring label;
    wchar_t vol[MAX_PATH + 1] = {};
    if ((type == DRIVE_FIXED || type == DRIVE_REMOVABLE) &&
        GetVolumeInformationW(root, vol, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) && vol[0])
        label = vol;
    if (label.empty()) {
        label = type == DRIVE_FIXED      ? L"Local Disk"
                : type == DRIVE_REMOVABLE ? L"Removable Drive"
                : type == DRIVE_REMOTE    ? L"Network Drive"
                : type == DRIVE_CDROM     ? L"CD Drive"
                                          : L"Drive";
    }
    std::wstring s = label + L" (" + std::wstring(1, letter) + L":)";
    return g_driveLabels.emplace(letter, s).first->second;
}

// Disk space; usage is the used fraction (0..1), or -1 if not applicable.
struct DriveSpace {
    float usage = -1;
    uint64_t freeB = 0, totalB = 0;
    double at = -1e9;
};

// Re-read every 10 s per drive, or immediately with fresh.
const DriveSpace& DriveSpaceOf(wchar_t letter, bool fresh = false) {
    static std::unordered_map<wchar_t, DriveSpace> cache;
    DriveSpace& c = cache[letter];
    double now = NowMs();
    if (!fresh && now - c.at < 10000) return c;
    c.at = now;
    wchar_t root[4] = {letter, L':', L'\\', 0};
    UINT type = GetDriveTypeW(root);
    c.usage = -1;
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
        ULARGE_INTEGER freeB, totalB;
        if (GetDiskFreeSpaceExW(root, nullptr, &totalB, &freeB) && totalB.QuadPart > 0) {
            c.freeB = freeB.QuadPart;
            c.totalB = totalB.QuadPart;
            c.usage = static_cast<float>(1.0 - static_cast<double>(freeB.QuadPart) / static_cast<double>(totalB.QuadPart));
        }
    }
    return c;
}

// Drive under the mouse: its row shows the space and is re-read every second.
constexpr UINT_PTR kTimerDisk = 14;
constexpr UINT kDiskTickMs = 1000;
wchar_t g_diskHover = 0;
std::wstring g_diskShown;  // free space currently shown

// ---------------------------------------------------------------- favorites
struct Favorite {
    std::wstring name, path;
};
std::vector<Favorite> g_favs;

std::wstring FavFile() {
    wchar_t* appdata = nullptr;
    std::wstring dir = g_dataDir;
    if (dir.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\Jolt";
        CoTaskMemFree(appdata);
    }
    if (dir.empty()) return L"";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\favorites.txt";
}

std::wstring Known(const KNOWNFOLDERID& id) {
    wchar_t* p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
        s = p;
        CoTaskMemFree(p);
    }
    return s;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void RefreshConfigSig();

void SaveFavs() {
    std::wstring f = FavFile();
    if (f.empty()) return;
    {
        std::ofstream out(f, std::ios::binary);
        out << "# Jolt favorites: one line per folder, \"Name|Path\" or just the path.\n";
        for (const Favorite& v : g_favs) out << Utf8(v.name) << '|' << Utf8(v.path) << '\n';
    }
    RefreshConfigSig();  // we wrote it ourselves: no need to reload
}

void LoadFavs() {
    g_favs.clear();
    std::wstring f = FavFile();
    std::ifstream in(f, std::ios::binary);
    if (!in) {
        struct Def {
            const wchar_t* name;
            const KNOWNFOLDERID* id;
        } defs[] = {{L"Desktop", &FOLDERID_Desktop},
                    {L"Downloads", &FOLDERID_Downloads},
                    {L"Documents", &FOLDERID_Documents},
                    {L"Pictures", &FOLDERID_Pictures},
                    {L"User", &FOLDERID_Profile}};
        for (const Def& d : defs) {
            std::wstring p = Known(*d.id);
            if (!p.empty()) g_favs.push_back({d.name, Normalize(p)});
        }
        SaveFavs();
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::wstring w = Wide(line);
        size_t bar = w.find(L'|');
        Favorite v;
        if (bar == std::wstring::npos) {
            v.path = Normalize(w);
            v.name = LeafOf(v.path);
        } else {
            v.name = w.substr(0, bar);
            v.path = Normalize(w.substr(bar + 1));
        }
        if (v.name.empty()) v.name = v.path;
        g_favs.push_back(v);
    }
}

#include "config.inl"
#include "dirsize.inl"
#include "views.inl"

// ---------------------------------------------------------------- state
// A visible row: an item from some listing (the tab's folder, a subfolder
// expanded in the tree, or a folder from the subfolder search).
struct Row {
    const Listing* l;
    int e;
    int depth;
};

struct Tab {
    std::wstring path;
    std::shared_ptr<const Listing> list;
    bool loading = false;
    bool has = false;
    int pane = 0;                // 0 = left, 1 = right (split view)
    std::wstring pendingSelect;  // name (in the folder) or full path to select when it appears
    int sel = 0;                 // cursor
    int anchor = 0;              // start of the Shift range
    std::vector<uint8_t> selMask;  // parallel to rows
    std::wstring filter;           // fuzzy filter text ("" = no filter)
    std::vector<Row> rows;         // visible rows: flattened tree and, with a filter, sorted by score
    // tree: expanded folders (key -> path) and their listings
    std::unordered_map<std::wstring, std::wstring> expanded;
    std::unordered_map<std::wstring, std::shared_ptr<const Listing>> kids;
    // groups collapsed or expanded by hand on this visit (title -> collapsed); on coming back,
    // the view decides
    std::unordered_map<std::wstring, bool> fold;
    // subfolder search
    bool recursive = false, recDone = false;
    uint64_t recGen = 0;
    int recDirs = 0;
    std::vector<std::shared_ptr<const Listing>> rec;
    float scroll = 0;
    // grid view: one rect per row in content coordinates (see RowRect), rebuilt when the rows or the width change
    std::vector<D2D1_RECT_F> cells;
    uint64_t cellsKey = 0, rowsGen = 0;
    std::vector<std::wstring> back, fwd;
    float slide = 0;     // initial animation offset, in tab places
    double slideAt = 0;  // when it started
};

std::vector<std::unique_ptr<Tab>> g_tabs;
int g_cur = 0;              // active tab (the visible one in the active pane)
bool g_split = false;       // split view
int g_act = 0;              // active pane
int g_paneCur[2] = {0, 0};  // visible tab of each pane
float g_splitRatio = 0.5f;
Tab& T() { return *g_tabs[g_cur]; }
int PaneCount() { return g_split ? 2 : 1; }
Tab& PT(int p) { return p == g_act ? T() : *g_tabs[g_paneCur[p]]; }

struct Stats {
    double navStart = 0, navMs = -1, enumMs = 0, sortMs = 0;
    bool navPending = false, fromCache = false, show = true;
    std::wstring message;
} S;

std::unordered_map<std::wstring, std::shared_ptr<const Listing>> g_cache;
bool g_elevated = false;     // this process runs as administrator
bool g_adminWindow = false;  // "--elevated": the separate window for that (background.inl)
std::unordered_map<std::wstring, std::wstring> g_lastSel;
std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> g_expandMem;  // tree per folder
std::vector<std::wstring> g_recents;  // visited folders and files, most recent first
bool g_editing = false;
bool g_renaming = false;
int g_hoverRow = -1, g_hoverPane = 0, g_hoverHeader = -1;
std::unordered_set<std::wstring> g_cutKeys;  // "cut" paths (drawn dimmed until pasted)
bool g_ignoreClipUpdate = false;
int g_dropRow = -1, g_dropPane = 0, g_dropSide = -1, g_dropTab = -1;  // target highlighted during a drag

// Clickable areas, recomputed on every draw.
enum class HitKind { None, Tab, TabClose, TabNew, SideToggle, AppMenu, Back, Fwd, Up, Crumb, CrumbChev, PathEmpty, Side, Header, Divider, ViewBtn, FoldBtn, ColEdge, PreviewEdge, FolderNote };
struct Hit {
    D2D1_RECT_F r;
    HitKind kind;
    int idx;
};
std::vector<Hit> g_hits;
std::vector<std::wstring> g_crumbPaths;
bool g_crumbMenuOpen = false;  // the breadcrumb dropdown, and the folder it lists (menu.inl)
std::wstring g_crumbMenuDir;
D2D1_RECT_F g_crumbLast[2]{};  // each pane's last segment: the keyboard opens the dropdown there
std::vector<std::wstring> g_sidePaths;
Hit g_hover{{}, HitKind::None, -1};
bool g_sideVisible = true;
float SideW() { return g_sideVisible ? kSideW : 0.f; }
// Title bar buttons (non-client area): hover and pressed are tracked by HT* code.
D2D1_RECT_F g_capRects[3];
int g_capHover = 0, g_capPressed = 0;


void AddRecent(const std::wstring& p) {
    if (p.empty()) return;
    std::wstring k = Key(p);
    g_recents.erase(std::remove_if(g_recents.begin(), g_recents.end(), [&](const std::wstring& r) { return Key(r) == k; }),
                    g_recents.end());
    g_recents.insert(g_recents.begin(), p);
    if (g_recents.size() > 300) g_recents.resize(300);
}

#include "heat.inl"

// --- rows
int RowCount(const Tab& t) { return static_cast<int>(t.rows.size()); }
int Count() { return RowCount(T()); }
// Rows with e < 0 are group titles (grouped views): they can't be selected or opened.
bool IsHeader(const Tab& t, int row) { return t.rows[row].e < 0; }
const Entry& EntryAt(const Tab& t, int row) {
    static const Entry kNone{};
    return t.rows[row].e < 0 ? kNone : t.rows[row].l->entries[t.rows[row].e];
}
const wchar_t* RowNameP(const Tab& t, int row) { return t.rows[row].e < 0 ? L"" : t.rows[row].l->name(EntryAt(t, row)); }
std::wstring RowName(const Tab& t, int row) { return std::wstring(RowNameP(t, row), EntryAt(t, row).nameLen); }
const std::wstring& RowDir(const Tab& t, int row) { return t.rows[row].l->path; }
std::wstring RowPath(const Tab& t, int row) { return Normalize(Join(RowDir(t, row), RowNameP(t, row))); }
std::wstring RowKey(const Tab& t, int row) { return Key(RowPath(t, row)); }
bool FlatView(const Tab& t) { return !t.filter.empty() || t.recursive; }

#include "fuzzy.inl"

// Is group gi collapsed? What was done by hand on this visit, otherwise what the view says.
bool GroupFolded(const Tab& t, const Listing* l, size_t gi) {
    auto it = t.fold.find(l->groups[gi].second);
    if (it != t.fold.end()) return it->second;
    return gi < l->groupFolded.size() && l->groupFolded[gi];
}

void AddTree(const Tab& t, const Listing* l, int depth, std::vector<Row>& out) {
    const bool tree = !t.expanded.empty();
    const bool groups = depth == 0 && !l->groups.empty() && t.filter.empty();
    size_t gi = 0;
    bool folded = false;
    for (size_t i = 0; i < l->entries.size(); ++i) {
        if (groups && gi < l->groups.size() && l->groups[gi].first == i) {
            out.push_back({l, -1 - static_cast<int>(gi), 0});  // group title
            folded = GroupFolded(t, l, gi);
            ++gi;
        }
        if (folded) continue;
        out.push_back({l, static_cast<int>(i), depth});
        if (!tree) continue;
        const Entry& e = l->entries[i];
        if (!e.isDir()) continue;
        std::wstring k = Key(Normalize(Join(l->path, l->name(e))));
        if (!t.expanded.count(k)) continue;
        auto it = t.kids.find(k);
        if (it != t.kids.end() && it->second && !it->second->error) AddTree(t, it->second.get(), depth + 1, out);
    }
}

// Builds the visible rows: the tree (or what was found in subfolders) and, with a filter, only what
// matches, sorted by score like fzf.
// Content search ("content:word" in the filter): what grep.inl found, by lowercase full path.
struct GrepHit {
    std::wstring snippet;  // text around the match
    int at = -1, len = 0;  // the match inside the snippet
    bool match = false;
};
std::unordered_map<std::wstring, GrepHit> g_grepHits;
std::unordered_set<std::wstring> g_grepSeen;  // queued or done
std::wstring g_grepTerm;                      // lowercase; empty = no content search
extern std::atomic<int> g_grepPending;        // files queued but not read yet
void GrepStart(const std::wstring& term);     // grep.inl
void GrepStop();
void GrepWant(const std::wstring& full);

// What the typed filter means. Plain words are the fuzzy pattern; the other terms narrow the list:
//   .png  *.png  ext:png,jpg        extension (several: any of them)
//   >10mb  <500kb  >2gb             size (a bare number is MB); folders have no size and are left out
//   type:folder  type:file          kind
//   newer:7d  older:2w  modified:today   date (d h w m y; "today" since midnight)
//   !tmp  !.bak  !*.log             exclude: name contains, extension, pattern
//   report*.pdf  ?.txt              wildcard pattern on the name
//   content:word  content:"two words"   files whose text contains it (read in the background)
//   tag:urgent  !tag:done           items with (without) a tag
struct FilterSpec {
    std::wstring fuzzy;  // lowercase
    std::wstring content;  // lowercase phrase the file must contain
    std::vector<std::wstring> exts, notExts, notNames, globs, notGlobs;
    std::vector<std::wstring> tags, notTags;  // tag:urgent / !tag:done
    uint64_t minSize = 0, maxSize = UINT64_MAX;
    bool sized = false;
    int64_t newerThan = 0, olderThan = 0;  // FILETIME; 0 = no limit
    int kind = 0;                          // 1 folders, 2 files
};

bool ParseFilterSize(const std::wstring& s, uint64_t& out) {
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || v < 0) return false;
    std::wstring u = Trim(end);
    double mul = u.empty() || u == L"m" || u == L"mb" ? 1048576.0 : u == L"b" ? 1 : u == L"k" || u == L"kb" ? 1024.0 : u == L"g" || u == L"gb" ? 1073741824.0 : u == L"t" || u == L"tb" ? 1099511627776.0 : 0;
    if (mul == 0) return false;
    out = static_cast<uint64_t>(v * mul);
    return true;
}

// "7d", "12h", "2w", "3m", "1y" or "today" -> the FILETIME where that period starts.
bool ParseFilterAge(const std::wstring& s, int64_t& out) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const int64_t nowT = (static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (s == L"today") {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        lt.wHour = lt.wMinute = lt.wSecond = lt.wMilliseconds = 0;
        FILETIME local, utc;
        if (!SystemTimeToFileTime(&lt, &local) || !LocalFileTimeToFileTime(&local, &utc)) return false;
        out = (static_cast<int64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
        return true;
    }
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || v < 0) return false;
    std::wstring u = Trim(end);
    const double day = 864000000000.0;  // 100 ns units
    double span = u.empty() || u == L"d" ? day : u == L"h" ? day / 24 : u == L"w" ? day * 7 : u == L"m" ? day * 30 : u == L"y" ? day * 365 : 0;
    if (span == 0) return false;
    out = nowT - static_cast<int64_t>(v * span);
    return true;
}

FilterSpec ParseFilter(const std::wstring& filter) {
    FilterSpec s;
    const std::wstring f = Lower(filter);
    size_t pos = 0;
    while (pos < f.size()) {
        size_t sp = f.find(L' ', pos);
        std::wstring tok = f.substr(pos, sp == std::wstring::npos ? std::wstring::npos : sp - pos);
        pos = sp == std::wstring::npos ? f.size() : sp + 1;
        if (tok.empty()) continue;
        if (tok.rfind(L"content:", 0) == 0 || tok.rfind(L"text:", 0) == 0) {
            std::wstring v = tok.substr(tok.find(L':') + 1);
            if (!v.empty() && v[0] == L'"') {  // a quoted phrase runs until the closing quote
                v.erase(0, 1);
                while (v.find(L'"') == std::wstring::npos && pos < f.size()) {
                    size_t sp2 = f.find(L' ', pos);
                    v += L" " + f.substr(pos, sp2 == std::wstring::npos ? std::wstring::npos : sp2 - pos);
                    pos = sp2 == std::wstring::npos ? f.size() : sp2 + 1;
                }
                size_t q = v.find(L'"');
                if (q != std::wstring::npos) v.resize(q);
            }
            s.content = v;
            continue;
        }
        const bool neg = tok[0] == L'!';
        if (neg) tok.erase(0, 1);
        if (tok.empty()) continue;
        if (tok.rfind(L"tag:", 0) == 0) {
            for (const std::wstring& x : SplitList(tok.substr(4))) (neg ? s.notTags : s.tags).push_back(x);
            continue;
        }
        auto ext = [&](std::wstring e) {
            if (e.empty()) return;
            if (e[0] != L'.') e.insert(0, 1, L'.');
            (neg ? s.notExts : s.exts).push_back(e);
        };
        const bool wild = tok.find_first_of(L"*?") != std::wstring::npos;
        if (tok.rfind(L"ext:", 0) == 0) {
            for (const std::wstring& e : SplitList(tok.substr(4))) ext(e);
        } else if (tok.size() > 1 && tok[0] == L'.' && tok.find(L'.', 1) == std::wstring::npos && !wild) {
            ext(tok);
        } else if (tok.size() > 2 && tok.rfind(L"*.", 0) == 0 && tok.find_first_of(L"*?.", 2) == std::wstring::npos) {
            ext(tok.substr(1));
        } else if (tok.rfind(L"type:", 0) == 0) {
            const std::wstring v = tok.substr(5);
            s.kind = v == L"folder" || v == L"folders" || v == L"dir" || v == L"dirs" ? 1 : v == L"file" || v == L"files" ? 2 : 0;
        } else if ((tok[0] == L'>' || tok[0] == L'<') && tok.size() > 1) {
            uint64_t bytes;
            if (ParseFilterSize(tok.substr(1), bytes)) {
                (tok[0] == L'>' ? s.minSize : s.maxSize) = bytes;
                s.sized = true;
            } else {
                s.fuzzy += (s.fuzzy.empty() ? L"" : L" ") + tok;
            }
        } else if (tok.rfind(L"newer:", 0) == 0 || tok.rfind(L"modified:", 0) == 0 || tok.rfind(L"older:", 0) == 0) {
            int64_t when;
            const bool older = tok[0] == L'o';
            if (ParseFilterAge(tok.substr(tok.find(L':') + 1), when)) (older ? s.olderThan : s.newerThan) = when;
        } else if (wild) {
            (neg ? s.notGlobs : s.globs).push_back(tok);
        } else if (neg) {
            s.notNames.push_back(tok);
        } else {
            s.fuzzy += (s.fuzzy.empty() ? L"" : L" ") + tok;
        }
    }
    return s;
}

// * and ? over a lowercase name.
bool GlobMatch(const wchar_t* name, size_t n, const std::wstring& pat) {
    size_t i = 0, p = 0, star = std::wstring::npos, mark = 0;
    while (i < n) {
        if (p < pat.size() && (pat[p] == L'?' || pat[p] == FzLower(name[i]))) {
            ++i;
            ++p;
        } else if (p < pat.size() && pat[p] == L'*') {
            star = p++;
            mark = i;
        } else if (star != std::wstring::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == L'*') ++p;
    return p == pat.size();
}

// Does the entry pass the typed filter? score and pos come from the fuzzy words.
bool FilterPass(const FilterSpec& s, const Entry& e, const wchar_t* name, size_t n, int& score, std::vector<int>* pos) {
    if (s.kind == 1 && !e.isDir()) return false;
    if (s.kind == 2 && e.isDir()) return false;
    if (s.sized && (e.isDir() || e.size < s.minSize || e.size > s.maxSize)) return false;
    if (s.newerThan && e.mtime < s.newerThan) return false;
    if (s.olderThan && e.mtime > s.olderThan) return false;
    if (!s.exts.empty() || !s.notExts.empty()) {
        const std::wstring ext = ExtOf(name, n);
        if (!s.exts.empty() && std::find(s.exts.begin(), s.exts.end(), ext) == s.exts.end()) return false;
        if (std::find(s.notExts.begin(), s.notExts.end(), ext) != s.notExts.end()) return false;
    }
    if (!s.notNames.empty()) {
        const std::wstring low = Lower(std::wstring(name, n));
        for (const std::wstring& x : s.notNames)
            if (low.find(x) != std::wstring::npos) return false;
    }
    for (const std::wstring& g : s.globs)
        if (!GlobMatch(name, n, g)) return false;
    for (const std::wstring& g : s.notGlobs)
        if (GlobMatch(name, n, g)) return false;
    return FuzzyMatch(name, n, s.fuzzy, score, pos);
}

#include "tags.inl"

void BuildRows(Tab& t) {
    t.rows.clear();
    ++t.rowsGen;
    if (!t.list) return;
    if (t.recursive) {
        const bool hide = !g_cfg.showHidden;
        for (const auto& l : t.rec) {
            const int depth = Key(l->path) == Key(t.path) ? 0 : 1;
            for (size_t i = 0; i < l->entries.size(); ++i) {
                if (hide && (l->entries[i].attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                t.rows.push_back({l.get(), static_cast<int>(i), depth});
            }
        }
    } else {
        AddTree(t, t.list.get(), 0, t.rows);
    }
    if (t.filter.empty()) {
        if (&t == &T()) GrepStop();
        return;
    }
    const FilterSpec spec = ParseFilter(t.filter);
    if (&t == &T() && spec.content != g_grepTerm) {
        if (spec.content.empty()) GrepStop();
        else GrepStart(spec.content);
    }
    std::vector<std::pair<int, int>> hits;  // (score, row)
    for (size_t i = 0; i < t.rows.size(); ++i) {
        const Row& r = t.rows[i];
        if (r.e < 0) continue;
        const Entry& e = r.l->entries[r.e];
        int sc;
        if (!FilterPass(spec, e, r.l->name(e), e.nameLen, sc, nullptr)) continue;
        if (!spec.content.empty()) {  // only files whose text has it; the unread ones appear as they are read
            if (e.isDir() || r.l->path.empty()) continue;
            const std::wstring full = Join(r.l->path, r.l->name(e));
            auto h = g_grepHits.find(Key(full));
            if (h == g_grepHits.end()) {
                if (!g_grepSeen.count(Key(full))) GrepWant(full);
                continue;
            }
            if (!h->second.match) continue;
        }
        if (!spec.tags.empty() || !spec.notTags.empty()) {
            if (r.l->path.empty()) continue;
            const std::wstring full = Join(r.l->path, r.l->name(e));
            bool ok = true;
            for (const std::wstring& tg : spec.tags) ok &= HasTag(full, tg);
            for (const std::wstring& tg : spec.notTags) ok &= !HasTag(full, tg);
            if (!ok) continue;
        }
        hits.push_back({sc, static_cast<int>(i)});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Row> out;
    out.reserve(hits.size());
    for (auto& h : hits) out.push_back(t.rows[h.second]);
    t.rows.swap(out);
}

// Row of a name in the tab's folder, or of a full path (if it contains "\").
int FindRow(const Tab& t, const std::wstring& s) {
    if (s.empty()) return -1;
    const int n = RowCount(t);
    if (s.find(L'\\') == std::wstring::npos) {
        for (int i = 0; i < n; ++i)
            if (t.rows[i].l == t.list.get() && _wcsicmp(RowNameP(t, i), s.c_str()) == 0) return i;
        return -1;
    }
    std::wstring p = Normalize(s), leaf = LeafOf(p), dirKey = Key(ParentOf(p));
    for (int i = 0; i < n; ++i)
        if (_wcsicmp(RowNameP(t, i), leaf.c_str()) == 0 && Key(RowDir(t, i)) == dirKey) return i;
    return -1;
}

int FindName(const Listing& l, const std::wstring& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < l.entries.size(); ++i)
        if (_wcsicmp(l.name(l.entries[i]), name.c_str()) == 0) return static_cast<int>(i);
    return -1;
}

D2D1_SIZE_F ClientDip() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return {rc.right * 96.f / g_dpi, rc.bottom * 96.f / g_dpi};
}
float ListTop() { return kTabH + kToolH + kHeadH; }
float ListBottom() { return ClientDip().height - kStatus; }
float ListHeight() { return std::max(0.f, ListBottom() - ListTop()); }

float PreviewW() {
    if (!g_preview) return 0.f;
    float W = ClientDip().width;
    return std::clamp(g_previewW, kPreviewMinW, std::max(kPreviewMinW, W * 0.6f));
}

// Horizontal bounds of each pane: [L, R) for header and list; [TL, TR) for the path bar and
// the status bar (which extend over the sidebar and the preview).
struct PaneX {
    float L, R, TL, TR;
};
PaneX PaneGeo(int p) {
    const float W = ClientDip().width, side = SideW(), right = W - PreviewW();
    if (!g_split) return {side, right, 0, W};
    const float mid = std::round(side + (right - side) * g_splitRatio);
    if (p == 0) return {side, mid, 0, mid};
    return {mid, right, mid, W};
}

// Pane under a point (path bar, list or status), or -1.
int PaneAt(float x, float y) {
    if (y < kTabH) return -1;
    const bool band = y < kTabH + kToolH || y >= ListBottom();
    for (int p = 0; p < PaneCount(); ++p) {
        PaneX g = PaneGeo(p);
        if (band ? (x >= g.TL && x < g.TR) : (x >= g.L && x < g.R)) return p;
    }
    return -1;
}

// ---- row geometry: the list (one row per item) or the grid of thumbnails (cells along lines, with the
// group titles on full-width rows). Rects are in content coordinates: x on screen, y from the top of
// the content; the scroll is subtracted when drawing.
constexpr int kGridSizes[] = {0, 80, 112, 144, 192, 256};  // the view scale: the list, then bigger cells
constexpr float kCellPadX = 20.f, kCellText = 40.f;      // around the image; the two lines of name below

int LayoutOf(const Tab& t) { return t.path.empty() || t.recursive ? 0 : ViewFor(t.path)->layout; }

struct Geo {
    bool grid = false;
    int size = 0, cols = 1;
    float cellW = 0, cellH = 0, x0 = 0;
};
Geo GeoFor(const Tab& t) {
    Geo g;
    g.size = LayoutOf(t);
    if (!g.size) return g;
    const PaneX p = PaneGeo(t.pane);
    g.grid = true;
    g.cellW = g.size + kCellPadX;
    g.cellH = g.size + kCellText + 8;
    const float room = p.R - p.L - 16;
    g.cols = std::max(1, static_cast<int>(room / g.cellW));
    g.x0 = p.L + 8 + std::floor((room - g.cols * g.cellW) / 2);
    return g;
}

void EnsureGeo(Tab& t) {
    const Geo g = GeoFor(t);
    if (!g.grid) {
        t.cells.clear();
        t.cellsKey = 0;
        return;
    }
    const uint64_t key = (t.rowsGen << 24) ^ (static_cast<uint64_t>(g.cols) << 16) ^ static_cast<uint64_t>(g.size) ^
                         (static_cast<uint64_t>(std::lround(g.x0)) << 44) ^ (static_cast<uint64_t>(std::lround(kRow)) << 56);
    if (key == t.cellsKey && t.cells.size() == t.rows.size()) return;
    t.cellsKey = key;
    t.cells.resize(t.rows.size());
    float y = 4;
    int col = 0;
    for (size_t i = 0; i < t.rows.size(); ++i) {
        if (t.rows[i].e < 0) {  // a group title takes a whole line
            if (col) {
                y += g.cellH;
                col = 0;
            }
            t.cells[i] = D2D1::RectF(g.x0 - 8, y, g.x0 + g.cols * g.cellW + 8, y + kRow);
            y += kRow;
        } else {
            t.cells[i] = D2D1::RectF(g.x0 + col * g.cellW, y, g.x0 + (col + 1) * g.cellW, y + g.cellH);
            if (++col == g.cols) {
                col = 0;
                y += g.cellH;
            }
        }
    }
}

D2D1_RECT_F RowRect(Tab& t, int i) {
    EnsureGeo(t);
    if (!t.cells.empty() && i >= 0 && i < static_cast<int>(t.cells.size())) return t.cells[i];
    const PaneX p = PaneGeo(t.pane);
    return D2D1::RectF(p.L, i * kRow, p.R, (i + 1) * kRow);
}

float ContentHeight(Tab& t) {
    EnsureGeo(t);
    const int n = RowCount(t);
    if (t.cells.empty()) return n * kRow;
    return n ? t.cells[n - 1].bottom + 8 : 0;
}

// The row under a point (x on screen, y in the content), or -1.
int RowAtContent(Tab& t, float x, float y) {
    EnsureGeo(t);
    const int n = RowCount(t);
    if (t.cells.empty()) {
        const int r = static_cast<int>(std::floor(y / kRow));
        return r >= 0 && r < n ? r : -1;
    }
    // the first row whose line reaches below y, then along that line
    int i = static_cast<int>(std::partition_point(t.cells.begin(), t.cells.end(), [&](const D2D1_RECT_F& r) { return r.bottom <= y; }) - t.cells.begin());
    for (; i < n && t.cells[i].top <= y; ++i)
        if (x >= t.cells[i].left && x < t.cells[i].right) return i;
    return -1;
}

// What the arrows and Page Up/Down move by.
int RowsPerLine(const Tab& t) { return GeoFor(t).cols; }
int RowsPerPage(const Tab& t) {
    const Geo g = GeoFor(t);
    return g.grid ? std::max(1, static_cast<int>(ListHeight() / g.cellH)) * g.cols : std::max(1, static_cast<int>(ListHeight() / kRow) - 1);
}

void ClampScroll(Tab& t) {
    float maxS = std::max(0.f, ContentHeight(t) - ListHeight());
    t.scroll = std::clamp(t.scroll, 0.f, maxS);
}

void EnsureVisible(Tab& t) {
    const D2D1_RECT_F r = RowRect(t, t.sel);
    const float h = ListHeight();
    if (r.top < t.scroll) t.scroll = r.top;
    if (r.bottom > t.scroll + h) t.scroll = r.bottom - h;
    ClampScroll(t);
}

std::wstring SelectedName(const Tab& t) {
    if (t.sel < 0 || t.sel >= RowCount(t)) return L"";
    return RowName(t, t.sel);
}

void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }

// --- selection
bool IsSel(const Tab& t, int i) { return i >= 0 && i < static_cast<int>(t.selMask.size()) && t.selMask[i]; }

int SelCount(const Tab& t) {
    int n = 0;
    for (uint8_t v : t.selMask) n += v;
    return n;
}

void SelectOnly(Tab& t, int i) {
    std::fill(t.selMask.begin(), t.selMask.end(), uint8_t{0});
    if (i >= 0 && i < static_cast<int>(t.selMask.size())) t.selMask[i] = 1;
    t.sel = t.anchor = i;
}

void SelectRange(Tab& t, int to) {
    std::fill(t.selMask.begin(), t.selMask.end(), uint8_t{0});
    int n = static_cast<int>(t.selMask.size());
    if (n == 0) return;
    int a = std::clamp(t.anchor, 0, n - 1), b = std::clamp(to, 0, n - 1);
    for (int i = std::min(a, b); i <= std::max(a, b); ++i) t.selMask[i] = !IsHeader(t, i);
    t.sel = b;
}

void SelectToggle(Tab& t, int i) {
    if (i < 0 || i >= static_cast<int>(t.selMask.size())) return;
    t.selMask[i] = !t.selMask[i];
    t.sel = t.anchor = i;
}

void SelectAll(Tab& t) {
    for (int i = 0; i < static_cast<int>(t.selMask.size()); ++i) t.selMask[i] = !IsHeader(t, i);
}

// If i lands on a group title, the nearest content row in that direction (or the other one).
int SkipHeader(const Tab& t, int i, int dir) {
    const int n = RowCount(t);
    if (n == 0) return 0;
    i = std::clamp(i, 0, n - 1);
    if (!IsHeader(t, i)) return i;
    for (int j = i + dir; j >= 0 && j < n; j += dir)
        if (!IsHeader(t, j)) return j;
    for (int j = i - dir; j >= 0 && j < n; j -= dir)
        if (!IsHeader(t, j)) return j;
    return i;
}

// Folder of the cursor row (in the tree or search it may not be the tab's).
std::wstring SelDir(const Tab& t) { return t.sel >= 0 && t.sel < RowCount(t) ? RowDir(t, t.sel) : t.path; }

// Selected names in the cursor's folder, ready for the Shell ("C:" -> "C:\").
std::vector<std::wstring> SelectedNames(const Tab& t) {
    std::vector<std::wstring> out;
    if (!t.list || t.sel < 0 || t.sel >= RowCount(t)) return out;
    const Listing* L = t.rows[t.sel].l;
    for (int i = 0; i < RowCount(t); ++i) {
        if (!IsSel(t, i) || t.rows[i].l != L) continue;
        std::wstring n = RowName(t, i);
        if (L->path.empty()) n += L"\\";
        out.push_back(n);
    }
    return out;
}

// Full paths of everything selected (from any folder).
std::vector<std::wstring> SelectedFullPaths(const Tab& t) {
    std::vector<std::wstring> out;
    if (t.path.empty() && !t.recursive) return out;  // drives: not copied
    for (int i = 0; i < RowCount(t); ++i)
        if (IsSel(t, i)) out.push_back(RowPath(t, i));
    return out;
}

// Listing ready to show: with its folder's view applied (filters, sort, groups) and without
// hidden items if so requested. Stored with the listing: while nothing changes, it isn't recomputed.
std::shared_ptr<const Listing> Prepared(std::shared_ptr<const Listing> l) {
    if (!l || l->error || l->isView) return l;
    if (l->view && l->viewSig == g_viewEpoch) return l->view;
    auto out = ApplyView(*l, *ViewFor(l->path));
    l->view = out;
    l->viewSig = g_viewEpoch;
    return out;
}

// Cursor and selection position, by path: survives reloads, the tree and the filter.
struct Place {
    std::wstring cur;
    int row = -1;
    std::vector<std::wstring> sel;
};
Place SavePlace(const Tab& t) {
    Place p;
    if (t.sel >= 0 && t.sel < RowCount(t)) {
        p.cur = RowPath(t, t.sel);
        p.row = t.sel;
    }
    if (SelCount(t) > 1)
        for (int i = 0; i < RowCount(t); ++i)
            if (IsSel(t, i)) p.sel.push_back(RowKey(t, i));
    return p;
}

// Rebuilds the rows and places the cursor: pendingSelect, or the saved place (keep), or prefer.
void Rebuild(Tab& t, const Place* keep, const std::wstring& prefer) {
    BuildRows(t);
    const int n = RowCount(t);
    int idx = -1;
    if (!t.pendingSelect.empty()) {
        idx = FindRow(t, t.pendingSelect);
        if (idx >= 0 || !keep) t.pendingSelect.clear();
    }
    if (idx < 0 && keep && !keep->cur.empty()) {
        // if the item is gone (e.g. its folder was collapsed), the nearest visible ancestor
        for (std::wstring p = keep->cur; idx < 0 && !p.empty() && Key(p) != Key(t.path); p = ParentOf(p)) {
            idx = FindRow(t, p);
            if (IsDriveRoot(p)) break;
        }
    }
    if (idx < 0 && !keep) idx = FindRow(t, prefer);
    t.selMask.assign(n, 0);
    if (keep) {
        int old = t.sel;
        t.sel = idx >= 0 ? idx : std::min(t.sel, n - 1);
        if (t.sel < 0) t.sel = 0;
        if (idx >= 0 && keep->row >= 0) t.scroll += RowRect(t, t.sel).top - RowRect(t, std::min(keep->row, std::max(0, n - 1))).top;  // so it doesn't jump on screen
        (void)old;
        ClampScroll(t);
    } else {
        t.sel = idx >= 0 ? idx : 0;
        const D2D1_RECT_F r = RowRect(t, t.sel);
        t.scroll = r.top - ListHeight() / 2 + (r.bottom - r.top) / 2;  // the selection ends up centered
        ClampScroll(t);
    }
    if (n > 0) t.sel = SkipHeader(t, t.sel, 1);
    t.anchor = t.sel;
    if (n > 0) t.selMask[t.sel] = 1;
    if (keep && keep->sel.size() > 1) {
        std::unordered_set<std::wstring> ks(keep->sel.begin(), keep->sel.end());
        for (int i = 0; i < n; ++i)
            if (ks.count(RowKey(t, i))) t.selMask[i] = 1;
    }
}

void ShowListing(Tab& t, std::shared_ptr<const Listing> l, const std::wstring& prefer, bool keepPlace) {
    Place p;
    if (keepPlace) p = SavePlace(t);  // before releasing the old listing: the rows point to it
    t.list = Prepared(std::move(l));
    t.loading = false;
    Rebuild(t, keepPlace ? &p : nullptr, prefer);
}

std::wstring TabTitle(const Tab& t) {
    if (t.path.empty()) return L"This PC";
    if (IsDriveRoot(t.path)) return DriveLabel(t.path[0]);
    return LeafOf(t.path);
}

void UpdateTitle() { SetWindowTextW(g_hwnd, (TabTitle(T()) + (g_elevated ? L" — Jolt (Administrator)" : L" — Jolt")).c_str()); }

// Watches the visible folders (both in split view); with the tree open, the subfolders too.
void SetWatchAll() {
    std::vector<std::pair<std::wstring, bool>> v;
    for (int p = 0; p < PaneCount(); ++p) {
        Tab& t = PT(p);
        if (!t.has || t.path.empty()) continue;
        bool dup = false;
        for (auto& w : v) dup |= Key(w.first) == Key(t.path);
        if (!dup) v.push_back({t.path, !t.expanded.empty()});
    }
    SetWatchList(v);
}

void CancelRename();
void BeginRename();
void UndoRecordRename(const std::wstring& from, const std::wstring& to);
void UndoRecordCreate(const std::wstring& path);
void CrumbMenuListed(const Listing& l);  // menu.inl
void RememberListing(const std::wstring& k, const std::shared_ptr<const Listing>& l);  // access.inl
bool CantEnter(const std::wstring& path);
void TreeFollow(Tab& t, const std::wstring& from, const std::wstring& to);
void TouchSession();
void StopRecursive(Tab& t, bool rebuild);
std::wstring g_renameAfterSelect;  // new folder or file: start renaming when it appears in the list

// Loads (or takes from memory) the listing of an expanded subfolder.
void LoadKid(Tab& t, const std::wstring& k, const std::wstring& path) {
    auto it = g_cache.find(k);
    if (it != g_cache.end()) {
        t.kids[k] = Prepared(it->second);
        Enqueue(path, JobKind::Revalidate);
        return;
    }
    auto j = Enqueue(path, JobKind::Load);
    if (WaitForSingleObject(j->done, 30) == WAIT_OBJECT_0 && j->result && !j->taken.exchange(true)) {
        RememberListing(k, j->result);
        t.kids[k] = Prepared(j->result);
    }
}

void Navigate(Tab& t, const std::wstring& path, bool pushHistory) {
    bool current = &t == &T();
    if (current) {
        CancelRename();
        S.navStart = NowMs();
        S.navPending = true;
        S.message.clear();
    }
    if (t.has) {
        if (t.list && t.sel >= 0 && t.sel < RowCount(t) && !t.recursive)
            g_lastSel[Key(t.path)] = t.rows[t.sel].depth == 0 ? SelectedName(t) : RowPath(t, t.sel);
        if (!t.expanded.empty())
            g_expandMem[Key(t.path)] = t.expanded;
        else
            g_expandMem.erase(Key(t.path));
        if (pushHistory && Key(path) != Key(t.path)) {
            t.back.push_back(t.path);
            t.fwd.clear();
        }
    }
    StopRecursive(t, false);
    if (current && (!t.has || Key(t.path) != Key(path))) HeatMark(path);
    t.has = true;
    t.path = path;
    t.filter.clear();
    t.rows.clear();
    t.selMask.clear();
    t.expanded.clear();
    t.kids.clear();
    t.fold.clear();  // groups start the way the view says
    AddRecent(path);
    ++g_navGen;
    auto ls = g_lastSel.find(Key(path));
    t.pendingSelect = ls != g_lastSel.end() ? ls->second : L"";
    // the tree left open last time in this folder
    auto em = g_expandMem.find(Key(path));
    if (em != g_expandMem.end()) {
        t.expanded = em->second;
        for (auto& [k, p] : t.expanded) LoadKid(t, k, p);
    }

    auto it = g_cache.find(Key(path));
    if (it != g_cache.end()) {
        // In memory: show it now, and check in the background in case it changed.
        ShowListing(t, it->second, t.pendingSelect, false);
        if (current) S.fromCache = true;
        Enqueue(path, JobKind::Revalidate);
    } else {
        auto j = Enqueue(path, JobKind::Load);
        // For local folders it's almost always ready within 30 ms: this avoids
        // an intermediate "loading" frame. If it takes longer (network), we don't block.
        if (WaitForSingleObject(j->done, 30) == WAIT_OBJECT_0 && j->result && !j->taken.exchange(true)) {
            RememberListing(Key(path), j->result);
            ShowListing(t, j->result, t.pendingSelect, false);
            if (current) {
                S.fromCache = false;
                S.enumMs = j->enumMs;
                S.sortMs = j->sortMs;
            }
        } else {
            t.list = nullptr;
            t.loading = true;
            t.sel = 0;
            t.scroll = 0;
        }
    }
    bool visible = current || (g_split && &t == &PT(1 - g_act));
    if (visible) {
        SetWatchAll();
        g_hoverRow = -1;
        Invalidate();
    }
    if (current) {
        UpdateTitle();
        UpdateWindow(g_hwnd);  // draw now, without waiting for the message queue
    }
    TouchSession();
}

void ApplyJob(const std::shared_ptr<Job>& j) {
    if (j->taken.exchange(true) || !j->result) return;
    auto l = j->result;
    const std::wstring k = Key(l->path);
    RememberListing(k, l);
    CrumbMenuListed(*l);
    if (j->kind == JobKind::Prefetch) {
        if (g_preview) Invalidate();  // a folder's preview may be waiting for it
        return;
    }
    bool changed = g_preview;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.expanded.count(k) && !t.recursive) {
            auto pl = Prepared(l);
            auto it = t.kids.find(k);
            if (it == t.kids.end() || !it->second || !SameContent(*it->second, *pl)) {
                Place p = SavePlace(t);
                t.kids[k] = pl;
                Rebuild(t, &p, L"");
                changed = true;
            }
        }
        if (k != Key(t.path)) continue;
        if (t.loading || !t.list) {
            ShowListing(t, l, t.pendingSelect, false);
            if (&t == &T()) {
                S.fromCache = false;
                S.enumMs = j->enumMs;
                S.sortMs = j->sortMs;
            }
            changed = true;
        } else {
            auto pl = Prepared(l);
            if (!SameContent(*t.list, *pl)) {
                ShowListing(t, pl, L"", true);
                changed = true;
            }
        }
        if (&t == &T() && !g_renameAfterSelect.empty() && SelectedName(t) == g_renameAfterSelect) {
            g_renameAfterSelect.clear();
            EnsureVisible(t);
            BeginRename();
        }
    }
    if (changed) Invalidate();
}

void SelectTab(int i) {
    CancelRename();
    g_cur = std::clamp(i, 0, static_cast<int>(g_tabs.size()) - 1);
    g_act = g_split ? T().pane : 0;
    g_paneCur[g_act] = g_cur;
    SetWatchAll();
    UpdateTitle();
    g_hoverRow = -1;
    if (T().has) Enqueue(T().path, JobKind::Revalidate);
    Invalidate();
    TouchSession();
}

void ActivatePane(int p) {
    if (!g_split || p < 0 || p > 1 || p == g_act) return;
    CancelRename();
    g_paneCur[g_act] = g_cur;
    g_act = p;
    g_cur = g_paneCur[p];
    UpdateTitle();
    Invalidate();
    TouchSession();
}

void NewTab(const std::wstring& path, bool select = true, int pane = -1) {
    g_tabs.push_back(std::make_unique<Tab>());
    Tab& t = *g_tabs.back();
    t.pane = g_split ? (pane < 0 ? g_act : pane) : 0;
    if (select) {
        if (g_split) g_paneCur[g_act] = g_cur;
        g_act = t.pane;
        g_cur = static_cast<int>(g_tabs.size()) - 1;
        g_paneCur[g_act] = g_cur;
        Navigate(t, path, false);
        UpdateTitle();
    } else {
        Navigate(t, path, false);
        Invalidate();
    }
}

// Tab of a pane near position i (the one to the right, otherwise the one to the left).
int NeighborInPane(int pane, int i) {
    int prev = -1;
    for (int k = 0; k < static_cast<int>(g_tabs.size()); ++k) {
        if (g_tabs[k]->pane != pane) continue;
        if (k >= i) return k;
        prev = k;
    }
    return prev;
}

void SetSplit(bool on);
bool FolderExists(const std::wstring& p);

// Closed tabs, to reopen them where they were and with their history.
struct ClosedTab {
    std::wstring path;
    int pane = 0, index = 0;
    std::vector<std::wstring> back, fwd;
};
std::vector<ClosedTab> g_closedTabs;

void CloseTab(int i) {
    if (g_tabs.size() == 1) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    CancelRename();
    StopRecursive(*g_tabs[i], false);
    g_closedTabs.push_back({g_tabs[i]->path, g_tabs[i]->pane, i, g_tabs[i]->back, g_tabs[i]->fwd});
    if (g_closedTabs.size() > 30) g_closedTabs.erase(g_closedTabs.begin());
    g_paneCur[g_act] = g_cur;
    const int pane = g_tabs[i]->pane;
    const bool wasVisible = g_paneCur[pane] == i;
    g_tabs.erase(g_tabs.begin() + i);
    for (int& c : g_paneCur)
        if (c > i) --c;
    if (g_split && NeighborInPane(pane, 0) < 0) {
        // the last tab of a pane was closed: back to a single pane
        int other = 1 - pane;
        g_cur = g_paneCur[other];
        g_split = false;
        for (auto& t : g_tabs) t->pane = 0;
        g_act = 0;
        g_paneCur[0] = g_cur;
        SelectTab(g_cur);
        return;
    }
    if (wasVisible) g_paneCur[pane] = NeighborInPane(pane, i);
    SelectTab(g_paneCur[g_act]);
}

void ReopenTab() {
    // if the folder no longer exists, move on to the previous one
    while (!g_closedTabs.empty()) {
        ClosedTab c = std::move(g_closedTabs.back());
        g_closedTabs.pop_back();
        if (!c.path.empty() && !FolderExists(c.path)) continue;
        NewTab(c.path, true, c.pane);
        T().back = std::move(c.back);
        T().fwd = std::move(c.fwd);
        // NewTab leaves it at the end: put it back in its place
        const int last = static_cast<int>(g_tabs.size()) - 1, idx = std::clamp(c.index, 0, last);
        if (idx < last) {
            std::rotate(g_tabs.begin() + idx, g_tabs.begin() + last, g_tabs.end());
            for (int& p : g_paneCur)
                if (p >= idx) ++p;
            g_cur = g_paneCur[g_act] = idx;
        }
        Invalidate();
        TouchSession();
        return;
    }
    S.message = L"No closed tabs";
    Invalidate();
}

// Tabs of the active pane, in order.
std::vector<int> ActiveTabs() {
    std::vector<int> idx;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        if (!g_split || g_tabs[i]->pane == g_act) idx.push_back(i);
    return idx;
}

// Moves to the next (d = 1) or previous (d = -1) tab of the active pane, wrapping around.
void CycleTab(int d) {
    const std::vector<int> idx = ActiveTabs();
    const int n = static_cast<int>(idx.size());
    const int pos = static_cast<int>(std::find(idx.begin(), idx.end(), g_cur) - idx.begin());
    if (n > 1) SelectTab(idx[(pos + d + n) % n]);
}

// How far a tab still is from its place, in tab places. Slows down at the end.
constexpr double kTabSlideMs = 150;
float TabSlide(Tab& t) {
    if (t.slide == 0) return 0;
    const double p = (NowMs() - t.slideAt) / kTabSlideMs;
    if (p >= 1) return t.slide = 0;
    const float left = static_cast<float>(1 - p);
    return t.slide * left * left * left;
}

void TabSlideBy(Tab& t, float places) {
    t.slide = TabSlide(t) + places;
    t.slideAt = NowMs();
}

// Moves the active tab one place within its pane (d = 1, to the right); at the ends it stays put.
// The change is immediate: the animation only draws the two tabs arriving at their places.
void MoveTab(int d) {
    const std::vector<int> idx = ActiveTabs();
    const int n = static_cast<int>(idx.size());
    const int to = static_cast<int>(std::find(idx.begin(), idx.end(), g_cur) - idx.begin()) + d;
    if (to < 0 || to >= n) return;
    std::swap(g_tabs[g_cur], g_tabs[idx[to]]);
    TabSlideBy(*g_tabs[g_cur], static_cast<float>(d));
    g_cur = g_paneCur[g_act] = idx[to];
    TabSlideBy(*g_tabs[g_cur], static_cast<float>(-d));
    Invalidate();
    TouchSession();
}

void CloseOtherTabs() {
    const Tab* keep = &T();
    const int act = g_act;
    for (int i = static_cast<int>(g_tabs.size()) - 1; i >= 0; --i)
        if (g_tabs[i].get() != keep && (!g_split || g_tabs[i]->pane == act)) CloseTab(i);
}

void GoBack() {
    Tab& t = T();
    if (t.back.empty()) return;
    t.fwd.push_back(t.path);
    std::wstring p = t.back.back();
    t.back.pop_back();
    Navigate(t, p, false);
}

void GoForward() {
    Tab& t = T();
    if (t.fwd.empty()) return;
    t.back.push_back(t.path);
    std::wstring p = t.fwd.back();
    t.fwd.pop_back();
    Navigate(t, p, false);
}

void GoUp() {
    Tab& t = T();
    if (!t.has || t.path.empty()) return;
    std::wstring parent = ParentOf(t.path);
    g_lastSel[Key(parent)] = LeafOf(t.path);
    Navigate(t, parent, true);
}

std::wstring SelectedPath() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return L"";
    return RowPath(t, t.sel);
}

void OpenSelected(bool inNewTab) {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return;
    const Entry& e = EntryAt(t, t.sel);
    std::wstring full = RowPath(t, t.sel);
    if (e.isDir()) {
        if (CantEnter(full)) return;
        if (inNewTab)
            NewTab(full);
        else
            Navigate(t, full, true);
        return;
    }
    AddRecent(full);
    // Starts in the file's own folder, like File Explorer: games and many programs look for their data there.
    std::thread([full, dir = ParentOf(full)] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, nullptr, full.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
        CoUninitialize();
    }).detach();
}

// Moves the cursor. extend = Shift (range from the anchor); toggle = Ctrl (toggles that item).
void MoveCursor(int i, bool extend = false, bool toggle = false) {
    Tab& t = T();
    int n = Count();
    if (n == 0) return;
    i = SkipHeader(t, i, i >= t.sel ? 1 : -1);
    if (extend)
        SelectRange(t, i);
    else if (toggle)
        SelectToggle(t, i);
    else
        SelectOnly(t, i);
    EnsureVisible(t);
    SetTimer(g_hwnd, kTimerPrefetch, 150, nullptr);
    Invalidate();
}
void SetSel(int i) { MoveCursor(i); }

void Prefetch() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return;
    const Entry& e = EntryAt(t, t.sel);
    if (!e.isDir()) return;
    std::wstring full = RowPath(t, t.sel);
    if (!IsLocalFixed(full) || g_cache.count(Key(full))) return;
    Enqueue(full, JobKind::Prefetch);
}

// --- tree: expanding folders inside the list
constexpr float kChev = 14.f, kIndent = 18.f;
bool IsExpanded(const Tab& t, int row) {
    return row >= 0 && row < RowCount(t) && !FlatView(t) && EntryAt(t, row).isDir() && t.expanded.count(RowKey(t, row));
}

void SetExpanded(Tab& t, int row, bool open) {
    if (row < 0 || row >= RowCount(t) || FlatView(t) || !EntryAt(t, row).isDir()) return;
    std::wstring path = RowPath(t, row), k = Key(path);
    if ((t.expanded.count(k) != 0) == open) return;
    Place p = SavePlace(t);
    if (open) {
        t.expanded[k] = path;
        if (!t.kids.count(k))
            LoadKid(t, k, path);
        else
            Enqueue(path, JobKind::Revalidate);
        auto kid = t.kids.find(k);
        if (kid != t.kids.end() && kid->second && kid->second->error) {  // nothing to show: the row says why
            t.kids.erase(kid);
            t.expanded.erase(k);
        }
    } else {
        t.expanded.erase(k);
    }
    Rebuild(t, &p, L"");
    SetWatchAll();
    Invalidate();
}

int ParentRow(const Tab& t, int row) {
    int d = t.rows[row].depth;
    for (int i = row - 1; i >= 0; --i)
        if (t.rows[i].depth < d) return i;
    return -1;
}

// Collapse: the cursor's folder if open, or the folder containing the cursor (and go to it).
bool Collapse() {
    Tab& t = T();
    int r = t.sel;
    if (r < 0 || r >= Count() || FlatView(t)) return false;
    if (IsExpanded(t, r)) {
        SetExpanded(t, r, false);
        return true;
    }
    if (t.rows[r].depth > 0) {
        int pr = ParentRow(t, r);
        if (pr < 0) return false;
        MoveCursor(pr);
        SetExpanded(t, pr, false);
        return true;
    }
    return false;
}

// Fuzzy filter: typing in the list filters and sorts by match (like fzf).
void SetFilter(Tab& t, const std::wstring& f) {
    t.filter = f;
    BuildRows(t);
    int n = RowCount(t);
    t.selMask.assign(n, 0);
    t.sel = t.anchor = n ? SkipHeader(t, 0, 1) : 0;
    if (n) t.selMask[t.sel] = 1;
    t.scroll = 0;
    SetTimer(g_hwnd, kTimerPrefetch, 150, nullptr);
    Invalidate();
}
void FilterAppend(wchar_t c) {
    Tab& t = T();
    if (t.filter.empty() && c == L' ') return;
    SetFilter(t, t.filter + c);
}
void FilterBackspace() {
    Tab& t = T();
    if (t.filter.empty()) return;
    SetFilter(t, t.filter.substr(0, t.filter.size() - 1));
}
void FilterClear() {
    Tab& t = T();
    if (t.filter.empty()) return;
    Place p = SavePlace(t);
    p.row = -1;
    t.filter.clear();
    Rebuild(t, &p, L"");
    EnsureVisible(t);
    Invalidate();
}

// The sort globals mirror the general view (the listing thread uses them, so results arrive nearly sorted).
void SyncGlobalSort() {
    SortBy by;
    bool asc;
    if (ViewPrimarySort(g_viewAll, by, asc)) {
        g_sortBy = static_cast<int>(by);
        g_sortAsc = asc;
    }
    const Step* f = FirstStep(g_viewAll, StepKind::Folders);
    g_foldersFirst = !f ? 0 : f->a == FmFirst ? 1 : f->a == FmFirstByName ? 2 : 0;
}

void ResortAll() {
    ++g_viewEpoch;  // all views get reapplied
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.list) continue;
        Place p = SavePlace(t);  // before replacing the listings: the rows point to them
        for (auto& [k, kl] : t.kids)
            if (kl) kl = Prepared(g_cache.count(k) ? g_cache[k] : kl);
        if (g_cache.count(Key(t.path))) t.list = Prepared(g_cache[Key(t.path)]);
        Rebuild(t, &p, L"");
    }
    EnsureVisible(T());
    Invalidate();
    TouchSession();
}

// A view changed: save it and show everything again.
void RefreshConfigSig();
void ViewsChanged() {
    SaveViews();
    RefreshConfigSig();  // we wrote it ourselves: no need to reload
    SyncGlobalSort();
    ResortAll();
}

// View of the active folder (its own, the inherited one or the general one).
View& CurView() { return *ViewFor(T().path); }

void SetSortField(int field, bool asc) {
    View& v = CurView();
    auto it = std::find_if(v.steps.begin(), v.steps.end(), [](const Step& s) { return s.kind == StepKind::Sort; });
    if (it == v.steps.end()) {
        v.steps.push_back({StepKind::Sort, field, asc ? 1 : 0, L""});
    } else {
        it->a = field;
        it->b = asc ? 1 : 0;
    }
    ViewsChanged();
}
void SetSortExplicit(SortBy by, bool asc) {
    SetSortField(by == SortBy::Date ? SfDate : by == SortBy::Size ? SfSize : SfName, asc);
}
// Groups the current folder (gb < 0: no groups). Grouping is per folder: if the folder uses
// the general view, it gets its own copy so that not every folder is grouped.
void SetGroup(int gb) {
    Tab& t = T();
    if (t.path.empty()) return;
    View* v = ViewFor(t.path);
    auto isGroup = [](const Step& s) { return s.kind == StepKind::Group; };
    if (gb < 0) {
        auto isFold = [](const Step& s) { return s.kind == StepKind::Fold; };
        v->steps.erase(std::remove_if(v->steps.begin(), v->steps.end(), isGroup), v->steps.end());
        v->steps.erase(std::remove_if(v->steps.begin(), v->steps.end(), isFold), v->steps.end());
    } else {
        if (v == &g_viewAll) {
            View own = *v;
            own.path = t.path;
            own.sub = false;
            v = &(g_views[Key(t.path)] = std::move(own));
        }
        auto it = std::find_if(v->steps.begin(), v->steps.end(), isGroup);
        if (it == v->steps.end()) v->steps.push_back({StepKind::Group, gb, 0, L""});
        else it->a = gb;
    }
    t.fold.clear();
    ViewsChanged();
}
// The folder's layout: 0 = list, otherwise a grid with cells of that size. Per folder, like grouping.
void SetLayout(int size) {
    Tab& t = T();
    if (t.path.empty()) return;
    View* v = ViewFor(t.path);
    if (v == &g_viewAll) {
        View own = *v;
        own.path = t.path;
        own.sub = false;
        v = &(g_views[Key(t.path)] = std::move(own));
    }
    v->layout = size;
    S.message = size ? L"Grid, " + std::to_wstring(size) + L" px cells   \u00b7   Ctrl+Alt+wheel changes the size" : L"List";
    ViewsChanged();
    EnsureVisible(t);
    Invalidate();
}
// Ctrl+Alt+wheel: one step along the view scale (list, then bigger and bigger cells).
void StepLayout(int dir) {
    const int cur = LayoutOf(T());
    int i = 0;
    for (int k = 0; k < static_cast<int>(std::size(kGridSizes)); ++k)
        if (kGridSizes[k] <= cur) i = k;
    i = std::clamp(i + dir, 0, static_cast<int>(std::size(kGridSizes)) - 1);
    if (kGridSizes[i] != cur) SetLayout(kGridSizes[i]);
}
void SetSort(SortBy by) {
    SortBy cur;
    bool asc;
    if (ViewPrimarySort(CurView(), cur, asc) && cur == by)
        SetSortExplicit(by, !asc);
    else
        SetSortExplicit(by, by == SortBy::Name);  // date and size: largest/newest first
}
void SetFoldersFirst(int mode) {
    View& v = CurView();
    int fm = mode == 1 ? FmFirst : mode == 2 ? FmFirstByName : FmMixed;
    auto it = std::find_if(v.steps.begin(), v.steps.end(), [](const Step& s) { return s.kind == StepKind::Folders; });
    if (it == v.steps.end())
        v.steps.push_back({StepKind::Folders, fm, 0, L""});
    else
        it->a = fm;
    ViewsChanged();
}

void ToggleFavorite() {
    Tab& t = T();
    if (t.path.empty()) return;
    auto it = std::find_if(g_favs.begin(), g_favs.end(), [&](const Favorite& v) { return Key(v.path) == Key(t.path); });
    if (it != g_favs.end()) {
        g_favs.erase(it);
        S.message = L"Removed from favorites";
    } else {
        g_favs.push_back({TabTitle(t), t.path});
        S.message = L"Added to favorites";
    }
    SaveFavs();
    HeatRefresh();
    Invalidate();
}

void FavAdd(const std::wstring& path) {
    auto same = [&](const Favorite& v) { return Key(v.path) == Key(path); };
    if (std::find_if(g_favs.begin(), g_favs.end(), same) != g_favs.end()) {
        S.message = L"Already in favorites";
    } else {
        g_favs.push_back({LeafOf(path), path});
        S.message = L"Added to favorites";
        SaveFavs();
        HeatRefresh();
    }
    Invalidate();
}

void FavRemove(const std::wstring& path) {
    g_favs.erase(std::remove_if(g_favs.begin(), g_favs.end(), [&](const Favorite& v) { return Key(v.path) == Key(path); }), g_favs.end());
    S.message = L"Removed from favorites";
    SaveFavs();
    HeatRefresh();
    Invalidate();
}

// ---------------------------------------------------------------- icons
// "Per extension" icons are resolved on the spot (they don't touch the disk).
// Those that depend on the file (folders, .exe, shortcuts, drives)
// are requested from another thread, and a generic icon is drawn until they arrive.
ComPtr<IImageList> g_imgList;
int g_imgListSize = 0;
std::mutex g_im;
std::unordered_map<std::wstring, int> g_iconByPath;
std::unordered_map<std::wstring, int> g_iconByExt;
std::vector<std::wstring> g_iconQueue;
std::unordered_set<std::wstring> g_iconPending;
std::condition_variable g_icv;
std::atomic<bool> g_iconDirty{false};
std::unordered_map<int, ComPtr<ID2D1Bitmap>> g_iconBmp;
int g_iconFolder = -1, g_iconFile = -1;

bool NeedsPerPathIcon(const Entry& e, const wchar_t* name) {
    if (e.isDir()) return true;
    std::wstring ext = ExtOf(name, e.nameLen);
    return ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".url" || ext == L".scr" ||
           ext == L".cur" || ext == L".appref-ms";
}

int IconByAttrs(const wchar_t* name, DWORD attrs) {
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(name, attrs, &sfi, sizeof(sfi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return -1;
    return sfi.iIcon;
}

int IconForExt(const wchar_t* name, size_t len) {
    std::wstring ext = ExtOf(name, len);
    auto it = g_iconByExt.find(ext);
    if (it != g_iconByExt.end()) return it->second;
    std::wstring probe = ext.empty() ? L"file" : L"file" + ext;
    int idx = IconByAttrs(probe.c_str(), FILE_ATTRIBUTE_NORMAL);
    g_iconByExt[ext] = idx;
    return idx;
}

void RequestIcon(const std::wstring& path) {
    std::lock_guard lk(g_im);
    std::wstring k = Key(path);
    if (g_iconByPath.count(k) || g_iconPending.count(k)) return;
    if (g_iconQueue.size() > 400) {  // what was left behind is no longer in view
        for (const auto& q : g_iconQueue) g_iconPending.erase(Key(q));
        g_iconQueue.clear();
    }
    g_iconPending.insert(k);
    g_iconQueue.push_back(path);
    g_icv.notify_one();
}

int IconForPathCached(const std::wstring& path) {
    std::lock_guard lk(g_im);
    auto it = g_iconByPath.find(Key(path));
    return it == g_iconByPath.end() ? -2 : it->second;
}

void IconThreadMain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::wstring p;
        {
            std::unique_lock lk(g_im);
            g_icv.wait(lk, [] { return !g_iconQueue.empty(); });
            p = std::move(g_iconQueue.back());
            g_iconQueue.pop_back();
        }
        SHFILEINFOW sfi{};
        int idx = -1;
        if (p.empty()) {
            // "This PC": the real icon of the virtual drives folder
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pidl))) {
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                    idx = sfi.iIcon;
                CoTaskMemFree(pidl);
            }
        } else if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) {
            idx = sfi.iIcon;
        }
        {
            std::lock_guard lk(g_im);
            std::wstring k = Key(p);
            g_iconByPath[k] = idx;
            g_iconPending.erase(k);
        }
        if (!g_iconDirty.exchange(true)) PostMessageW(g_hwnd, WM_APP_ICON, 0, 0);
    }
}

// ---------------------------------------------------------------- drawing
ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dw;
ComPtr<IWICImagingFactory> g_wic;
ComPtr<ID2D1RenderTarget> g_rt;           // where drawing happens (the window, or a bitmap in test mode)
ComPtr<ID2D1HwndRenderTarget> g_hwndRt;   // the window's
ComPtr<IWICBitmap> g_testBmp;             // the test mode bitmap
ComPtr<ID2D1SolidColorBrush> g_brush, g_brushAccent;
ComPtr<IDWriteRenderingParams> g_textParams;
#include "thumbs.inl"

// defined in palette.inl
void TextHighlighted(const std::wstring& s, const std::wstring& patLower, IDWriteTextFormat* fmt, float l, float t,
                     float r, float b, UINT32 color, UINT32 hiColor, float alpha = 1.f);
void DrawPalette(float W, float H);  // "classic GDI" rasterization: same as the EDIT and File Pilot

void CreateTextParams() {
    ComPtr<IDWriteRenderingParams> def;
    if (FAILED(g_dw->CreateRenderingParams(&def))) return;
    g_dw->CreateCustomRenderingParams(def->GetGamma(), def->GetEnhancedContrast(), def->GetClearTypeLevel(),
                                      def->GetPixelGeometry(), DWRITE_RENDERING_MODE_GDI_CLASSIC, &g_textParams);
}
ComPtr<IDWriteTextFormat> g_fmtName, g_fmtNameBold, g_fmtRight, g_fmtIcon, g_fmtIconSm, g_fmtCap, g_fmtCrumb, g_fmtTab, g_fmtSide,
    g_fmtSection, g_fmtHeader, g_fmtStatus, g_fmtStatusR, g_fmtCenter, g_fmtMono, g_fmtMonoR, g_fmtCell, g_fmtTag, g_fmtMonoWrap;

bool FontExists(const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fc;
    g_dw->GetSystemFontCollection(&fc);
    UINT32 idx = 0;
    BOOL exists = FALSE;
    fc->FindFamilyName(family, &idx, &exists);
    return exists != FALSE;
}

ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size, DWRITE_TEXT_ALIGNMENT align,
                                     DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, bool trim = true) {
    ComPtr<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                           L"en-us", &f);
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (trim) {
        DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        g_dw->CreateEllipsisTrimmingSign(f.Get(), &sign);
        f->SetTrimming(&t, sign.Get());
    }
    return f;
}

// Laying out text is the most expensive part of each frame, and most repeat: they're kept ready-made.
std::unordered_map<uint64_t, ComPtr<IDWriteTextLayout>> g_textCache;

void CreateFormats() {
    g_textCache.clear();
    // By default, the same typeface as File Pilot: classic Segoe UI at 9 pt (12 DIP).
    const wchar_t* ui = FontExists(g_cfg.font.c_str()) ? g_cfg.font.c_str() : L"Segoe UI";
    const wchar_t* icons = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const float sz = g_cfg.fontSize, smallSz = std::max(8.f, sz - 2);  // ("small" is a Windows macro)
    g_fmtName = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtNameBold = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fmtRight = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fmtIcon = MakeFormat(icons, 14.f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtIconSm = MakeFormat(icons, 9.f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtCap = MakeFormat(icons, 10.f, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtCrumb = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtTab = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtSide = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtSection = MakeFormat(ui, smallSz, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fmtHeader = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_NORMAL);
    g_fmtStatus = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmtStatusR = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fmtCenter = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fmtTag = MakeFormat(ui, smallSz, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);  // the name inside a tag label
    g_fmtCell = MakeFormat(ui, sz, DWRITE_TEXT_ALIGNMENT_CENTER);  // grid names: centered, up to two lines
    g_fmtCell->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    g_fmtCell->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    // text preview: monospaced, not trimmed with "…"
    const wchar_t* mono = FontExists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    g_fmtMono = MakeFormat(mono, 12.f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtMonoR = MakeFormat(mono, 12.f, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtMonoWrap = MakeFormat(mono, 12.f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtMonoWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);  // by character: the row count is then len / columns
    g_fmtMonoWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void PreviewReleaseBitmap();  // preview.inl
void ThumbsReleaseBitmaps();  // thumbs.inl
void ReleaseTarget() {
    PreviewReleaseBitmap();
    ThumbsReleaseBitmaps();
    g_iconBmp.clear();
    g_brushAccent.Reset();
    g_brush.Reset();
    g_rt.Reset();
    g_hwndRt.Reset();
    g_testBmp.Reset();
}

bool EnsureTarget() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(),
                                              static_cast<float>(g_dpi), static_cast<float>(g_dpi));
    auto hprops = D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom),
                                                   D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (g_test) {
        // no visible window: draw to a bitmap (which the capture step saves as a PNG)
        if (FAILED(g_wic->CreateBitmap(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom), GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapCacheOnLoad, &g_testBmp)))
            return false;
        auto wprops = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                   static_cast<float>(g_dpi), static_cast<float>(g_dpi));
        if (FAILED(g_d2d->CreateWicBitmapRenderTarget(g_testBmp.Get(), wprops, &g_rt))) return false;
    } else {
        if (FAILED(g_d2d->CreateHwndRenderTarget(props, hprops, &g_hwndRt))) return false;
        g_rt = g_hwndRt;
    }
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (g_textParams) g_rt->SetTextRenderingParams(g_textParams.Get());
    g_rt->CreateSolidColorBrush(D2D1::ColorF(cText), &g_brush);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(cAccent), &g_brushAccent);
    return true;
}

void EnsureImageList() {
    int want = g_dpi >= 144 || g_dpi > g_sysDpi ? SHIL_LARGE : SHIL_SMALL;  // with zoom: shrink, don't enlarge
    if (g_imgList && g_imgListSize == want) return;
    g_imgList.Reset();
    SHGetImageList(want, IID_PPV_ARGS(&g_imgList));
    g_imgListSize = want;
    g_iconBmp.clear();
}

// A system icon as a bitmap of the current render target.
ComPtr<ID2D1Bitmap> MakeIconBitmap(int idx) {
    ComPtr<ID2D1Bitmap> bmp;
    if (idx < 0 || !g_imgList) return bmp;
    HICON hi = nullptr;
    if (SUCCEEDED(g_imgList->GetIcon(idx, ILD_TRANSPARENT, &hi)) && hi) {
        ComPtr<IWICBitmap> wb;
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(g_wic->CreateBitmapFromHICON(hi, &wb)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeMedianCut)))
            g_rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
        DestroyIcon(hi);
    }
    return bmp;
}

ID2D1Bitmap* IconBitmap(int idx) {
    if (idx < 0 || !g_imgList) return nullptr;
    auto it = g_iconBmp.find(idx);
    if (it != g_iconBmp.end()) return it->second.Get();
    return (g_iconBmp[idx] = MakeIconBitmap(idx)).Get();
}

void Color(UINT32 rgb, float a = 1.f) { g_brush->SetColor(D2D1::ColorF(rgb, a)); }

void Text(const wchar_t* s, size_t len, IDWriteTextFormat* f, float l, float t, float r, float b) {
    if (r <= l) return;
    const float w = r - l, h = b - t;
    uint64_t key = 14695981039346656037ull;
    auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    for (size_t i = 0; i < len; ++i) mix(s[i]);
    mix(reinterpret_cast<uintptr_t>(f));
    mix(std::bit_cast<uint32_t>(w));
    mix(std::bit_cast<uint32_t>(h));
    auto it = g_textCache.find(key);
    if (it == g_textCache.end()) {
        if (g_textCache.size() >= 4096) g_textCache.clear();
        ComPtr<IDWriteTextLayout> lay;
        if (FAILED(g_dw->CreateTextLayout(s, static_cast<UINT32>(len), f, w, h, &lay))) return;
        it = g_textCache.emplace(key, std::move(lay)).first;
    }
    g_rt->DrawTextLayout(D2D1::Point2F(l, t), it->second.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Text(const std::wstring& s, IDWriteTextFormat* f, float l, float t, float r, float b) {
    Text(s.c_str(), s.size(), f, l, t, r, b);
}
void Glyph(wchar_t g, IDWriteTextFormat* f, float l, float t, float r, float b) { Text(&g, 1, f, l, t, r, b); }

float MeasureW(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4096, 100, &lay))) return 0;
    DWRITE_TEXT_METRICS m;
    lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Fill(const D2D1_RECT_F& r, UINT32 c, float a = 1.f) {
    Color(c, a);
    if (g_dpi % 96 == 0) {
        g_rt->FillRectangle(r, g_brush.Get());
        return;
    }
    // with zoom a DIP isn't a whole pixel: snap so thin lines stay sharp
    const float s = g_dpi / 96.f;
    auto snap = [s](float v) { return std::round(v * s) / s; };
    D2D1_RECT_F q{snap(r.left), snap(r.top), snap(r.right), snap(r.bottom)};
    if (r.right > r.left && r.right - r.left <= 1.01f) q.right = q.left + 1 / s;  // a line: always 1 pixel
    if (r.bottom > r.top && r.bottom - r.top <= 1.01f) q.bottom = q.top + 1 / s;
    g_rt->FillRectangle(q, g_brush.Get());
}
void Round(const D2D1_RECT_F& r, float rad, UINT32 c, float a = 1.f) {
    Color(c, a);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_brush.Get());
}
void HLine(float x1, float x2, float y, UINT32 c = cBorder) { Fill(D2D1::RectF(x1, y, x2, y + 1), c); }

bool IsHover(HitKind k, int idx) { return g_hover.kind == k && g_hover.idx == idx; }

// Draws the item's real icon, or a generic one while it's being resolved.
void DrawIcon(const std::wstring& fullPath, const Entry* e, const wchar_t* name, size_t nameLen, bool drive, float x,
              float y, float alpha) {
    int idx = -2;
    bool perPath = drive || !e || NeedsPerPathIcon(*e, name);
    if (perPath) {
        idx = IconForPathCached(fullPath);
        if (idx == -2) RequestIcon(fullPath);
    } else {
        idx = IconForExt(name, nameLen);
    }
    ID2D1Bitmap* bmp = idx >= 0 ? IconBitmap(idx) : nullptr;
    const bool pc = fullPath.empty();
    const bool dirLike = !e || e->isDir();
    if (!bmp && idx == -2 && !drive && !pc && dirLike) bmp = IconBitmap(g_iconFolder);
    if (!bmp && idx == -2 && e && !e->isDir()) bmp = IconBitmap(g_iconFile);
    D2D1_RECT_F r = D2D1::RectF(x, y, x + kIcon, y + kIcon);
    if (bmp) {
        g_rt->DrawBitmap(bmp, r, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        Color(drive || pc ? 0x7FB3FF : dirLike ? 0xE6B450 : 0x9AA3B2, alpha);
        Glyph(pc ? gPc : drive ? gDrive : dirLike ? gFolder : gFile, g_fmtIcon.Get(), x - 2, y - 5, x + kIcon + 2,
              y + kIcon + 5);
    }
}

void FormatDate(int64_t ft, wchar_t* buf, size_t n) {
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(static_cast<uint64_t>(ft) >> 32)};
    SYSTEMTIME u, l;
    FileTimeToSystemTime(&f, &u);
    SystemTimeToTzSpecificLocalTime(nullptr, &u, &l);
    swprintf(buf, n, L"%02u/%02u/%04u  %02u:%02u", l.wDay, l.wMonth, l.wYear, l.wHour, l.wMinute);
}

void FormatSize(uint64_t s, wchar_t* buf, size_t n) {
    if (s < 1024) {
        swprintf(buf, n, L"%llu B", static_cast<unsigned long long>(s));
        return;
    }
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB"};
    double v = s / 1024.0;
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    swprintf(buf, n, v < 10 ? L"%.1f %s" : L"%.0f %s", v, units[u]);
}

std::wstring Thousands(int n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}

std::wstring Ms(double v) {
    wchar_t b[32];
    swprintf(b, 32, v < 10 ? L"%.2f ms" : L"%.0f ms", v);
    return b;
}

std::wstring ErrorText(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"unknown error";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

// View editor (viewedit.inl): what list drawing needs to know.
struct ViewEditorState {
    bool open = false;
    int pane = 0;
} g_ve;

// --- tabs (also the title bar: the window is dragged from here)
// In split view, each pane has its own tab group above it.
// The visible tab's background travels to the new one. Drawing only: the change already happened.
struct TabPill {
    const Tab* tab = nullptr;
    float x = 0, from = 0;  // where it was last drawn; distance to travel at the start
    double at = 0;
} g_pill[2];
constexpr double kTabPillMs = 120;

void DrawTabGroup(int pane, float x0, float xEnd, bool activeWnd) {
    std::vector<int> idx;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        if (!g_split || g_tabs[i]->pane == pane) idx.push_back(i);
    const int n = static_cast<int>(idx.size());
    if (!n) return;
    const float avail = xEnd - x0 - 30;  // room for the "+"
    float tw = std::min(kTabMaxW, std::max(36.f, avail / n));
    if (avail / n >= kTabMinW) tw = std::clamp(avail / n, kTabMinW, kTabMaxW);
    const float top = 7, bot = kTabH;
    const int visible = g_split ? g_paneCur[pane] : g_cur;
    // the visible tab goes last, so it passes over the other one when moving
    std::vector<int> order;
    for (int k = 0; k < n; ++k)
        if (idx[k] != visible) order.push_back(k);
    for (int k = 0; k < n; ++k)
        if (idx[k] == visible) order.push_back(k);
    float pillOff = 0;
    if (order.back() >= 0 && idx[order.back()] == visible) {
        TabPill& pill = g_pill[pane];
        Tab& vt = *g_tabs[visible];
        const float vx = x0 + order.back() * tw + TabSlide(vt) * tw;
        if (&vt != pill.tab) {
            if (pill.tab) {
                pill.from = pill.x - vx;
                pill.at = NowMs();
            }
            pill.tab = &vt;
        }
        const double p = (NowMs() - pill.at) / kTabPillMs;
        if (p >= 1) pill.from = 0;
        const float left = static_cast<float>(1 - std::min(p, 1.0));
        pillOff = pill.from * left * left * left;
        pill.x = vx + pillOff;
        if (pillOff != 0) {
            // beneath the titles, so it doesn't cover them as it passes
            SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
            Round(D2D1::RectF(pill.x, top, pill.x + tw - 4, bot + 8), 7, cTop);
        }
    }
    for (int k : order) {
        const int i = idx[k];
        const float x = x0 + k * tw;
        const float off = TabSlide(*g_tabs[i]) * tw;
        if (off != 0) SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
        const D2D1_RECT_F home = D2D1::RectF(x, top, x + tw - 4, bot);  // clicks go to its final place
        D2D1_RECT_F r = home;
        r.left += off;
        r.right += off;
        bool cur = i == visible;
        bool activePane = !g_split || pane == g_act;
        bool hov = IsHover(HitKind::Tab, i) || IsHover(HitKind::TabClose, i);
        if (cur) {
            // visible tab: blends into the bar below
            if (pillOff == 0) Round(D2D1::RectF(r.left, r.top, r.right, r.bottom + 8), 7, cTop);
        } else if (hov || i == g_dropTab) {
            Round(D2D1::RectF(r.left, r.top + 2, r.right, r.bottom - 3), 6, i == g_dropTab ? cAccent : 0xFFFFFF,
                  i == g_dropTab ? 0.25f : 0.05f);
        }
        std::wstring title = TabTitle(*g_tabs[i]);
        bool showClose = cur || hov;
        float textR = showClose ? r.right - 26 : r.right - 10;
        Color(cur && activePane && activeWnd ? cText : cDim);
        Text(title, g_fmtTab.Get(), r.left + 12, r.top, textR, r.bottom);
        if (showClose) {
            D2D1_RECT_F cr = D2D1::RectF(r.right - 24, (top + bot) / 2 - 8, r.right - 8, (top + bot) / 2 + 8);
            if (IsHover(HitKind::TabClose, i)) Round(cr, 4, 0xFFFFFF, 0.12f);
            Color(cur ? cDim : cFaint);
            Glyph(gClose, g_fmtIconSm.Get(), cr.left, cr.top, cr.right, cr.bottom);
            g_hits.push_back({D2D1::RectF(cr.left - off, cr.top, cr.right - off, cr.bottom), HitKind::TabClose, i});
        }
        g_hits.push_back({home, HitKind::Tab, i});
    }
    const float x = x0 + n * tw;
    D2D1_RECT_F plus = D2D1::RectF(x + 2, (top + bot) / 2 - 11, x + 24, (top + bot) / 2 + 11);
    if (IsHover(HitKind::TabNew, pane)) Round(plus, 5, 0xFFFFFF, 0.08f);
    Color(cDim);
    Glyph(gAdd, g_fmtIconSm.Get(), plus.left, plus.top, plus.right, plus.bottom);
    g_hits.push_back({plus, HitKind::TabNew, pane});
}

void DrawTabs(float W) {
    Fill(D2D1::RectF(0, 0, W, kTabH), cTabBar);
    const bool active = g_test ? g_testActive : GetActiveWindow() == g_hwnd;

    // Fixed-width left area, always free for dragging, with the sidebar button
    // at its end. It doesn't move when the sidebar is hidden.
    D2D1_RECT_F tg = D2D1::RectF(kTitleLeft - 36, 7, kTitleLeft - 8, kTabH - 7);
    if (IsHover(HitKind::SideToggle, 0)) Round(tg, 5, 0xFFFFFF, 0.08f);
    Color(active ? cDim : cFaint);
    Glyph(g_sideVisible ? gPaneClose : gPaneOpen, g_fmtIcon.Get(), tg.left, tg.top, tg.right, tg.bottom);
    g_hits.push_back({tg, HitKind::SideToggle, 0});

    // general app menu
    D2D1_RECT_F mb = D2D1::RectF(10, 7, 38, kTabH - 7);
    if (IsHover(HitKind::AppMenu, 0)) Round(mb, 5, 0xFFFFFF, 0.08f);
    Color(active ? cDim : cFaint);
    Glyph(0xE700, g_fmtIcon.Get(), mb.left, mb.top, mb.right, mb.bottom);
    g_hits.push_back({mb, HitKind::AppMenu, 0});

    // window buttons, on the right
    const wchar_t capGlyph[3] = {gMin, IsZoomed(g_hwnd) ? gRestore : gMax, gClose};
    const int capCode[3] = {HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = D2D1::RectF(W - (3 - i) * kCapBtnW, 0, W - (2 - i) * kCapBtnW, kTabH);
        g_capRects[i] = r;
        bool hov = g_capHover == capCode[i];
        if (hov) Fill(r, i == 2 ? 0xC42B1C : 0xFFFFFF, i == 2 ? 1.f : 0.08f);
        Color(active || hov ? cText : cFaint);
        Glyph(capGlyph[i], g_fmtCap.Get(), r.left, r.top, r.right, r.bottom);
    }

    float xEnd = W - 3 * kCapBtnW - 14;
    if (g_elevated) {  // what is opened from here runs as administrator too: it has to show
        const float tw = std::ceil(MeasureW(L"Administrator", g_fmtName.Get())), bx = xEnd - tw;
        Color(0xE5A84B, active ? 1.f : 0.6f);
        Glyph(0xEA18, g_fmtIcon.Get(), bx - 22, 0, bx - 4, kTabH);
        Text(L"Administrator", g_fmtName.Get(), bx, 0, bx + tw + 2, kTabH);
        xEnd = bx - 34;
    }
    const float x0 = kTitleLeft + 2;
    if (!g_split) {
        DrawTabGroup(0, x0, xEnd, active);
    } else {
        const float mid = PaneGeo(1).TL;
        DrawTabGroup(0, x0, mid - 6, active);
        Fill(D2D1::RectF(mid, 12, mid + 1, kTabH - 8), cBorder);
        DrawTabGroup(1, mid + 6, xEnd, active);
    }
}

// --- navigation bar (one per pane)
void DrawToolbar(int pane) {
    const PaneX g = PaneGeo(pane);
    const float y0 = kTabH, y1 = kTabH + kToolH;
    Fill(D2D1::RectF(g.TL, y0, g.TR, y1), cTop);
    Tab& t = PT(pane);
    const bool activePane = pane == g_act;
    struct Btn {
        wchar_t g;
        HitKind k;
        bool enabled;
    } btns[] = {{gBack, HitKind::Back, !t.back.empty()},
                {gFwd, HitKind::Fwd, !t.fwd.empty()},
                {gUp, HitKind::Up, !t.path.empty()}};
    float x = g.TL + kPad;
    const float cy = (y0 + y1) / 2;
    for (const Btn& b : btns) {
        D2D1_RECT_F r = D2D1::RectF(x, cy - 14, x + 28, cy + 14);
        if (b.enabled && IsHover(b.k, pane)) Round(r, 6, 0xFFFFFF, 0.08f);
        Color(b.enabled ? (activePane ? cText : cDim) : cFaint);
        Glyph(b.g, g_fmtIcon.Get(), r.left, r.top, r.right, r.bottom);
        if (b.enabled) g_hits.push_back({r, b.k, pane});
        x += 32;
    }
    x += 8;

    // path as breadcrumbs
    const float crumbL = x, crumbR = g.TR - kPad;
    D2D1_RECT_F bar = D2D1::RectF(crumbL, cy - 15, crumbR, cy + 15);
    Round(bar, 7, cBg);
    if (g_editing && activePane) return;
    struct Crumb {
        std::wstring label, path;
        float w;
    };
    std::vector<Crumb> crumbs;
    crumbs.push_back({L"", L"", 30});  // "This PC" icon
    if (!t.path.empty()) {
        std::wstring acc;
        size_t start = 0;
        bool unc = t.path.rfind(L"\\\\", 0) == 0;
        if (unc) start = 2;
        for (;;) {
            size_t pos = t.path.find(L'\\', start);
            std::wstring seg = t.path.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
            if (seg.empty()) break;
            if (acc.empty())
                acc = unc ? L"\\\\" + seg : seg + L"\\";
            else
                acc = Join(acc, seg.c_str());
            std::wstring label = (!unc && acc.size() == 3 && IsDriveRoot(acc)) ? DriveLabel(acc[0]) : seg;
            crumbs.push_back({label, Normalize(acc), MeasureW(label, g_fmtCrumb.Get()) + 18});
            if (pos == std::wstring::npos) break;
            start = pos + 1;
        }
    }
    // if it doesn't fit, the first ones are hidden behind "…"
    const float chevW = 16;
    float total = 0;
    for (const Crumb& c : crumbs) total += c.w + chevW;
    size_t first = 0;
    while (total > crumbR - crumbL - 8 && first + 1 < crumbs.size()) {
        total -= crumbs[first].w + chevW;
        ++first;
    }
    const UINT32 lastColor = activePane ? cText : 0xB9B9C2;
    float cx = crumbL + 4;
    // the chevron after a segment opens the list of the folders inside it; idx: that segment
    auto chevron = [&](int idx) {
        const D2D1_RECT_F r = D2D1::RectF(cx, cy - 12, cx + chevW, cy + 12);
        const bool open = g_crumbMenuOpen && activePane && Key(g_crumbPaths[idx]) == Key(g_crumbMenuDir);
        const bool hot = open || IsHover(HitKind::CrumbChev, idx);
        if (hot) Round(r, 4, 0xFFFFFF, 0.08f);
        Color(hot ? cDim : cFaint);
        Glyph(open ? 0xE70D : gChevron, g_fmtIconSm.Get(), cx, cy - 8, cx + chevW, cy + 8);
        g_hits.push_back({r, HitKind::CrumbChev, idx});
        cx += chevW;
    };
    g_rt->PushAxisAlignedClip(bar, D2D1_ANTIALIAS_MODE_ALIASED);
    if (first > 0) {
        D2D1_RECT_F r = D2D1::RectF(cx, cy - 12, cx + 24, cy + 12);
        if (IsHover(HitKind::Crumb, static_cast<int>(g_crumbPaths.size()))) Round(r, 5, 0xFFFFFF, 0.08f);
        Color(cDim);
        Text(L"…", g_fmtCrumb.Get(), r.left, r.top, r.right, r.bottom);
        g_hits.push_back({r, HitKind::Crumb, static_cast<int>(g_crumbPaths.size())});
        g_crumbPaths.push_back(crumbs[first - 1].path);
        cx += 24;
        chevron(static_cast<int>(g_crumbPaths.size()) - 1);
    }
    for (size_t i = first; i < crumbs.size(); ++i) {
        const Crumb& c = crumbs[i];
        bool last = i + 1 == crumbs.size();
        D2D1_RECT_F r = D2D1::RectF(cx, cy - 12, cx + c.w, cy + 12);
        if (IsHover(HitKind::Crumb, static_cast<int>(g_crumbPaths.size()))) Round(r, 5, 0xFFFFFF, 0.08f);
        Color(last ? lastColor : cDim);
        if (i == 0)
            Glyph(gPc, g_fmtIcon.Get(), r.left, r.top, r.right, r.bottom);
        else
            Text(c.label, g_fmtCrumb.Get(), r.left, r.top, r.right, r.bottom);
        g_hits.push_back({r, HitKind::Crumb, static_cast<int>(g_crumbPaths.size())});
        g_crumbPaths.push_back(c.path);
        cx += c.w;
        if (last) g_crumbLast[pane] = r;
        else chevron(static_cast<int>(g_crumbPaths.size()) - 1);
    }
    g_rt->PopAxisAlignedClip();
    if (cx < bar.right) g_hits.push_back({D2D1::RectF(cx, bar.top, bar.right, bar.bottom), HitKind::PathEmpty, pane});
}

// --- sidebar
std::wstring DiskFreeText(const DriveSpace& d) {
    wchar_t buf[64];
    FormatSize(d.freeB, buf, 64);
    return std::wstring(buf) + L" free";
}

// The whole row becomes the usage bar, with the free space and the total on top.
void DrawDiskSpace(const D2D1_RECT_F& r, const DriveSpace& d) {
    constexpr float kRad = 5, kTextPad = 10;
    const float w = r.right - r.left;
    Round(r, kRad, 0xFFFFFF, 0.08f);
    Round(D2D1::RectF(r.left, r.top, r.left + std::max(kRad * 2, w * d.usage), r.bottom), kRad,
          d.usage >= 0.9f ? cDiskFull : cDisk, 0.4f);
    g_diskShown = DiskFreeText(d);
    wchar_t buf[64];
    FormatSize(d.totalB, buf, 64);
    const std::wstring total = std::wstring(L"of ") + buf;
    Color(cText);
    Text(g_diskShown, g_fmtSide.Get(), r.left + kTextPad, r.top, r.right - kTextPad, r.bottom);
    // With a very narrow sidebar only the free space fits.
    if (MeasureW(g_diskShown, g_fmtSide.Get()) + MeasureW(total, g_fmtRight.Get()) + kTextPad * 3 <= w)
        Text(total, g_fmtRight.Get(), r.left + kTextPad, r.top, r.right - kTextPad, r.bottom);
}

// Repaints only if what's shown changed; stops reading once the mouse leaves the drive.
void DiskTick() {
    if (!g_diskHover || g_hover.kind != HitKind::Side) {
        KillTimer(g_hwnd, kTimerDisk);
        g_diskHover = 0;
        return;
    }
    if (DiskFreeText(DriveSpaceOf(g_diskHover, true)) != g_diskShown) Invalidate();
}

void DrawSidebar(float H) {
    if (!g_sideVisible) return;
    const float y0 = kTabH + kToolH;
    Fill(D2D1::RectF(0, y0, kSideW, H - kStatus), cSide);
    Fill(D2D1::RectF(kSideW - 1, y0, kSideW, H - kStatus), cBorder);
    g_sidePaths.clear();
    const std::wstring cur = Key(T().path);
    float y = y0 + 10;
    auto section = [&](const wchar_t* title) {
        Color(cFaint);
        std::wstring s = title;
        Text(s, g_fmtSection.Get(), kPad + 6, y, kSideW - kPad, y + 20);
        y += 22;
    };
    // name, path, is drive, disk usage (0..1, <0 = no bar)
    // disk: drive row under the mouse, showing the space instead of the name.
    auto item = [&](const std::wstring& name, const std::wstring& path, bool drive, float usage,
                    const DriveSpace* disk = nullptr) {
        const float rowH = usage >= 0 ? kSideRow + 12 : kSideRow;
        D2D1_RECT_F r = D2D1::RectF(6, y, kSideW - 7, y + rowH);
        int idx = static_cast<int>(g_sidePaths.size());
        bool active = Key(path) == cur;
        if (disk) {
            DrawDiskSpace(r, *disk);
            g_hits.push_back({r, HitKind::Side, idx});
            g_sidePaths.push_back(path);
            y += rowH;
            return;
        }
        if (active)
            Round(r, 5, cAccent, 0.22f);
        else if (IsHover(HitKind::Side, idx) && !g_sideDrag.active)
            Round(r, 5, 0xFFFFFF, 0.05f);
        if (idx == g_dropSide) {
            Color(cAccent, 0.9f);
            g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 5, 5), g_brush.Get(), 1.5f);
        }
        DrawIcon(path, nullptr, nullptr, 0, drive, r.left + 10, y + 5, 1.f);
        Color(active ? cText : 0xC9C9D1);
        Text(name, g_fmtSide.Get(), r.left + 34, y, r.right - 6, y + kSideRow);
        if (usage >= 0) {
            // usage bar, like File Pilot: blue, red when space is low.
            // Same margin on both sides relative to the sidebar edge.
            const float margin = kSideW - 1 - (r.right - 10);
            const float bx = margin, bw = r.right - 10 - bx, by = y + kSideRow - 1, bh = 6;
            Round(D2D1::RectF(bx, by, bx + bw, by + bh), bh / 2, 0xFFFFFF, 0.10f);
            Round(D2D1::RectF(bx, by, bx + std::max(bh, bw * usage), by + bh), bh / 2, usage >= 0.9f ? cDiskFull : cDisk);
        }
        g_hits.push_back({r, HitKind::Side, idx});
        g_sidePaths.push_back(path);
        y += rowH;
    };
    section(L"FAVORITES");
    g_favTop = y;
    for (const Favorite& v : g_favs) item(v.name, v.path, false, -1);
    g_favBottom = y;
    y += 12;
    section(L"DRIVES");
    item(L"This PC", L"", false, -1);
    DWORD mask = GetLogicalDrives();
    wchar_t hoverDrive = 0;
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = static_cast<wchar_t>(L'A' + i);
        std::wstring root = std::wstring(1, letter) + L":\\";
        const int idx = static_cast<int>(g_sidePaths.size());
        const bool hov = IsHover(HitKind::Side, idx) && !g_sideDrag.active && idx != g_dropSide;
        const DriveSpace& ds = DriveSpaceOf(letter, hov && letter != g_diskHover);
        const bool showSpace = hov && ds.usage >= 0;
        if (showSpace) hoverDrive = letter;
        item(DriveLabel(letter), root, true, ds.usage, showSpace ? &ds : nullptr);
    }
    if (hoverDrive != g_diskHover) {
        g_diskHover = hoverDrive;
        if (hoverDrive) SetTimer(g_hwnd, kTimerDisk, kDiskTickMs, nullptr);
        else KillTimer(g_hwnd, kTimerDisk);
    }
    g_sideHotFirst = static_cast<int>(g_sidePaths.size());
    const float bottom = H - kStatus - 6;
    if (g_cfg.hotCount > 0 && !g_hot.empty() && y + 12 + 22 + kSideRow <= bottom) {
        y += 12;
        section(L"RECENTS");
        int shown = 0;
        for (const HotItem& h : g_hot) {
            if (shown++ >= g_cfg.hotCount || y + kSideRow > bottom) break;
            item(h.name, h.path, false, -1);
        }
    }
    DrawSideDrag();
}

// --- column header and list
int g_colDrag = 0, g_colDragPane = 0;  // column being dragged (1 = date, 2 = size)
float g_colDragX = 0, g_colDragW = 0;
struct Columns {
    bool showSize, showDate;
    float L, R, nameL, nameR, dateL, sizeL, right;
};
Columns Cols(int pane) {
    Columns c;
    const PaneX g = PaneGeo(pane);
    c.L = g.L;
    c.R = g.R;
    c.showSize = g_cfg.colSize && g.R - g.L >= kSizeW + 308;
    c.showDate = g_cfg.colDate && g.R - g.L >= kSizeW + kDateW + 350;
    c.right = g.R - kPad - 6;
    c.sizeL = c.right - kSizeW;
    c.dateL = (c.showSize ? c.sizeL - 18 : c.right) - kDateW;
    c.nameL = g.L + kPad;
    c.nameR = c.showDate ? c.dateL - 18 : (c.showSize ? c.sizeL - 18 : c.right);
    return c;
}

// Tree indent for a row (0 with a filter or search: the list is flat there).
float RowIndent(const Tab& t, int row) { return FlatView(t) ? 0.f : t.rows[row].depth * kIndent; }
float RowTextX(const Columns& c, const Tab& t, int row) { return c.nameL + kChev + RowIndent(t, row) + 28; }

void DrawHeader(int pane) {
    const float y0 = kTabH + kToolH, y1 = y0 + kHeadH;
    Columns c = Cols(pane);
    Fill(D2D1::RectF(c.L, y0, c.R, y1), cBg);
    HLine(c.L, c.R, y1 - 1);
    const View& view = *ViewFor(PT(pane).path);
    SortBy by = SortBy::Name;
    bool sortAsc = true;
    const bool hasSort = ViewPrimarySort(view, by, sortAsc);
    // view button (filter): highlighted if the folder has its own (or inherited) view
    {
        const bool own = &view != &g_viewAll;
        D2D1_RECT_F vb = D2D1::RectF(c.nameL - 4, y0 + 4, c.nameL + 20, y1 - 5);
        if (IsHover(HitKind::ViewBtn, pane) || (g_ve.open && g_ve.pane == pane)) Round(vb, 5, 0xFFFFFF, 0.08f);
        Color(own ? cAccent : IsHover(HitKind::ViewBtn, pane) ? cDim : cFaint);
        Glyph(0xE71C, g_fmtIconSm.Get(), vb.left, vb.top, vb.right, vb.bottom);
        g_hits.push_back({vb, HitKind::ViewBtn, pane});
        // collapse / expand all groups (only if the list has groups)
        const Tab& pt = PT(pane);
        if (pt.list && !pt.list->groups.empty() && pt.filter.empty() && !pt.recursive) {
            bool allFolded = true;
            for (size_t gi = 0; gi < pt.list->groups.size(); ++gi) allFolded &= GroupFolded(pt, pt.list.get(), gi);
            D2D1_RECT_F fb = D2D1::RectF(c.nameL + 18, y0 + 4, c.nameL + 40, y1 - 5);
            if (IsHover(HitKind::FoldBtn, pane)) Round(fb, 5, 0xFFFFFF, 0.08f);
            Color(IsHover(HitKind::FoldBtn, pane) ? cText : cDim);
            // two chevrons: inward = collapse all, outward = expand all
            const float cx = (fb.left + fb.right) / 2, cy = (fb.top + fb.bottom) / 2;
            Glyph(allFolded ? 0xE70D : 0xE70E, g_fmtIconSm.Get(), cx - 8, cy - 9, cx + 8, cy + 1);
            Glyph(allFolded ? 0xE70E : 0xE70D, g_fmtIconSm.Get(), cx - 8, cy - 1, cx + 8, cy + 9);
            g_hits.push_back({fb, HitKind::FoldBtn, pane});
        }
    }
    struct Col {
        const wchar_t* label;
        SortBy by;
        float l, r;
        bool right;
        bool show;
    } cols[] = {{L"Name", SortBy::Name, c.nameL, c.nameR, false, true},
                {L"Modified", SortBy::Date, c.dateL, c.dateL + kDateW, true, c.showDate},
                {L"Size", SortBy::Size, c.sizeL, c.right, true, c.showSize}};
    int i = 0;
    for (const Col& col : cols) {
        if (col.show) {
            // "Name" starts after the view button
            D2D1_RECT_F r = D2D1::RectF(col.right ? col.l - 6 : col.l + kChev + 26, y0 + 3, col.r + 6, y1 - 4);
            bool active = hasSort && by == col.by;
            int hid = pane * 3 + i;
            if (IsHover(HitKind::Header, hid)) Round(r, 4, 0xFFFFFF, 0.05f);
            Color(active ? cText : cDim);
            float tl = col.l + (col.right ? 0 : kChev + 28), tr = col.r;
            if (active) {
                wchar_t g = sortAsc ? gSortUp : gSortDown;
                float w = MeasureW(col.label, g_fmtHeader.Get());
                // right next to the title, even if the column is wide
                if (col.right) Glyph(g, g_fmtIconSm.Get(), tr - w - 18, y0, tr - w - 4, y1);
                else Glyph(g, g_fmtIconSm.Get(), tl + w + 4, y0, tl + w + 18, y1);
            }
            Text(col.label, wcslen(col.label), col.right ? g_fmtRight.Get() : g_fmtHeader.Get(), tl, y0, tr, y1);
            g_hits.push_back({r, HitKind::Header, hid});
        }
        ++i;
    }
    // edges for changing the width: the line only appears with the mouse over it
    auto edge = [&](int which, float colL) {
        const float x = colL - 9;
        const int id = pane * 2 + which;
        if (IsHover(HitKind::ColEdge, id) || (g_colDrag == which + 1 && g_colDragPane == pane))
            Fill(D2D1::RectF(x, y0 + 6, x + 1, y1 - 7), cDim);
        g_hits.push_back({D2D1::RectF(x - 5, y0, x + 6, y1 - 1), HitKind::ColEdge, id});
    };
    if (c.showDate) edge(0, c.dateL);
    if (c.showSize) edge(1, c.sizeL);
}

void DrawBox(int pane, float listTop, float listBot);  // boxselect.inl
#include "access.inl"

// The little discs after a name: one per tag (where a label wouldn't fit: the grid cells).
void DrawTagDots(const std::wstring& full, float x, float cy, float a, float& xEnd) {
    xEnd = x;
    const auto* tags = TagsOf(full);
    if (!tags) return;
    for (const std::wstring& t : *tags) {
        Round(D2D1::RectF(xEnd, cy - 3.5f, xEnd + 7, cy + 3.5f), 3.5f, TagColor(t), a);
        xEnd += 10;
    }
    xEnd += 4;
}

// A tag as a little label: a rectangle with a pointed left end (and its hole), the name inside. Draws as many as fit
// before xMax, the rest as dots; xEnd is where the row may go on.
void DrawTagLabels(const std::wstring& full, float x, float cy, float xMax, float a, float& xEnd) {
    xEnd = x;
    const auto* tags = TagsOf(full);
    if (!tags) return;
    const float h = std::clamp(kRow - 6.f, 14.f, 17.f), top = std::floor(cy - h / 2), bot = top + h, notch = std::floor(h / 2), r = 3;
    const float mid = (top + bot) / 2;
    bool dots = false;
    for (const std::wstring& t : *tags) {
        const UINT32 col = TagColor(t);
        if (!dots) {
            const float tw = std::ceil(MeasureW(t, g_fmtTag.Get())), w = notch + tw + 11;
            if (xEnd + w <= xMax) {
                ComPtr<ID2D1PathGeometry> g;
                ComPtr<ID2D1GeometrySink> s;
                if (SUCCEEDED(g_d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
                    s->BeginFigure(D2D1::Point2F(xEnd, mid), D2D1_FIGURE_BEGIN_FILLED);
                    s->AddLine(D2D1::Point2F(xEnd + notch, top));
                    s->AddLine(D2D1::Point2F(xEnd + w - r, top));
                    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(xEnd + w, top + r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    s->AddLine(D2D1::Point2F(xEnd + w, bot - r));
                    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(xEnd + w - r, bot), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    s->AddLine(D2D1::Point2F(xEnd + notch, bot));
                    s->EndFigure(D2D1_FIGURE_END_CLOSED);
                    s->Close();
                    Color(col, 0.92f * a);
                    g_rt->FillGeometry(g.Get(), g_brush.Get());
                }
                Round(D2D1::RectF(xEnd + notch - 0.5f, mid - 1.5f, xEnd + notch + 2.5f, mid + 1.5f), 1.5f, cBg, 0.9f * a);  // the hole
                // dark text on a light tag, light on a dark one
                const float lum = (0.299f * ((col >> 16) & 255) + 0.587f * ((col >> 8) & 255) + 0.114f * (col & 255)) / 255.f;
                Color(lum > 0.5f ? 0x15151A : 0xF2F2F6, a);
                Text(t, g_fmtTag.Get(), xEnd + notch + 5, top, xEnd + w - 4, bot);
                xEnd += w + 5;
                continue;
            }
            dots = true;  // no room for another label: the rest as dots
        }
        if (xEnd + 7 > xMax) break;
        Round(D2D1::RectF(xEnd, mid - 3.5f, xEnd + 7, mid + 3.5f), 3.5f, col, a);
        xEnd += 10;
    }
    xEnd += 4;
}

// A grid cell: the thumbnail (the small icon until it arrives) and the name under it, two lines at most.
void DrawGridCell(const Entry& e, const wchar_t* name, const std::wstring& full, const D2D1_RECT_F& cell, float y, const Geo& geo,
                  float a, const std::wstring& filterPat) {
    const float box = static_cast<float>(geo.size), boxL = cell.left + kCellPadX / 2, boxT = y + 6;
    bool drawn = false;
    if (Thumb* th = ThumbGet(full, geo.size, e.mtime)) {
        if (ID2D1Bitmap* bmp = ThumbBitmap(*th); bmp && th->w && th->h) {
            const float sc = std::min(1.f, std::min(box / th->w, box / th->h));  // never enlarged: it would blur
            const float w = std::floor(th->w * sc), h = std::floor(th->h * sc);
            const float x0 = std::floor(boxL + (box - w) / 2), y0 = std::floor(boxT + (box - h) / 2);
            if (!th->isIcon) Round(D2D1::RectF(x0 - 1, y0 - 1, x0 + w + 1, y0 + h + 1), 3, 0x000000, 0.35f * a);
            g_rt->DrawBitmap(bmp, D2D1::RectF(x0, y0, x0 + w, y0 + h), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            drawn = true;
        }
    }
    if (!drawn) DrawIcon(full, &e, name, e.nameLen, false, std::floor(boxL + (box - kIcon) / 2), std::floor(boxT + (box - kIcon) / 2), a * 0.8f);
    UINT32 shownLen = e.nameLen;
    if (g_hideExt && !e.isDir())
        for (UINT32 k = e.nameLen; k > 1; --k)
            if (name[k - 1] == L'.') {
                shownLen = k - 1;
                break;
            }
    const D2D1_RECT_F tr = D2D1::RectF(cell.left + 4, boxT + box + 4, cell.right - 4, y + geo.cellH - 4);
    const UINT32 nameCol = NameColorFor(name, e.nameLen, e.isDir(), cText);
    if (!filterPat.empty()) {
        TextHighlighted(std::wstring(name, shownLen), filterPat, g_fmtCell.Get(), tr.left, tr.top, tr.right, tr.bottom, nameCol, cAccent, a);
    } else {
        Color(nameCol, a);
        Text(name, shownLen, g_fmtCell.Get(), tr.left, tr.top, tr.right, tr.bottom);
    }
    float xe;
    DrawTagDots(full, cell.left + 10, y + 12, a, xe);  // the tag dots, in the cell's corner
}

void DrawList(int pane, float H) {
    const float listTop = ListTop(), listBot = H - kStatus;
    Tab& t = PT(pane);
    Columns c = Cols(pane);
    const float L = c.L, R = c.R;
    g_rt->PushAxisAlignedClip(D2D1::RectF(L, listTop, R, listBot), D2D1_ANTIALIAS_MODE_ALIASED);
    const int n = RowCount(t);
    if (t.loading) {
        Color(cDim);
        Text(L"Loading…", g_fmtCenter.Get(), L, listTop, R, listTop + 80);
    } else if (t.list && t.list->error) {
        DrawFolderError(t, L, R, listTop);
    } else if (t.list && n == 0) {
        Color(cDim);
        Text(t.recursive && !t.recDone ? L"Searching…" : !t.filter.empty() ? L"No matches" : L"Empty folder",
             g_fmtCenter.Get(), L, listTop, R, listTop + 80);
    } else if (t.list) {
        const bool focused = (g_test ? g_testActive && !g_renaming && !g_editing : GetFocus() == g_hwnd) && pane == g_act;
        const bool multi = SelCount(t) > 1;
        const bool flat = FlatView(t);
        const int hoverHeader = g_hoverHeader;
        const std::wstring filterPat = ParseFilter(t.filter).fuzzy;  // only the words light up in the names
        wchar_t buf[64];
        const Geo geo = GeoFor(t);
        int first = std::max(0, static_cast<int>(t.scroll / kRow));
        if (geo.grid) {
            EnsureGeo(t);
            first = static_cast<int>(std::partition_point(t.cells.begin(), t.cells.end(), [&](const D2D1_RECT_F& r) { return r.bottom <= t.scroll; }) - t.cells.begin());
        }
        for (int i = first; i < n; ++i) {
            const D2D1_RECT_F cell = RowRect(t, i);
            const float y = listTop + cell.top - t.scroll;
            if (y >= listBot) break;
            const Row& row = t.rows[i];
            if (row.e < 0) {
                // group title: small text and a thin line to the edge
                const auto& gr = row.l->groups[static_cast<size_t>(-1 - row.e)];
                size_t end = static_cast<size_t>(-row.e) < row.l->groups.size() ? row.l->groups[-row.e].first : row.l->entries.size();
                std::wstring cnt = Thousands(static_cast<int>(end - gr.first));
                float tw = MeasureW(gr.second, g_fmtSection.Get());
                const bool gfold = GroupFolded(t, row.l, static_cast<size_t>(-1 - row.e));
                const bool ghov = hoverHeader == i && pane == g_hoverPane;
                if (ghov) Round(D2D1::RectF(L + 6, y + 3, R - 8, y + kRow - 1), 5, 0xFFFFFF, 0.04f);
                Color(ghov ? cDim : cFaint);
                Glyph(gfold ? gChevron : 0xE70D, g_fmtIconSm.Get(), c.nameL, y + 4, c.nameL + kChev, y + kRow);
                const float gx = c.nameL + kChev + 4;
                Color(0xB9B9C2);
                Text(gr.second, g_fmtSection.Get(), gx, y + 4, c.nameR, y + kRow);
                Color(cFaint);
                Text(cnt, g_fmtSection.Get(), gx + tw + 8, y + 4, c.nameR, y + kRow);
                float lx = gx + tw + 8 + MeasureW(cnt, g_fmtSection.Get()) + 10;
                Fill(D2D1::RectF(lx, y + kRow / 2 + 2, R - 14, y + kRow / 2 + 3), cBorder);
                continue;
            }
            const Entry& e = row.l->entries[row.e];
            const wchar_t* name = row.l->name(e);
            const bool drives = row.l->path.empty();
            const D2D1_RECT_F rr = geo.grid ? D2D1::RectF(cell.left + 2, y + 2, cell.right - 2, y + geo.cellH - 2) : D2D1::RectF(L + 6, y + 1, R - 8, y + kRow - 1);
            const bool selected = IsSel(t, i);
            const bool hover = i == g_hoverRow && pane == g_hoverPane;
            if (selected)
                Round(rr, 5, focused ? cSel : 0xFFFFFF, focused ? 1.f : 0.08f);
            else if (hover)
                Round(rr, 5, 0xFFFFFF, 0.045f);
            if (i == t.sel && focused && multi) {
                Color(cAccent, 0.9f);
                g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rr.left + 0.5f, rr.top + 0.5f, rr.right - 0.5f, rr.bottom - 0.5f), 5, 5), g_brush.Get(), 1.f);
            }
            std::wstring full = drives ? std::wstring(name) + L"\\" : Join(row.l->path, name);
            float a = (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && !drives ? 0.5f : 1.f;
            if (!g_cutKeys.empty() && g_cutKeys.count(Key(full))) a *= 0.45f;
            if (i == g_dropRow && pane == g_dropPane) {
                Color(cAccent, 0.9f);
                g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rr.left + 0.5f, rr.top + 0.5f, rr.right - 0.5f, rr.bottom - 0.5f), 5, 5), g_brush.Get(), 1.5f);
            }
            if (geo.grid) {
                DrawGridCell(e, name, full, cell, y, geo, a, filterPat);
                continue;
            }
            const float indent = flat ? 0.f : row.depth * kIndent;
            const float chevX = c.nameL + indent;
            // tree guides: one faint line per level
            for (int d = 1; !flat && d <= row.depth; ++d) {
                float gx = c.nameL + (d - 1) * kIndent + kChev / 2 + 9;
                Fill(D2D1::RectF(gx, y, gx + 1, y + kRow), 0xFFFFFF, 0.06f);
            }
            if (!flat && e.isDir()) {
                bool open = t.expanded.count(Key(Normalize(full))) != 0;
                Color(hover || selected ? cDim : cFaint, a);
                Glyph(open ? 0xE70D : gChevron, g_fmtIconSm.Get(), chevX, y, chevX + kChev, y + kRow);
            }
            const float iconX = chevX + kChev + 2, textX = chevX + kChev + 28;
            // without extension, if so requested (folders are shown in full)
            UINT32 shownLen = e.nameLen;
            if (g_hideExt && !e.isDir() && !drives)
                for (UINT32 k = e.nameLen; k > 1; --k)
                    if (name[k - 1] == L'.') {
                        shownLen = k - 1;
                        break;
                    }
            DrawIcon(full, &e, name, e.nameLen, drives, iconX, y + (kRow - kIcon) / 2, a);
            float nameEnd = c.nameR;
            if (drives) {
                Color(cText, a);
                Text(DriveLabel(name[0]), g_fmtName.Get(), textX, y, c.nameR, y + kRow);
            } else if (!t.filter.empty()) {
                TextHighlighted(std::wstring(name, shownLen), filterPat, g_fmtName.Get(), textX, y, c.nameR, y + kRow, NameColorFor(name, e.nameLen, e.isDir(), cText), cAccent, a);
            } else {
                Color(NameColorFor(name, e.nameLen, e.isDir(), cText), a);
                Text(name, shownLen, g_fmtName.Get(), textX, y, c.nameR, y + kRow);
            }
            float tagEnd = 0;  // the tag labels, right after the name, as far as the column goes
            if (!drives && TagsOf(full)) DrawTagLabels(full, textX + MeasureW(std::wstring(name, shownLen), g_fmtName.Get()) + 8, y + kRow / 2, c.nameR - 3, a, tagEnd);
            // in the flat list (filter or search), where each thing is
            float afterName = std::max(textX + MeasureW(std::wstring(name, shownLen), g_fmtName.Get()), tagEnd) + 14;
            if (e.isDir() && !drives && !g_folderNotes.empty()) afterName = DrawFolderNote(full, afterName, nameEnd, y, a, selected && focused);
            if (flat && row.depth > 0 && row.l->path.size() > t.path.size()) {
                std::wstring rel = row.l->path.substr(t.path.size());
                while (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
                if (afterName < nameEnd - 30) {
                    Color(selected && focused ? 0x9FB3D9 : cFaint, a);
                    Text(rel, g_fmtName.Get(), afterName, y, nameEnd, y + kRow);
                    afterName += MeasureW(rel, g_fmtName.Get()) + 14;
                }
            }
            // content search: the text around the match, with the match in the accent color
            if (!g_grepTerm.empty() && !drives && !e.isDir() && afterName < nameEnd - 40) {
                auto h = g_grepHits.find(Key(full));
                if (h != g_grepHits.end() && h->second.match) {
                    const GrepHit& g = h->second;
                    const std::wstring& s = g.snippet;
                    const int at = std::clamp(g.at, 0, static_cast<int>(s.size())), end = std::min(static_cast<int>(s.size()), at + g.len);
                    float x = afterName;
                    auto part = [&](const std::wstring& txt, UINT32 col) {
                        if (txt.empty() || x >= nameEnd) return;
                        Color(col, a);
                        Text(txt, g_fmtName.Get(), x, y, nameEnd, y + kRow);
                        x += MeasureW(txt, g_fmtName.Get());
                    };
                    part(s.substr(0, at), selected && focused ? 0x9FB3D9 : cFaint);
                    part(s.substr(at, end - at), cAccent);
                    part(s.substr(end), selected && focused ? 0x9FB3D9 : cFaint);
                }
            }
            Color(selected && focused ? 0xC6D4EE : cDim, a);
            if (c.showDate && e.mtime) {
                FormatDate(e.mtime, buf, 64);
                Text(buf, wcslen(buf), g_fmtRight.Get(), c.dateL, y, c.dateL + kDateW, y + kRow);
            }
            if (c.showSize && !e.isDir()) {
                FormatSize(e.size, buf, 64);
                Text(buf, wcslen(buf), g_fmtRight.Get(), c.sizeL, y, c.right, y + kRow);
            } else if (c.showSize && !drives && g_dsAny) {
                DirSize ds;
                if (DirSizeOf(Key(Normalize(full)), ds)) {
                    std::wstring s;
                    if (ds.done || ds.bytes) {
                        FormatSize(ds.bytes, buf, 64);
                        s = buf;
                    }
                    if (!ds.done) s += L"…";
                    if (!ds.done) Color(cFaint, a);
                    Text(s, g_fmtRight.Get(), c.sizeL, y, c.right, y + kRow);
                }
            }
        }
        // thin scrollbar, only when needed
        const float contentH = ContentHeight(t), viewH = listBot - listTop;
        if (contentH > viewH) {
            const float thumbH = std::max(24.f, viewH * viewH / contentH);
            const float thumbY = listTop + (viewH - thumbH) * (t.scroll / (contentH - viewH));
            Round(D2D1::RectF(R - 5, thumbY, R - 2, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.14f);
        }
    }
    DrawBox(pane, listTop, listBot);
    g_rt->PopAxisAlignedClip();
}

void DrawStatus(int pane, float H) {
    const PaneX g = PaneGeo(pane);
    const float y0 = H - kStatus;
    Fill(D2D1::RectF(g.TL, y0, g.TR, H), cSide);
    HLine(g.TL, g.TR, y0);
    Tab& t = PT(pane);
    const bool activePane = pane == g_act;
    const int n = RowCount(t);
    std::wstring left;
    if (!S.message.empty() && activePane)
        left = S.message;
    else if (t.list && !t.list->error) {
        int items = n;
        for (const Row& r : t.rows) items -= r.e < 0;
        // what's inside collapsed groups counts too
        if (!FlatView(t) && !t.list->groups.empty())
            for (size_t gi = 0; gi < t.list->groups.size(); ++gi)
                if (GroupFolded(t, t.list.get(), gi)) {
                    size_t end = gi + 1 < t.list->groups.size() ? t.list->groups[gi + 1].first : t.list->entries.size();
                    items += static_cast<int>(end - t.list->groups[gi].first);
                }
        left = Thousands(items) + (items == 1 ? L" item" : L" items");
        // sizes: the files, plus the folders whose size has been calculated
        auto bytesOf = [&](bool selectedOnly) {
            uint64_t sum = 0;
            for (int i = 0; i < n; ++i) {
                if (IsHeader(t, i) || (selectedOnly && !IsSel(t, i))) continue;
                const Entry& e = EntryAt(t, i);
                if (!e.isDir()) sum += e.size;
                else if (DirSize ds; g_dsAny && DirSizeOf(Key(RowPath(t, i)), ds)) sum += ds.bytes;
            }
            return sum;
        };
        wchar_t buf[64];
        if (!t.recursive && !t.path.empty()) {
            if (uint64_t total = bytesOf(false)) {
                FormatSize(total, buf, 64);
                left += L"  ·  " + std::wstring(buf);
            }
        }
        int s = SelCount(t);
        if (s > 1) {
            left += L"   ·   " + Thousands(s) + L" selected";
            if (uint64_t sel = bytesOf(true)) {
                FormatSize(sel, buf, 64);
                left += L"  ·  " + std::wstring(buf);
            }
        }
    }
    float lx = g.TL + kPad + 2;
    if (!t.filter.empty() || t.recursive) {
        // filter box, File Pilot style: magnifier + text + count
        std::wstring label = t.recursive ? L"in subfolders:" : L"";
        float lw = label.empty() ? 0 : MeasureW(label, g_fmtName.Get()) + 6;
        float tw = MeasureW(t.filter, g_fmtName.Get());
        D2D1_RECT_F box = D2D1::RectF(lx - 4, y0 + 3, lx + 30 + lw + tw + 12, H - 3);
        Round(box, 5, cAccent, 0.18f);
        Color(cAccent);
        Glyph(t.recursive ? 0xE8B7 : 0xE721, g_fmtIconSm.Get(), box.left + 4, box.top, box.left + 24, box.bottom);
        if (!label.empty()) {
            Color(0x9FB3D9);
            Text(label, g_fmtName.Get(), box.left + 26, box.top, box.right, box.bottom);
        }
        Color(cText);
        Text(t.filter, g_fmtName.Get(), box.left + 26 + lw, box.top, box.right, box.bottom);
        if (activePane) Fill(D2D1::RectF(box.left + 26 + lw + tw + 1, box.top + 5, box.left + 26 + lw + tw + 2, box.bottom - 5), cText);
        lx = box.right + 10;
        if (t.recursive) {
            left = (t.filter.empty() ? Thousands(n) + L" items" : Thousands(n) + L" matches") + L" in " +
                   Thousands(t.recDirs) + (t.recDirs == 1 ? L" folder" : L" folders") +
                   (t.recDone ? L"   ·   Esc goes back" : L"   ·   searching…");
        } else {
            left = Thousands(n) + (n == 1 ? L" match" : L" matches") + L"   ·   Esc clears";
        }
        if (!g_grepTerm.empty() && activePane && S.message.empty()) {
            const int pending = g_grepPending.load();
            left += pending ? L"   ·   reading files… " + Thousands(pending) + L" left" : L"   ·   in the files' text";
        }
        if (!S.message.empty() && activePane) left = S.message;
    }
    if (g_dsBusy && activePane && S.message.empty()) left += L"   ·   calculating sizes…";
    if (S.message.empty() || !activePane) {
        if (g_hideDirs) left += L"   ·   no folders";
        if (g_hideFiles) left += L"   ·   no files";
    }
    // timings on the right, only if they fit without overlapping the text
    std::wstring right;
    float rw = 0;
    if (activePane && S.show && S.navMs >= 0) {
        right = L"open " + Ms(S.navMs) + L"   ·   ";
        right += S.fromCache ? L"from memory" : L"disk " + Ms(S.enumMs) + L" + sort " + Ms(S.sortMs);
        rw = MeasureW(right, g_fmtStatusR.Get());
        if (lx + MeasureW(left, g_fmtStatus.Get()) + 24 + rw > g.TR - kPad) right.clear();
    }
    Color(S.message.empty() || !activePane ? cDim : cAccent);
    Text(left, g_fmtStatus.Get(), lx, y0, right.empty() ? g.TR - kPad : g.TR - kPad - rw - 24, H);
    if (!right.empty()) {
        Color(cFaint);
        Text(right, g_fmtStatusR.Get(), g.TR - kPad - rw - 4, y0, g.TR - kPad - 2, H);
    }
}

void DrawPreview(float W, float H);     // preview.inl
void DrawViewEditor(float W, float H);  // viewedit.inl
void DrawInstallPanel(float W, float H);  // install.inl
void DrawRenamePanel(float W, float H);   // rename.inl

// Milliseconds since the process started (includes loading the executable).
double MsSinceProcessStart() {
    FILETIME created, a, b, c, now;
    GetProcessTimes(GetCurrentProcess(), &created, &a, &b, &c);
    GetSystemTimePreciseAsFileTime(&now);
    auto ticks = [](const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(ticks(now) - ticks(created)) / 10000.0;
}
double g_firstFrameMs = 0;
std::vector<std::pair<const wchar_t*, double>> g_bootMarks;
void BootMark(const wchar_t* what) { g_bootMarks.push_back({what, MsSinceProcessStart()}); }

void Render() {
    if (!EnsureTarget()) return;
    EnsureImageList();
    if (g_iconFolder < 0) g_iconFolder = IconByAttrs(L"folder", FILE_ATTRIBUTE_DIRECTORY);
    if (g_iconFile < 0) g_iconFile = IconByAttrs(L"file", FILE_ATTRIBUTE_NORMAL);
    g_hits.clear();
    g_crumbPaths.clear();
    g_notePaths.clear();
    D2D1_SIZE_F sz = g_rt->GetSize();
    const float W = sz.width, H = sz.height;

    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(cBg));
    DrawTabs(W);
    for (int p = 0; p < PaneCount(); ++p) DrawToolbar(p);
    DrawSidebar(H);
    for (int p = 0; p < PaneCount(); ++p) {
        DrawHeader(p);
        DrawList(p, H);
        DrawStatus(p, H);
    }
    if (g_split) {
        // divider between panes: drag to change the width (double click: half and half)
        const float x = PaneGeo(1).L;
        Fill(D2D1::RectF(x, kTabH, x + 1, H), cBorder);
        g_hits.push_back({D2D1::RectF(x - 3, kTabH + kToolH, x + 4, H - kStatus), HitKind::Divider, 0});
    }
    DrawPreview(W, H);
    DrawViewEditor(W, H);
    DrawPalette(W, H);
    DrawInstallPanel(W, H);
    DrawRenamePanel(W, H);
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseTarget();
    if (g_firstFrameMs == 0) g_firstFrameMs = MsSinceProcessStart();

    // Time from key/click to finished frame. Shown on the next frame.
    if (S.navPending && !T().loading) {
        S.navMs = NowMs() - S.navStart;
        S.navPending = false;
        Invalidate();
    }
}

// ---------------------------------------------------------------- editable path bar
HFONT g_editFont = nullptr;
HBRUSH g_editBrush = nullptr;
HWND g_renameEdit = nullptr;

void LayoutEdit() {
    if (!g_edit) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    auto px = [](float dip) { return MulDiv(static_cast<int>(dip), g_dpi, 96); };
    (void)rc;
    const PaneX g = PaneGeo(g_act);
    int left = px(g.TL + kPad + 3 * 32 + 8 + 10), right = px(g.TR - kPad - 8);
    // as in rename: the EDIT draws text flush to the top, so it gets exactly the height of the
    // text and centered in the bar (like the breadcrumbs, which DirectWrite centers vertically)
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_editFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    int h = tm.tmHeight + 2;
    int y = MulDiv(static_cast<int>(std::lround((kTabH + kToolH / 2) * 100)), g_dpi, 9600) - h / 2 + px(1);  // +1: DirectWrite leaves the line spacing on top
    MoveWindow(g_edit, left, y, std::max(10, right - left), h, TRUE);
}

void UpdateEditFont() {
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-MulDiv(static_cast<int>(g_cfg.fontSize), g_dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, g_cfg.font.c_str());
    SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    if (g_renameEdit) {
        SendMessageW(g_renameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_editFont), TRUE);
        SendMessageW(g_renameEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    }
}

void ShowPathEdit() {
    SetWindowTextW(g_edit, T().path.c_str());
    LayoutEdit();
    g_editing = true;
    ShowWindow(g_edit, SW_SHOW);
    SetFocus(g_edit);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    Invalidate();
}

void HidePathEdit() {
    if (!g_editing) return;
    g_editing = false;
    ShowWindow(g_edit, SW_HIDE);
    SetFocus(g_hwnd);
    Invalidate();
}

void CommitPathEdit() {
    wchar_t text[4096];
    GetWindowTextW(g_edit, text, 4096);
    HidePathEdit();
    std::wstring t = text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') t = t.substr(1, t.size() - 2);
    if (t.empty() || _wcsicmp(t.c_str(), L"This PC") == 0) {
        Navigate(T(), L"", true);
        return;
    }
    wchar_t expanded[4096];
    ExpandEnvironmentStringsW(t.c_str(), expanded, 4096);
    t = expanded;
    if (t.size() == 2 && t[1] == L':') t += L'\\';
    wchar_t full[4096];
    if (!GetFullPathNameW(t.c_str(), 4096, full, nullptr)) return;
    std::wstring p = Normalize(full);
    DWORD a = GetFileAttributesW(ToExtendedPath(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        S.message = L"Not found: " + p;
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        Navigate(T(), p, true);
    } else {
        std::wstring parent = ParentOf(p);
        g_lastSel[Key(parent)] = LeafOf(p);
        Navigate(T(), parent, true);
    }
}

LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        CommitPathEdit();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        HidePathEdit();
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
    if (m == WM_KILLFOCUS) HidePathEdit();
    return DefSubclassProc(h, m, w, l);
}

// ---------------------------------------------------------------- renaming in the list (F2)
std::wstring g_renameOld;  // original name
std::wstring g_renameDir;  // the item's folder (in the tree it may not be the tab's)

// The EDIT draws text flush to the top: it gets exactly the text's height and is centered in the row.
int RenameEditH() {
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_editFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    return tm.tmHeight + 2;
}

void LayoutRename() {
    if (!g_renaming) return;
    auto px = [](float dip) { return MulDiv(static_cast<int>(std::lround(dip)), g_dpi, 96); };
    const int editH = RenameEditH();
    if (g_favRename >= 0) {
        for (const Hit& h : g_hits) {
            if (h.kind != HitKind::Side || h.idx != g_favRename) continue;
            const int left = px(h.r.left + 32), right = px(h.r.right - 4);
            SetWindowPos(g_renameEdit, nullptr, left, px(h.r.top) + (px(kSideRow) - editH) / 2, std::max(40, right - left), editH, SWP_NOZORDER | SWP_SHOWWINDOW);
            break;
        }
        return;
    }
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return;
    Columns c = Cols(g_act);
    const Geo geo = GeoFor(t);
    const D2D1_RECT_F cell = RowRect(t, t.sel);
    float y = ListTop() + cell.top - t.scroll;
    int top, left, right;
    if (geo.grid) {  // under the image, as wide as the cell
        top = px(y + 6 + geo.size + 4);
        left = px(cell.left + 4);
        right = px(cell.right - 4);
    } else {
        int rowTop = px(y), rowH = px(kRow);
        top = rowTop + (rowH - editH) / 2;
        left = px(RowTextX(c, t, t.sel) - 2);
        right = px(c.nameR);
    }
    bool visible = y + (cell.bottom - cell.top) > ListTop() && y < ListBottom();
    SetWindowPos(g_renameEdit, nullptr, left, top, std::max(40, right - left), editH,
                 SWP_NOZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void BeginRename() {
    Tab& t = T();
    if (g_renaming || !t.list || t.list->error || t.sel < 0 || t.sel >= Count()) return;
    if (RowDir(t, t.sel).empty()) return;  // drives
    g_renameOld = SelectedName(t);
    g_renameDir = RowDir(t, t.sel);
    g_renaming = true;
    SetWindowTextW(g_renameEdit, g_renameOld.c_str());
    LayoutRename();
    SetFocus(g_renameEdit);
    // like File Explorer: the name is selected without the extension
    int selEnd = static_cast<int>(g_renameOld.size());
    if (!EntryAt(t, t.sel).isDir()) {
        size_t dot = g_renameOld.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) selEnd = static_cast<int>(dot);
    }
    SendMessageW(g_renameEdit, EM_SETSEL, 0, selEnd);
    Invalidate();
}

void BeginFavRename(int idx) {
    if (g_renaming || idx < 0 || idx >= static_cast<int>(g_favs.size())) return;
    g_favRename = idx;
    g_renameOld = g_favs[idx].name;
    g_renaming = true;
    SetWindowTextW(g_renameEdit, g_renameOld.c_str());
    LayoutRename();
    SetFocus(g_renameEdit);
    SendMessageW(g_renameEdit, EM_SETSEL, 0, -1);
    Invalidate();
}

void CancelRename() {
    if (!g_renaming) return;
    g_renaming = false;
    g_favRename = -1;
    ShowWindow(g_renameEdit, SW_HIDE);
    if (GetFocus() == g_renameEdit) SetFocus(g_hwnd);
    Invalidate();
}

// follow: the cursor follows the new name when the list reloads.
void CommitRename(bool follow = true) {
    if (!g_renaming) return;
    wchar_t buf[1024];
    GetWindowTextW(g_renameEdit, buf, 1024);
    std::wstring newName = buf;
    while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'.')) newName.pop_back();
    if (g_favRename >= 0) {  // a favorite is just a label: the folder isn't touched
        const int i = g_favRename;
        CancelRename();
        std::erase(newName, L'|');
        if (!newName.empty() && i < static_cast<int>(g_favs.size())) {
            g_favs[i].name = newName;
            SaveFavs();
        }
        return;
    }
    std::wstring oldName = g_renameOld, dir = g_renameDir;
    Tab& t = T();
    CancelRename();
    if (newName.empty() || newName == oldName) return;
    if (newName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        S.message = L"Names can't contain \\ / : * ? \" < > |";
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    std::wstring from = ToExtendedPath(Join(dir, oldName.c_str()));
    std::wstring to = ToExtendedPath(Join(dir, newName.c_str()));
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        S.message = L"Couldn't rename: " + ErrorText(GetLastError());
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    UndoRecordRename(Join(dir, oldName.c_str()), Join(dir, newName.c_str()));
    if (follow) t.pendingSelect = Key(dir) == Key(t.path) ? newName : Join(dir, newName.c_str());
    TreeFollow(t, Join(dir, oldName.c_str()), Join(dir, newName.c_str()));
    TagsFollow(Join(dir, oldName.c_str()), Join(dir, newName.c_str()));
    Enqueue(dir, JobKind::Revalidate);
}

// Tab / Shift+Tab while renaming: saves and moves on to renaming the next / previous item.
void RenameStep(int delta) {
    if (g_favRename >= 0) {
        CommitRename();
        return;
    }
    Tab& t = T();
    int next = t.sel + delta;
    CommitRename(false);
    if (next < 0 || next >= Count()) return;
    MoveCursor(next);
    BeginRename();
}

LRESULT CALLBACK RenameProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        CommitRename();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        CancelRename();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_TAB) {
        RenameStep(KeyDown(VK_SHIFT) ? -1 : 1);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;
    if (m == WM_KILLFOCUS && g_renaming) CommitRename();
    return DefSubclassProc(h, m, w, l);
}

// Free name in a folder: "base ext", "base (2) ext", ...
std::wstring FreeName(const std::wstring& dir, const std::wstring& base, const std::wstring& ext) {
    std::wstring name = base + ext;
    for (int i = 2; GetFileAttributesW(ToExtendedPath(Join(dir, name.c_str())).c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        name = base + L" (" + std::to_wstring(i) + L")" + ext;
    return name;
}

// New folder or file with a unique name, and immediate rename.
void NewItem(bool folder) {
    Tab& t = T();
    if (t.path.empty() || !t.list || t.list->error) return;
    std::wstring name = folder ? FreeName(t.path, L"New folder", L"") : FreeName(t.path, L"New file", L".txt");
    std::wstring full = ToExtendedPath(Join(t.path, name.c_str()));
    bool ok;
    if (folder) {
        ok = CreateDirectoryW(full.c_str(), nullptr) != 0;
    } else {
        HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) CloseHandle(h);
    }
    if (!ok) {
        S.message = std::wstring(folder ? L"Couldn't create the folder: " : L"Couldn't create the file: ") +
                    ErrorText(GetLastError());
        Invalidate();
        return;
    }
    UndoRecordCreate(Join(t.path, name.c_str()));
    if (!t.filter.empty()) {
        t.filter.clear();
        BuildRows(t);
    }
    t.pendingSelect = name;
    g_renameAfterSelect = name;
    Enqueue(t.path, JobKind::Revalidate);
}
void NewFolder() { NewItem(true); }

int HitRow(float x, float y);
int HitRowIn(int pane, float x, float y);
Hit HitAt(float x, float y);
RECT ScreenRect(const D2D1_RECT_F& r);
#include "undo.inl"
#include "menu.inl"
#include "dragdrop.inl"
#include "boxselect.inl"
#include "sidebar.inl"
void RunCommand(Cmd cmd);
void SetFoldersFirst(int mode);
void ViewEditorOpen(int pane);
#include "indexer.inl"
#include "palette.inl"
#include "viewedit.inl"
#include "ops.inl"
#include "search.inl"
#include "syntax.inl"
#include "grep.inl"
#include "preview.inl"
void PreviewReleaseBitmap() { g_pv.d2d.Reset(); }
#include "background.inl"
#include "install.inl"
#include "rename.inl"
#include "testmode.inl"

// ---------------------------------------------------------------- session (tabs, window, panes)
std::wstring SessionFile() {
    std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\') + 1) + L"session.txt";
}
std::wstring RecentsFile() {
    std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\') + 1) + L"recents.txt";
}

void SaveSession() {
    if (g_adminWindow) return;  // the session is the normal window's
    std::wstring f = SessionFile();
    if (f.empty() || g_tabs.empty()) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(g_hwnd, &wp);
    std::ofstream out(f, std::ios::binary);
    out << "# Jolt session. Rewritten automatically; safe to delete.\n";
    const RECT& r = wp.rcNormalPosition;
    out << "window=" << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << ','
        << (IsZoomed(g_hwnd) || g_showMaxOnce ? 1 : 0) << '\n';
    out << "sidebar=" << (g_sideVisible ? 1 : 0) << '\n';
    out << "preview=" << (g_preview ? 1 : 0) << '\n';
    out << "show=" << (g_hideDirs ? 1 : 0) << ',' << (g_hideFiles ? 1 : 0) << ',' << (g_hideExt ? 1 : 0) << '\n';
    out << "sort=" << g_sortBy.load() << ',' << (g_sortAsc.load() ? 1 : 0) << '\n';
    out << "split=" << (g_split ? 1 : 0) << ',' << static_cast<int>(g_splitRatio * 1000) << '\n';
    g_paneCur[g_act] = g_cur;
    out << "active=" << g_cur << '\n';
    out << "visible=" << g_paneCur[0] << ',' << g_paneCur[1] << '\n';
    for (auto& t : g_tabs) out << (t->pane ? "tab2=" : "tab=") << Utf8(t->path) << '\n';
    std::wstring rf = RecentsFile();
    if (!rf.empty()) {
        std::ofstream ro(rf, std::ios::binary);
        for (const std::wstring& p : g_recents) ro << Utf8(p) << '\n';
    }
    HeatSave();
}

struct Session {
    std::vector<std::pair<std::wstring, int>> tabs;  // path, pane
    int active = 0, visible[2] = {-1, -1};
    bool hasWindow = false;
    RECT rc{};
    bool maximized = false;
    bool sidebar = true, preview = false, split = false;
    float ratio = 0.5f;
    int sortBy = 0;
    bool sortAsc = true;
};

Session LoadSession() {
    Session s;
    std::ifstream in(SessionFile(), std::ios::binary);
    std::string line;
    while (in && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "tab") s.tabs.push_back({Wide(v), 0});
        else if (k == "tab2") s.tabs.push_back({Wide(v), 1});
        else if (k == "active") s.active = atoi(v.c_str());
        else if (k == "visible") sscanf_s(v.c_str(), "%d,%d", &s.visible[0], &s.visible[1]);
        else if (k == "sidebar") s.sidebar = v != "0";
        else if (k == "preview") s.preview = v != "0";
        else if (k == "show") {
            int a = 0, b = 0, c = 0;
            sscanf_s(v.c_str(), "%d,%d,%d", &a, &b, &c);
            g_hideDirs = a != 0;
            g_hideFiles = b != 0 && !g_hideDirs;
            g_hideExt = c != 0;
        }
        else if (k == "split") {
            int on = 0, ratio = 500;
            sscanf_s(v.c_str(), "%d,%d", &on, &ratio);
            s.split = on != 0;
            s.ratio = std::clamp(ratio / 1000.f, 0.15f, 0.85f);
        } else if (k == "sort") {
            s.sortBy = atoi(v.c_str());
            size_t c = v.find(',');
            s.sortAsc = c == std::string::npos || v.substr(c + 1) != "0";
        } else if (k == "window") {
            int a[5] = {};
            if (sscanf_s(v.c_str(), "%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4]) == 5) {
                s.rc = {a[0], a[1], a[2], a[3]};
                s.maximized = a[4] != 0;
                s.hasWindow = s.rc.right > s.rc.left + 100 && s.rc.bottom > s.rc.top + 100;
            }
        }
    }
    std::ifstream ri(RecentsFile(), std::ios::binary);
    while (ri && std::getline(ri, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && g_recents.size() < 300) g_recents.push_back(Wide(line));
    }
    HeatLoad();
    return s;
}

constexpr UINT_PTR kTimerSession = 3;
void TouchSession() { SetTimer(g_hwnd, kTimerSession, 2000, nullptr); }  // saves a while after the last change

// Reloads everything in %APPDATA%\Jolt (config, favorites, menu) after a change on disk.
constexpr UINT_PTR kTimerConfig = 4;
// Signature (date and size) of the files the user edits; session.txt and recents.txt don't count.
std::wstring ConfigSignature() {
    std::wstring sig;
    for (const wchar_t* n : {L"config.txt", L"favorites.txt", L"menu.txt", L"views.txt", L"tags.txt"}) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (GetFileAttributesExW((ConfigDir() + L"\\" + n).c_str(), GetFileExInfoStandard, &d))
            sig += std::to_wstring(d.ftLastWriteTime.dwLowDateTime) + L"." + std::to_wstring(d.nFileSizeLow) + L";";
        else
            sig += L"-;";
    }
    return sig;
}
std::wstring g_configSig;
void RefreshConfigSig() { g_configSig = ConfigSignature(); }

void BgApplyConfig();  // background.inl

void ReloadConfig() {
    std::wstring sig = ConfigSignature();
    if (sig == g_configSig) return;  // something else in the folder changed (the session, the recents)
    g_configSig = sig;
    const UINT oldDpi = g_dpi;
    LoadConfig();
    ApplyDpi();
    if (g_dpi != oldDpi) ReleaseTarget();
    LoadFavs();
    HeatRefresh();
    LoadMenuCfg();
    LoadViews();
    LoadTags();
    SyncGlobalSort();
    CreateFormats();
    UpdateEditFont();
    ResortAll();
    for (auto& tp : g_tabs) ClampScroll(*tp);
    BgApplyConfig();
    LayoutEdit();
    LayoutRename();
    S.message = L"Config reloaded";
    Invalidate();
}

// Zoom of the whole interface; saved in config.txt.
void SetUiZoom(int px) {
    px = std::clamp(px, kUiPxMin, kUiPxMax);
    const int pct = (px * 100 + 6) / 12;
    if (px != g_uiPx) {
        g_uiPx = px;
        ApplyDpi();
        ReleaseTarget();
        UpdateEditFont();
        for (auto& tp : g_tabs) ClampScroll(*tp);
        EnsureVisible(T());
        LayoutEdit();
        LayoutRename();
        ConfigSetValue(L"general", L"scale", std::to_wstring(pct));
        RefreshConfigSig();
    }
    S.message = L"Interface at " + std::to_wstring(pct) + L"%";
    Invalidate();
}

// Row spacing; saved in config.txt.
void SetRowHeight(float h) {
    h = std::clamp(h, 18.f, 60.f);
    if (h != kRow) {
        const float ratio = h / kRow;
        kRow = h;
        for (auto& tp : g_tabs) {
            tp->scroll *= ratio;  // keep showing the same thing
            ClampScroll(*tp);
        }
        EnsureVisible(T());
        LayoutRename();
        ConfigSetValue(L"general", L"row_height", std::to_wstring(static_cast<int>(h)));
        RefreshConfigSig();
    }
    S.message = L"Row height " + std::to_wstring(static_cast<int>(kRow)) + L" px";
    Invalidate();
}

// Collapses or expands all groups, for this visit only: the view doesn't change.
void FoldAllGroups(Tab& t, bool folded) {
    if (!t.list || t.list->groups.empty()) {
        S.message = L"This folder has no groups";
        Invalidate();
        return;
    }
    Place p = SavePlace(t);
    for (auto& g : t.list->groups) t.fold[g.second] = folded;
    Rebuild(t, &p, L"");
    if (!folded) t.scroll = 0;  // when expanding everything, start from the top
    EnsureVisible(t);
    Invalidate();
}

void ToggleSidebar() {
    g_sideVisible = !g_sideVisible;
    for (auto& tp : g_tabs) ClampScroll(*tp);
    LayoutEdit();
    LayoutRename();
    Invalidate();
    TouchSession();
}

// Checks the visible folders and their expanded subfolders in the background.
void RevalidateVisible() {
    for (int p = 0; p < PaneCount(); ++p) {
        Tab& t = PT(p);
        if (!t.has) continue;
        Enqueue(t.path, JobKind::Revalidate);
        for (auto& [k, path] : t.expanded) Enqueue(path, JobKind::Revalidate);
    }
}

// Runs a command from the shortcut table.
void RunCommand(Cmd cmd) {
    Tab& t = T();
    if (IsCustom(cmd)) {
        RunCustomCommand(CustomIndex(cmd));
        return;
    }
    switch (cmd) {
        case Cmd::None: break;
        case Cmd::Up:
            if (t.recursive) StopRecursive(t, true);
            else if (!Collapse()) GoUp();
            break;
        case Cmd::Enter:
            if (t.sel >= 0 && t.sel < Count() && EntryAt(t, t.sel).isDir()) OpenSelected(false);
            break;
        case Cmd::Open: OpenSelected(false); break;
        case Cmd::OpenTab: OpenSelected(true); break;
        case Cmd::Back: GoBack(); break;
        case Cmd::Forward: GoForward(); break;
        case Cmd::Rename:
            if (SelCount(t) > 1) RenameManyOpen();
            else BeginRename();
            break;
        case Cmd::RenameMany: RenameManyOpen(); break;
        case Cmd::Delete: DeleteSelection(); break;
        case Cmd::NewFolder: NewItem(true); break;
        case Cmd::NewFile: NewItem(false); break;
        case Cmd::Copy:
            if (ClipboardSetFiles(SelectedFullPaths(t), false)) {
                g_ignoreClipUpdate = true;
                g_cutKeys.clear();
                Invalidate();
            }
            break;
        case Cmd::Cut: {
            auto paths = SelectedFullPaths(t);
            if (ClipboardSetFiles(paths, true)) {
                g_ignoreClipUpdate = true;
                g_cutKeys.clear();
                for (const auto& p : paths) g_cutKeys.insert(Key(p));
                Invalidate();
            }
            break;
        }
        case Cmd::Paste:
            if (!IsClipboardFormatAvailable(CF_HDROP)) PasteClipboardAsFile();
            else if (ClipboardPaste(t.path) && !g_cutKeys.empty()) {
                g_cutKeys.clear();
                Invalidate();
            }
            break;
        case Cmd::SelectAll:
            SelectAll(t);
            Invalidate();
            break;
        case Cmd::Favorite: ToggleFavorite(); break;
        case Cmd::NewTab: NewTab(t.path); break;
        case Cmd::CloseTab: CloseTab(g_cur); break;
        case Cmd::NextTab: CycleTab(1); break;
        case Cmd::PreviousTab: CycleTab(-1); break;
        case Cmd::Path: ShowPathEdit(); break;
        case Cmd::Sidebar: ToggleSidebar(); break;
        case Cmd::Reload: RevalidateVisible(); break;
        case Cmd::Timings:
            S.show = !S.show;
            Invalidate();
            break;
        case Cmd::ContextMenu: {
            int r = Count() > 0 ? t.sel : -1;
            float y = r >= 0 ? ListTop() + (RowRect(t, r).top + RowRect(t, r).bottom) / 2 - t.scroll : ListTop() + 40;
            float x = PaneGeo(g_act).L + 60;
            RECT sr = ScreenRect(D2D1::RectF(x, y, x, y));
            OpenShellMenu(POINT{sr.left, sr.top}, r);
            break;
        }
        case Cmd::AppMenu: {
            RECT r = ScreenRect(D2D1::RectF(6, 7, 38, kTabH - 3));
            OpenAppMenu(r);
            break;
        }
        case Cmd::Palette: PaletteOpen(0); break;
        case Cmd::GoTo: PaletteOpen(1); break;
        case Cmd::SearchSubfolders:
            if (t.recursive) StopRecursive(t, true);
            else StartRecursive(t);
            break;
        case Cmd::Expand: SetExpanded(t, t.sel, !IsExpanded(t, t.sel)); break;
        case Cmd::Collapse: Collapse(); break;
        case Cmd::GoToLocation: GoToLocation(); break;
        case Cmd::Preview: PreviewToggle(); break;
        case Cmd::Split: SetSplit(!g_split); break;
        case Cmd::OtherPane: ActivatePane(1 - g_act); break;
        case Cmd::CopyToOther: TransferToOther(false); break;
        case Cmd::MoveToOther: TransferToOther(true); break;
        case Cmd::CopyPath: CopyPathText(CopyWhat::Path); break;
        case Cmd::CopyName: CopyPathText(CopyWhat::Name); break;
        case Cmd::CopyPathUnix: CopyPathText(CopyWhat::Unix); break;
        case Cmd::Duplicate: Duplicate(); break;
        case Cmd::GroupInFolder: GroupInFolder(); break;
        case Cmd::Install: InstPanelOpen(); break;
        case Cmd::Quit: BgQuit(); break;
        case Cmd::Undo: UndoStep(true); break;
        case Cmd::Redo: UndoStep(false); break;
        case Cmd::Properties: Properties(); break;
        case Cmd::FolderSizes: ComputeFolderSizes(); break;
        case Cmd::ShowFolders:
            g_hideDirs = !g_hideDirs;
            if (g_hideDirs) g_hideFiles = false;  // never both at once: the list would be empty
            S.message = g_hideDirs ? L"No folders: files only" : L"Folders visible";
            ResortAll();
            break;
        case Cmd::ShowFiles:
            g_hideFiles = !g_hideFiles;
            if (g_hideFiles) g_hideDirs = false;
            S.message = g_hideFiles ? L"No files: folders only" : L"Files visible";
            ResortAll();
            break;
        case Cmd::ShowHidden:
            g_cfg.showHidden = !g_cfg.showHidden;
            ConfigSetValue(L"general", L"show_hidden", g_cfg.showHidden ? L"yes" : L"no");
            RefreshConfigSig();
            S.message = g_cfg.showHidden ? L"Showing hidden files" : L"Hiding hidden files";
            ResortAll();
            break;
        case Cmd::ShowExtensions:
            g_hideExt = !g_hideExt;
            S.message = g_hideExt ? L"Extensions hidden" : L"Extensions visible";
            TouchSession();
            Invalidate();
            break;
        case Cmd::ReopenTab: ReopenTab(); break;
        case Cmd::CloseOthers: CloseOtherTabs(); break;
        case Cmd::Terminal: OpenTerminal(); break;
        case Cmd::OpenExplorer: OpenInExplorer(); break;
        case Cmd::RealLocation: GoToRealLocation(); break;
        case Cmd::CollapseGroups: FoldAllGroups(t, true); break;
        case Cmd::ExpandGroups: FoldAllGroups(t, false); break;
        case Cmd::GroupByDay: SetGroup(GbDay); break;
        case Cmd::GroupByType: SetGroup(GbType); break;
        case Cmd::NoGroups: SetGroup(-1); break;
        case Cmd::SortByType: SetSortField(SfExt, true); break;
        case Cmd::ZoomIn: SetUiZoom(g_uiPx + 1); break;
        case Cmd::ZoomOut: SetUiZoom(g_uiPx - 1); break;
        case Cmd::ZoomReset: SetUiZoom(12); break;
        case Cmd::RowsTaller: SetRowHeight(kRow + 1); break;
        case Cmd::RowsShorter: SetRowHeight(kRow - 1); break;
        case Cmd::FolderSiblings: CrumbMenu(CrumbSiblingsDir(t.path), g_crumbLast[g_act]); break;
        case Cmd::OpenAsAdmin:
            FolderNoteAct(t.sel >= 0 && t.sel < Count() && EntryAt(t, t.sel).isDir() ? RowPath(t, t.sel) : t.path);
            break;
        case Cmd::PreviewWrap:
            g_cfg.previewWrap = !g_cfg.previewWrap;
            ConfigSetValue(L"general", L"preview_wrap", g_cfg.previewWrap ? L"yes" : L"no");
            RefreshConfigSig();
            S.message = g_cfg.previewWrap ? L"Preview: long lines wrap" : L"Preview: long lines run off the edge";
            Invalidate();
            break;
        case Cmd::Tag: TagPaletteOpen(); break;
        case Cmd::ViewList: SetLayout(0); break;
        case Cmd::ViewGrid: if (!LayoutOf(t)) SetLayout(144); break;
        case Cmd::ViewBigger: StepLayout(1); break;
        case Cmd::ViewSmaller: StepLayout(-1); break;
        case Cmd::EmptyRecycleBin: EmptyRecycleBin(); break;
    }
}

// ---------------------------------------------------------------- window
float DipX(LPARAM l) { return GET_X_LPARAM(l) * 96.f / g_dpi; }

RECT ScreenRect(const D2D1_RECT_F& r) {
    POINT a{MulDiv(static_cast<int>(r.left), g_dpi, 96), MulDiv(static_cast<int>(r.top), g_dpi, 96)};
    POINT b{MulDiv(static_cast<int>(r.right), g_dpi, 96), MulDiv(static_cast<int>(r.bottom), g_dpi, 96)};
    ClientToScreen(g_hwnd, &a);
    ClientToScreen(g_hwnd, &b);
    return {a.x, a.y, b.x, b.y};
}
float DipY(LPARAM l) { return GET_Y_LPARAM(l) * 96.f / g_dpi; }

// Row under the point in a pane (regardless of which is active), or -1.
// Row under the point, including group titles, or -1.
int RawRowIn(int pane, float x, float y) {
    if (pane < 0 || pane >= PaneCount()) return -1;
    PaneX g = PaneGeo(pane);
    if (x < g.L || x >= g.R || y < ListTop() || y >= ListBottom()) return -1;
    Tab& t = PT(pane);
    return RowAtContent(t, x, y - ListTop() + t.scroll);
}
int HitRowIn(int pane, float x, float y) {
    int r = RawRowIn(pane, x, y);
    return r >= 0 && !IsHeader(PT(pane), r) ? r : -1;
}
int HeaderRowIn(int pane, float x, float y) {
    int r = RawRowIn(pane, x, y);
    return r >= 0 && IsHeader(PT(pane), r) ? r : -1;
}

// Click on a group title: collapses or expands it.
void ToggleGroup(Tab& t, int row) {
    const Row& r = t.rows[row];
    const Listing* l = r.l;
    const int gi = -1 - r.e;
    const bool opening = GroupFolded(t, l, static_cast<size_t>(gi));
    t.fold[l->groups[gi].second] = !opening;
    Place p = SavePlace(t);
    const float scroll = t.scroll;
    Rebuild(t, &p, L"");
    t.scroll = scroll;  // the title stays where it was (the cursor isn't followed)
    if (opening) {
        // show the whole group, starting from its title
        int h = -1, e = RowCount(t);
        for (int i = 0; i < RowCount(t); ++i)
            if (t.rows[i].l == l && t.rows[i].e == -1 - gi) {
                h = i;
                for (int j = i + 1; j < RowCount(t); ++j)
                    if (IsHeader(t, j)) {
                        e = j;
                        break;
                    }
                break;
            }
        if (h >= 0) {
            const float top = RowRect(t, h).top, bottom = e < RowCount(t) ? RowRect(t, e).top : ContentHeight(t), view = ListHeight();
            if (top < t.scroll) t.scroll = top;
            if (bottom > t.scroll + view) t.scroll = std::min(top, bottom - view);
        }
    }
    ClampScroll(t);
    Invalidate();
}
// Row under the point in the active pane.
int HitRow(float x, float y) { return HitRowIn(g_act, x, y); }

Hit HitAt(float x, float y) {
    // the last ones added (most specific) take priority
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return *it;
    return {{}, HitKind::None, -1};
}

// Is the point over the chevron (expand) of a folder row?
bool OnChevron(int row, float x) {
    Tab& t = T();
    if (row < 0 || row >= Count() || FlatView(t) || !EntryAt(t, row).isDir()) return false;
    Columns c = Cols(g_act);
    float cx = c.nameL + RowIndent(t, row);
    return x >= cx - 4 && x < cx + kChev + 2;
}

bool g_divDrag = false;  // dragging the split view divider
bool g_eatSysChar = false;  // the Alt key combo was already used: its WM_SYSCHAR must not beep
int g_tabWheelAcc = 0;      // Ctrl+wheel on high-resolution wheels: accumulates until a full step

// New width of a column (1 = date, 2 = size), always leaving room for the name.
void SetColWidth(int which, float w, int pane) {
    const PaneX g = PaneGeo(pane);
    const float room = g.R - g.L - 350;
    if (which == 1) kDateW = std::clamp(w, kDateMin, std::max(kDateMin, std::min(kDateMax, room - kSizeW)));
    else kSizeW = std::clamp(w, kSizeMin, std::max(kSizeMin, std::min(kSizeMax, room - (Cols(pane).showDate ? kDateW : -42.f))));
    LayoutRename();
    Invalidate();
}

void SaveColWidth(int which) {
    ConfigSetValue(L"columns", which == 1 ? L"modified_width" : L"size_width",
                   std::to_wstring(std::lround(which == 1 ? kDateW : kSizeW)));
    RefreshConfigSig();
}

void ColDragEnd() {
    if (!g_colDrag) return;
    const int which = g_colDrag;
    g_colDrag = 0;
    SaveColWidth(which);
    Invalidate();
}

void SetPreviewW(float w) {
    if (!g_preview) return;
    g_previewW = std::round(w);
    g_previewW = PreviewW();  // what is saved is the width really shown
    LayoutEdit();
    Invalidate();
}

void SavePreviewW() {
    ConfigSetValue(L"general", L"preview_width", std::to_wstring(std::lround(g_previewW)));
    RefreshConfigSig();
}

void PreviewDragEnd() {
    if (!g_pvDrag) return;
    g_pvDrag = false;
    SavePreviewW();
    Invalidate();
}

// Double click on the edge: exactly the width needed for the folder's contents.
void FitColumn(int which, int pane) {
    Tab& t = PT(pane);
    float w = MeasureW(which == 1 ? L"Modified" : L"Size", g_fmtRight.Get()) + 16;
    wchar_t buf[64];
    const int n = std::min(RowCount(t), 3000);
    for (int i = 0; i < n; ++i) {
        if (IsHeader(t, i)) continue;
        const Entry& e = EntryAt(t, i);
        buf[0] = 0;
        if (which == 1) {
            if (e.mtime) FormatDate(e.mtime, buf, 64);
        } else if (!e.isDir()) {
            FormatSize(e.size, buf, 64);
        } else if (g_dsAny) {
            DirSize ds;
            if (DirSizeOf(Key(RowPath(t, i)), ds) && (ds.done || ds.bytes)) FormatSize(ds.bytes, buf, 64);
        }
        if (buf[0]) w = std::max(w, MeasureW(buf, g_fmtRight.Get()) + 4);
    }
    SetColWidth(which, std::ceil(w), pane);
    SaveColWidth(which);
}

void OnClick(float x, float y, bool middle) {
    Hit h = HitAt(x, y);
    switch (h.kind) {
        case HitKind::Tab:
            if (middle)
                CloseTab(h.idx);
            else
                SelectTab(h.idx);
            return;
        case HitKind::TabClose: CloseTab(h.idx); return;
        case HitKind::TabNew:
            if (g_split) ActivatePane(h.idx);
            NewTab(T().path);
            return;
        case HitKind::SideToggle: ToggleSidebar(); return;
        case HitKind::AppMenu: {
            RECT r = ScreenRect(h.r);
            r.left -= MulDiv(4, g_dpi, 96);
            r.bottom += MulDiv(4, g_dpi, 96);
            OpenAppMenu(r);
            return;
        }
        case HitKind::Back: GoBack(); return;
        case HitKind::Fwd: GoForward(); return;
        case HitKind::Up: GoUp(); return;
        case HitKind::Crumb:
            if (middle)
                NewTab(g_crumbPaths[h.idx]);
            else
                Navigate(T(), g_crumbPaths[h.idx], true);
            return;
        case HitKind::CrumbChev:
            if (!middle) CrumbMenu(g_crumbPaths[h.idx], h.r);
            return;
        case HitKind::FolderNote:
            if (!middle) FolderNoteAct(g_notePaths[h.idx]);
            return;
        case HitKind::PathEmpty: ShowPathEdit(); return;
        case HitKind::Side:
            if (middle)
                NewTab(g_sidePaths[h.idx]);
            else
                Navigate(T(), g_sidePaths[h.idx], true);
            return;
        case HitKind::Header: SetSort(static_cast<SortBy>(h.idx % 3)); return;
        case HitKind::FoldBtn: {
            ActivatePane(h.idx);
            Tab& t = T();
            if (!t.list || t.list->groups.empty()) return;
            bool allFolded = true;
            for (size_t gi = 0; gi < t.list->groups.size(); ++gi) allFolded &= GroupFolded(t, t.list.get(), gi);
            FoldAllGroups(t, !allFolded);
            return;
        }
        case HitKind::ColEdge:
            if (!middle) {
                g_colDrag = h.idx % 2 + 1;
                g_colDragPane = h.idx / 2;
                g_colDragX = x;
                g_colDragW = g_colDrag == 1 ? kDateW : kSizeW;
                SetCapture(g_hwnd);
                Invalidate();
            }
            return;
        case HitKind::ViewBtn:
            if (g_ve.open && g_ve.pane == h.idx) ViewEditorClose();
            else ViewEditorOpen(h.idx);
            return;
        case HitKind::Divider:
            if (!middle) {
                g_divDrag = true;
                SetCapture(g_hwnd);
            }
            return;
        default: break;
    }
    int hr = HeaderRowIn(g_act, x, y);
    if (hr >= 0 && !middle) {
        ToggleGroup(T(), hr);
        return;
    }
    int r = HitRow(x, y);
    if (r >= 0 && !middle && OnChevron(r, x) && !KeyDown(VK_SHIFT) && !KeyDown(VK_CONTROL)) {
        SetExpanded(T(), r, !IsExpanded(T(), r));
        return;
    }
    if (r >= 0) {
        MoveCursor(r, KeyDown(VK_SHIFT), KeyDown(VK_CONTROL));
        if (middle) OpenSelected(true);
    } else if (y >= ListTop() && y < ListBottom() && PaneAt(x, y) == g_act && Count() > 0) {
        SelectOnly(T(), T().sel);  // click on empty space: only the cursor remains
        std::fill(T().selMask.begin(), T().selMask.end(), uint8_t{0});
        Invalidate();
    }
}

// Thickness of the invisible frame Windows leaves around a window without a title bar.
void FrameMetrics(HWND hwnd, int& fx, int& fy) {
    UINT dpi = GetDpiForWindow(hwnd);
    int pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + pad;
    fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + pad;
}

bool IsCapButton(WPARAM ht) { return ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE; }

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // with the install panel open, nothing reaches what's behind it
    if (g_inst.open) {
        if (msg == WM_KEYDOWN) {
            if (wp == VK_ESCAPE) InstPanelClose();
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN ||
            msg == WM_MOUSEWHEEL || msg == WM_LBUTTONDBLCLK)
            return 0;
        if (msg == WM_MOUSEMOVE) {
            InstHover(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN) {
            InstClick(DipX(lp), DipY(lp));
            return 0;
        }
    }
    // the rename panel: its text boxes have the keyboard; the mouse only reaches the panel
    if (g_rn.open) {
        if (msg == WM_KEYDOWN) {
            if (wp == VK_ESCAPE) RenameManyClose();
            else if (wp == VK_RETURN) RenameManyApply();
            else if (wp == VK_TAB) RnFocus(g_rn.focus + (KeyDown(VK_SHIFT) ? -1 : 1));
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
            return 0;
        if (msg == WM_MOUSEWHEEL) {
            RnWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
            return 0;
        }
        if (msg == WM_MOUSEMOVE) {
            RnHover(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN) {
            RnClick(DipX(lp), DipY(lp));
            return 0;
        }
    }
    // with the palette open, the keyboard belongs to the palette
    if (g_pal.open) {
        if (msg == WM_KEYDOWN || (msg == WM_SYSKEYDOWN && (g_pal.capture >= 0 || wp != VK_F4))) {
            PaletteKey(wp);
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_SYSCHAR) {
            if (wp >= 32) PaletteChar(static_cast<wchar_t>(wp));
            return 0;
        }
        if (msg == WM_MOUSEMOVE) {
            PaletteHover(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g_pal.hoverShortcut >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
            PaletteClick(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {
            PaletteWheel(GET_WHEEL_DELTA_WPARAM(wp) / 120);
            return 0;
        }
        if (msg == WM_LBUTTONDBLCLK) return 0;
    }
    // with a popup menu open, the keyboard belongs to the menu
    if (g_popup.open()) {
        if (msg == WM_KEYDOWN) {
            PopupKey(wp);
            return 0;
        }
        if (msg == WM_CHAR) {
            PopupChar(static_cast<wchar_t>(wp));
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {
            PopupWheel(GET_WHEEL_DELTA_WPARAM(wp), POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) return 0;
    }
    // with the view editor open, the list's mouse and keyboard belong to the editor
    if (g_ve.open) {
        if (msg == WM_KEYDOWN) {
            if (wp == VK_ESCAPE) ViewEditorClose();
            return 0;
        }
        if (msg == WM_CHAR) return 0;
        if (msg == WM_MOUSEMOVE) {
            VeMouseMove(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            VeClick(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONUP) {
            VeMouseUp();
            return 0;
        }
        if (msg == WM_CAPTURECHANGED) {
            g_ved.drag = -1;
            g_ved.dragging = false;
        }
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL) return 0;
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            int hv = g_ved.hover;
            bool hand = hv >= 0 && hv < static_cast<int>(g_ved.hits.size()) && g_ved.hits[hv].k != VeK::Text;
            bool beam = hv >= 0 && hv < static_cast<int>(g_ved.hits.size()) && g_ved.hits[hv].k == VeK::Text;
            SetCursor(LoadCursorW(nullptr, g_ved.dragging ? IDC_SIZENS : beam ? IDC_IBEAM : hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
    }
    switch (msg) {
        // --- custom title bar: the client area reaches all the way to the top
        case WM_NCCALCSIZE: {
            if (!wp) break;
            RECT& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
            int fx, fy;
            FrameMetrics(hwnd, fx, fy);
            r.left += fx;
            r.right -= fx;
            r.bottom -= fy;
            if (IsZoomed(hwnd)) r.top += fy;  // when maximized, the frame ends up off screen
            return 0;
        }
        case WM_NCHITTEST: {
            LRESULT def = DefWindowProcW(hwnd, msg, wp, lp);
            if (def != HTCLIENT) return def;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (!IsZoomed(hwnd)) {
                int fx, fy;
                FrameMetrics(hwnd, fx, fy);
                if (pt.y < fy) {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    return pt.x < fx ? HTTOPLEFT : pt.x >= rc.right - fx ? HTTOPRIGHT : HTTOP;
                }
            }
            float x = pt.x * 96.f / g_dpi, y = pt.y * 96.f / g_dpi;
            if (y >= kTabH) return HTCLIENT;
            const int codes[3] = {HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
            for (int i = 0; i < 3; ++i)
                if (x >= g_capRects[i].left && x < g_capRects[i].right) return codes[i];
            return HitAt(x, y).kind != HitKind::None ? HTCLIENT : HTCAPTION;
        }
        case WM_NCMOUSEMOVE: {
            int h = IsCapButton(wp) ? static_cast<int>(wp) : 0;
            if (h != g_capHover) {
                g_capHover = h;
                Invalidate();
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            TrackMouseEvent(&tme);
            break;
        }
        case WM_NCMOUSELEAVE:
            if (g_capHover) {
                g_capHover = 0;
                Invalidate();
            }
            break;
        case WM_NCLBUTTONDOWN:
            if (IsCapButton(wp)) {
                g_capPressed = static_cast<int>(wp);
                return 0;  // so Windows doesn't draw its own button
            }
            break;
        case WM_NCLBUTTONUP: {
            int pressed = g_capPressed;
            g_capPressed = 0;
            if (IsCapButton(wp) && static_cast<int>(wp) == pressed) {
                WPARAM cmd = wp == HTMINBUTTON ? SC_MINIMIZE
                             : wp == HTCLOSE  ? SC_CLOSE
                             : IsZoomed(hwnd) ? SC_RESTORE
                                              : SC_MAXIMIZE;
                PostMessageW(hwnd, WM_SYSCOMMAND, cmd, 0);
                return 0;
            }
            break;
        }
        case WM_NCRBUTTONUP:
            if (wp == HTCAPTION) {
                HMENU m = GetSystemMenu(hwnd, FALSE);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0,
                                         hwnd, nullptr);
                if (cmd) PostMessageW(hwnd, WM_SYSCOMMAND, cmd, 0);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) PopupClose();
            Invalidate();
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (g_hwndRt) g_hwndRt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (g_test) ReleaseTarget();  // the bitmap is rebuilt at the new size
            for (auto& t : g_tabs) ClampScroll(*t);
            LayoutEdit();
            LayoutRename();
            Invalidate();
            return 0;
        case WM_DPICHANGED: {
            g_sysDpi = HIWORD(wp);
            ApplyDpi();
            ReleaseTarget();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateEditFont();
            LayoutEdit();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {MulDiv(640, g_dpi, 96), MulDiv(360, g_dpi, 96)};
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            Invalidate();
            return 0;
        case WM_APP_RESULT: {
            auto* hold = reinterpret_cast<std::shared_ptr<Job>*>(lp);
            ApplyJob(*hold);
            delete hold;
            LayoutRename();  // the list may have moved under the rename box
            return 0;
        }
        case WM_APP_DIRSIZE:
            g_dsNotify = false;
            if (g_dsFinished.exchange(false) && static_cast<SortBy>(g_sortBy.load()) == SortBy::Size) ResortAll();
            if (!g_dsBusy && S.message.rfind(L"Calculating the size", 0) == 0) S.message = L"Sizes calculated";
            Invalidate();
            return 0;
        case WM_APP_PREVIEW:
            PreviewApply(reinterpret_cast<PreviewResult*>(lp));
            return 0;
        case WM_APP_REC:
            ApplyRecBatch(reinterpret_cast<RecBatch*>(lp));
            return 0;
        case WM_APP_GREP:
            ApplyGrepResult(reinterpret_cast<GrepResult*>(lp));
        case WM_APP_THUMB:
            g_thumbDirty = false;
            Invalidate();
            return 0;
        case WM_APP_IDXMSG: {
            std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(lp));
            S.message = *m;
            Invalidate();
            return 0;
        }
        case WM_APP_IDX: {
            std::unique_ptr<IdxResult> r(reinterpret_cast<IdxResult*>(lp));
            if (r->gen != g_ixGen) return 0;
            g_ixRes = std::move(*r);
            if (g_pal.open && g_pal.mode == 1) {
                // keep the selection if it had already moved
                std::wstring keep = g_pal.sel > 0 && g_pal.sel < static_cast<int>(g_pal.hits.size()) ? g_pal.all[g_pal.hits[g_pal.sel].first].path : L"\x01";
                PaletteFilter();
                for (int i = 0; i < static_cast<int>(g_pal.hits.size()); ++i)
                    if (g_pal.all[g_pal.hits[i].first].path == keep) g_pal.sel = i;
                PaletteEnsureVisible();
                Invalidate();
            }
            return 0;
        }
        case WM_APP_CHANGED:
            SetTimer(hwnd, kTimerRevalidate, 80, nullptr);  // batches bursts of changes
            return 0;
        case WM_APP_ICON:
            g_iconDirty = false;
            Invalidate();
            for (auto& L : g_popup.levels) InvalidateRect(L->hwnd, nullptr, FALSE);  // a menu may be showing them too
            return 0;
        case WM_TIMER:
            KillTimer(hwnd, wp);
            if (wp == kTimerRevalidate) RevalidateVisible();
            if (wp == kTimerPreview) PreviewLoad();
            if (wp == kTimerTest) {
                if (g_testBlocking) SetTimer(hwnd, kTimerTest, 30, nullptr);  // a step is still waiting
                else TestRunStep();
            }
            if (wp == kTimerTestKill) {
                TestLog(L"timed out");
                ExitProcess(1);
            }
            if (wp == kTimerPrefetch) Prefetch();
            if (wp == kTimerBox) BoxTick();
            if (wp == kTimerHeat) HeatCommit();
            if (wp == kTimerDisk) DiskTick();
            if (wp == kTimerTabSlide) Invalidate();
            if (wp == kTimerSession) SaveSession();
            if (wp == kTimerConfig) ReloadConfig();
            return 0;
        case WM_APP_CONFIG:
            SetTimer(hwnd, kTimerConfig, 250, nullptr);  // editors save in several writes
            return 0;
        case WM_CLIPBOARDUPDATE:
            // if the clipboard changed from outside, what was "cut" no longer is
            if (g_ignoreClipUpdate) g_ignoreClipUpdate = false;
            else if (!g_cutKeys.empty()) {
                g_cutKeys.clear();
                Invalidate();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (g_sideDrag.press) {
                if (wp == VK_ESCAPE) SideDragCancel();
                return 0;
            }
            if (g_box.active) {  // while the box is being drawn, the keyboard doesn't change the list
                if (wp == VK_ESCAPE) BoxCancel();
                return 0;
            }
            const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT),
                       alt = KeyDown(VK_MENU);
            // with an active filter, Backspace and Esc belong to the filter (and Space is typed)
            if (!T().filter.empty() && msg == WM_KEYDOWN && !ctrl && !alt) {
                if (wp == VK_BACK) {
                    FilterBackspace();
                    return 0;
                }
                if (wp == VK_ESCAPE) {
                    FilterClear();
                    return 0;
                }
                if (wp == VK_SPACE && !shift) return 0;
            }
            // in the subfolder search with no text, Esc and Backspace return to the folder
            if (T().recursive && T().filter.empty() && msg == WM_KEYDOWN && !ctrl && !alt &&
                (wp == VK_ESCAPE || wp == VK_BACK)) {
                StopRecursive(T(), true);
                return 0;
            }
            // in the grid the plain arrows walk the line; with modifiers they keep their commands
            if (!ctrl && !alt && (wp == VK_LEFT || wp == VK_RIGHT) && RowsPerLine(T()) > 1) {
                MoveCursor(T().sel + (wp == VK_RIGHT ? 1 : -1), shift);
                return 0;
            }
            // configurable shortcuts first (config.txt)
            const UINT mods = (ctrl ? ModCtrl : 0) | (shift ? ModShift : 0) | (alt ? ModAlt : 0);
            Cmd cmd = FindCommand(mods, static_cast<UINT>(wp));
            if (cmd != Cmd::None) {
                g_eatSysChar = msg == WM_SYSKEYDOWN;
                RunCommand(cmd);
                return 0;
            }
            // Alt+1..8: that tab of the pane; Alt+9: the last one
            if (alt && !ctrl && !shift && wp >= '1' && wp <= '9') {
                const std::vector<int> idx = ActiveTabs();
                const size_t i = wp == '9' ? idx.size() - 1 : static_cast<size_t>(wp - '1');
                if (i < idx.size()) SelectTab(idx[i]);
                g_eatSysChar = true;
                return 0;
            }
            if (msg == WM_SYSKEYDOWN) break;  // Alt+F4, Alt+Space, etc. still belong to the system
            // fixed keys: cursor and selection
            const int page = RowsPerPage(T()), line = RowsPerLine(T());
            switch (wp) {
                case VK_UP: MoveCursor(T().sel - line, shift); break;
                case VK_DOWN: MoveCursor(T().sel + line, shift); break;
                case VK_PRIOR: MoveCursor(T().sel - page, shift); break;
                case VK_NEXT: MoveCursor(T().sel + page, shift); break;
                case VK_HOME: MoveCursor(0, shift); break;
                case VK_END: MoveCursor(Count() - 1, shift); break;
                case VK_SPACE: if (ctrl) MoveCursor(T().sel, false, true); break;
                case VK_ESCAPE:
                    // (no typing buffer)
                    S.message.clear();
                    g_cutKeys.clear();
                    if (Count() > 0) SelectOnly(T(), T().sel);
                    Invalidate();
                    break;
                default:
                    if (ctrl && !shift && wp >= '1' && wp <= '9') {
                        int i = static_cast<int>(wp - '1');
                        if (i < static_cast<int>(g_favs.size())) Navigate(T(), g_favs[i].path, true);
                    }
            }
            return 0;
        }
        case WM_SYSCHAR:
            if (g_eatSysChar) {
                g_eatSysChar = false;
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp >= 32 && !KeyDown(VK_CONTROL) && !KeyDown(VK_MENU) && T().list && !T().list->error) {
                // a letter without modifiers that is a configured shortcut doesn't filter
                SHORT vks = VkKeyScanW(static_cast<wchar_t>(wp));
                UINT vk = LOBYTE(vks), kmods = (HIBYTE(vks) & 1) ? ModShift : 0;
                bool spaceInFilter = wp == L' ' && !T().filter.empty();
                if (spaceInFilter || FindCommand(kmods, vk) == Cmd::None) FilterAppend(static_cast<wchar_t>(wp));
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = DipX(lp), y = DipY(lp);
            if (HitAt(x, y).kind == HitKind::PreviewEdge) {
                g_pvDrag = true;
                g_pvDragX = x;
                g_pvDragW = PreviewW();
                SetCapture(hwnd);
                Invalidate();
                return 0;
            }
            if (HitAt(x, y).kind != HitKind::Divider) ActivatePane(PaneAt(x, y));
            if (SideDragStart(HitAt(x, y), x, y)) return 0;
            if (HitAt(x, y).kind == HitKind::FolderNote) {  // before the row under it takes the click
                OnClick(x, y, false);
                return 0;
            }
            int r = HitRow(x, y);
            if (r >= 0 && OnChevron(r, x)) {
                OnClick(x, y, false);
                return 0;
            }
            bool mods = KeyDown(VK_SHIFT) || KeyDown(VK_CONTROL);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            Log(L"lbuttondown x=%.0f y=%.0f row=%d sel=%d", x, y, r, r >= 0 ? IsSel(T(), r) : 0);
            const bool box = BoxCanStart(r, x, y, mods);
            if (box) g_boxBefore = T().selMask;
            if (r >= 0 && !mods && IsSel(T(), r)) {
                // already selected: the selection isn't touched until release (it may be a drag)
                T().sel = T().anchor = r;
                Invalidate();
                PressBegin(r, pt, true);
                g_press.box = box;
                return 0;
            }
            if (!(r < 0 && mods && box)) OnClick(x, y, false);  // Ctrl or Shift on empty space don't lose the selection
            if (box || (r >= 0 && !mods)) {
                PressBegin(r, pt, false);
                g_press.box = box;
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_sideDrag.press) {
                SideDragMove(DipX(lp), DipY(lp));
                SideDragEnd();
                return 0;
            }
            if (g_colDrag) {
                ColDragEnd();
                ReleaseCapture();
                return 0;
            }
            if (g_pvDrag) {
                PreviewDragEnd();
                ReleaseCapture();
                return 0;
            }
            if (g_divDrag) {
                g_divDrag = false;
                ReleaseCapture();
                TouchSession();
                return 0;
            }
            if (g_box.active) {
                BoxEnd();
                return 0;
            }
            PressEnd(true);
            return 0;
        case WM_CAPTURECHANGED:
            g_press = {};
            g_sideDrag = {};
            BoxEnd();
            g_divDrag = false;
            ColDragEnd();
            PreviewDragEnd();
            return 0;
        case WM_MBUTTONDOWN:
            SetFocus(hwnd);
            ActivatePane(PaneAt(DipX(lp), DipY(lp)));
            OnClick(DipX(lp), DipY(lp), true);
            return 0;
        case WM_RBUTTONDOWN: {
            SetFocus(hwnd);
            ActivatePane(PaneAt(DipX(lp), DipY(lp)));
            int r = HitRow(DipX(lp), DipY(lp));
            if (r >= 0 && !IsSel(T(), r)) SetSel(r);  // over something already selected, the menu acts on all of it
            else if (r >= 0) { T().sel = r; Invalidate(); }
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = DipX(lp), y = DipY(lp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            const Hit side = HitAt(x, y);
            if (side.kind == HitKind::Side) OpenSideMenu(side.idx, pt);
            else if (side.kind == HitKind::Crumb) CrumbMenu(CrumbSiblingsDir(g_crumbPaths[side.idx]), side.r);
            else if (side.kind == HitKind::CrumbChev) CrumbMenu(g_crumbPaths[side.idx], side.r);
            else if (PaneAt(x, y) == g_act && y >= ListTop() && y < ListBottom()) OpenShellMenu(pt, HitRow(x, y));
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            PressEnd(false);
            Hit dh = HitAt(DipX(lp), DipY(lp));
            if (dh.kind == HitKind::FolderNote) return 0;  // the first click already did it
            if (dh.kind == HitKind::ColEdge) {
                FitColumn(dh.idx % 2 + 1, dh.idx / 2);
                return 0;
            }
            if (dh.kind == HitKind::PreviewEdge) {
                SetPreviewW(kPreviewDefaultW);
                SavePreviewW();
                return 0;
            }
            if (dh.kind == HitKind::Divider) {
                g_splitRatio = 0.5f;
                LayoutEdit();
                Invalidate();
                TouchSession();
                return 0;
            }
            if (dh.kind != HitKind::None) {
                OnClick(DipX(lp), DipY(lp), false);  // two clicks in a row on a button = two actions
                return 0;
            }
            int r = HitRow(DipX(lp), DipY(lp));
            // the second quick click arrives as a double click: on a group title or a chevron it counts
            // as another normal click (collapse and expand instantly, without waiting)
            if (HeaderRowIn(g_act, DipX(lp), DipY(lp)) >= 0 || (r >= 0 && OnChevron(r, DipX(lp)))) {
                OnClick(DipX(lp), DipY(lp), false);
                return 0;
            }
            if (r >= 0) {
                SetSel(r);
                OpenSelected(false);
            }
            return 0;
        }
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoBack();
            else GoForward();
            return TRUE;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x * 96.f / g_dpi, y = pt.y * 96.f / g_dpi;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            // Ctrl+Alt+wheel: the view of the pane underneath, from the list to bigger and bigger cells.
            if (((GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) || (g_test && KeyDown(VK_CONTROL))) && KeyDown(VK_MENU)) {
                if ((g_tabWheelAcc > 0) != (delta > 0)) g_tabWheelAcc = 0;
                g_tabWheelAcc += delta;
                const int steps = g_tabWheelAcc / WHEEL_DELTA;
                g_tabWheelAcc -= steps * WHEEL_DELTA;
                if (steps) {
                    ActivatePane(PaneAt(x, y));
                    StepLayout(steps > 0 ? 1 : -1);
                }
                return 0;
            }
            // Ctrl+wheel: tabs of the pane underneath (up = next, down = previous).
            // Ctrl comes in the wheel message itself, so it doesn't depend on focus.
            if ((GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) || (g_test && KeyDown(VK_CONTROL))) {
                if ((g_tabWheelAcc > 0) != (delta > 0)) g_tabWheelAcc = 0;
                g_tabWheelAcc += delta;
                const int steps = g_tabWheelAcc / WHEEL_DELTA;
                g_tabWheelAcc -= steps * WHEEL_DELTA;
                if (steps) ActivatePane(PaneAt(x, y));
                // with Shift, instead of switching tabs, moves the active one
                const bool moveTab = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) || (g_test && KeyDown(VK_SHIFT));
                for (int i = 0; i < std::abs(steps); ++i) {
                    if (moveTab) MoveTab(steps > 0 ? 1 : -1);
                    else CycleTab(steps > 0 ? 1 : -1);
                }
                return 0;
            }
            if (g_preview && x >= ClientDip().width - PreviewW() && y >= kTabH + kToolH) {
                PreviewWheel(delta / 120);
                return 0;
            }
            int pane = PaneAt(x, y);
            if (pane < 0) return 0;
            Tab& wt = PT(pane);  // the wheel scrolls the pane underneath, without activating it
            wt.scroll -= delta / 120.f * 3 * kRow;
            ClampScroll(wt);
            LayoutRename();
            g_hoverRow = HitRowIn(pane, x, y);
            g_hoverPane = pane;
            if (g_box.active) {
                g_hoverRow = -1;
                BoxMove(g_box.x, g_box.y);
            }
            Invalidate();
            return 0;
        }
        case WM_MOUSEMOVE: {
            float x = DipX(lp), y = DipY(lp);
            if (g_sideDrag.press) {
                if (wp & MK_LBUTTON) SideDragMove(x, y);
                else SideDragCancel();
                return 0;
            }
            if (g_colDrag) {
                // columns are anchored to the right: the left edge follows the mouse
                SetColWidth(g_colDrag, g_colDragW + g_colDragX - x, g_colDragPane);
                return 0;
            }
            if (g_pvDrag) {
                SetPreviewW(g_pvDragW + g_pvDragX - x);  // anchored to the right: the edge follows the mouse
                return 0;
            }
            if (g_divDrag) {
                const float side = SideW(), right = ClientDip().width - PreviewW();
                g_splitRatio = std::clamp((x - side) / std::max(1.f, right - side), 0.15f, 0.85f);
                LayoutEdit();
                Invalidate();
                return 0;
            }
            if (g_box.active) {
                if (wp & MK_LBUTTON) BoxMove(x, y);
                else BoxEnd();
                return 0;
            }
            if (g_press.active) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (!(wp & MK_LBUTTON)) {
                    PressEnd(false);
                } else if (PressMoved(pt) && g_press.box) {
                    const POINT pressed = g_press.pt;
                    g_press = {};  // capture continues: it now belongs to the box
                    BoxBegin(pressed, x, y);
                    return 0;
                } else if (PressMoved(pt)) {
                    PressEnd(false);
                    StartDrag();
                    return 0;
                }
            }
            int hp = std::max(0, PaneAt(x, y));
            int r = HitRowIn(hp, x, y);
            int hh = HeaderRowIn(hp, x, y);
            if (hh != g_hoverHeader) {
                g_hoverHeader = hh;
                Invalidate();
            }
            Hit h = HitAt(x, y);
            if (g_capHover) {
                g_capHover = 0;
                Invalidate();
            }
            if (r != g_hoverRow || hp != g_hoverPane || h.kind != g_hover.kind || h.idx != g_hover.idx) {
                g_hoverRow = r;
                g_hoverPane = hp;
                g_hover = h;
                Invalidate();
            }
            // In test mode the real mouse isn't over the window: it would report leaving right away.
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            if (!g_test) TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hoverRow != -1 || g_hover.kind != HitKind::None) {
                g_hoverRow = -1;
                g_hover = {{}, HitKind::None, -1};
                Invalidate();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                Hit h = HitAt(pt.x * 96.f / g_dpi, pt.y * 96.f / g_dpi);
                const bool sizeWE = h.kind == HitKind::Divider || h.kind == HitKind::ColEdge || h.kind == HitKind::PreviewEdge;
                bool link = h.kind != HitKind::None && h.kind != HitKind::PathEmpty && !sizeWE;
                if (h.kind == HitKind::None && HeaderRowIn(std::max(0, PaneAt(pt.x * 96.f / g_dpi, pt.y * 96.f / g_dpi)), pt.x * 96.f / g_dpi, pt.y * 96.f / g_dpi) >= 0)
                    link = true;
                SetCursor(LoadCursorW(nullptr, h.kind == HitKind::PathEmpty ? IDC_IBEAM
                                               : sizeWE                     ? IDC_SIZEWE
                                               : link                       ? IDC_HAND
                                                                            : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, RGB(0xE9, 0xE9, 0xEE));
            SetBkColor(dc, RGB(0x1A, 0x1A, 0x1E));
            return reinterpret_cast<LRESULT>(g_editBrush);
        }
        case WM_CLOSE:
            if (g_cfg.background && !g_quitting && !g_adminWindow) {
                BgHide();  // the X hides: ☰ → Quit really closes
                return 0;
            }
            if (g_test || g_adminWindow) break;  // tests don't save anything, and the administrator window is passing through
            SaveSession();
            if (g_dsAny) SaveDirSizes();  // what's already calculated, even if something is half done
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp && !g_test && !g_adminWindow) {
                SaveSession();
                if (g_dsAny) SaveDirSizes();
            }
            return 0;
        case WM_APP_QUIT:
            BgQuit();
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkeyId) BgToggle(true);
            return 0;
        case WM_APP_TRAY:
            if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) BgToggle(false);
            if (LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
            return 0;
        case WM_COPYDATA: {
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cd->dwData == 2) BgToggle(true);
            if (cd->dwData >= kAdminHide) BgAdminOrder(cd->dwData);
            if ((cd->dwData == 1 || cd->dwData == 3) && cd->cbData >= sizeof(wchar_t)) {
                const std::wstring p(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t) - 1);
                if (!p.empty()) cd->dwData == 3 ? BgRevealPath(p) : BgOpenPath(p);
                BgShow();
            }
            return TRUE;
        }
        case WM_DEVICECHANGE:
            BgDeviceChange(wp, lp);
            break;
        case WM_DESTROY:
            TrayRemove();
            RevokeDragDrop(hwnd);
            PostQuitMessage(0);
            return 0;
    }
    if (g_taskbarCreated && msg == g_taskbarCreated) {  // File Explorer restarted: the tray was lost
        g_trayOn = false;
        BgApplyConfig();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The folder from the command line. For a drive it arrives as "C:\\" and Windows reads that backslash as an escaped quote.
std::wstring ArgPath() {
    if (g_test) return L"";
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring p;
    for (int i = 1; argv && i < argc && p.empty(); ++i) {
        std::wstring a = argv[i];
        if (a == L"--select") {  // its value is the item to reveal, not a folder to open
            ++i;
            continue;
        }
        if (a.rfind(L"--", 0) == 0 || a.rfind(L"::", 0) == 0 || a.rfind(L"/select,", 0) == 0) continue;  // "::{...}": virtual shell folder, no path
        while (!a.empty() && a.back() == L'"') a.pop_back();
        if (a.size() == 2 && a[1] == L':') a += L'\\';
        wchar_t full[4096];
        if (GetFullPathNameW(a.c_str(), 4096, full, nullptr)) p = Normalize(full);
    }
    if (argv) LocalFree(argv);
    return p;
}

// "--select <item>" or File Explorer's "/select,<item>": the item (file or folder) to leave highlighted
// in the folder that holds it. Empty if not given.
std::wstring SelectArg() {
    if (g_test) return L"";
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring item;
    for (int i = 1; argv && i < argc && item.empty(); ++i) {
        std::wstring a = argv[i];
        if (a == L"--select" && i + 1 < argc) a = argv[i + 1];
        else if (a.rfind(L"/select,", 0) == 0) a = a.substr(8);
        else continue;
        while (!a.empty() && a.back() == L'"') a.pop_back();
        wchar_t full[4096];
        if (!a.empty() && GetFullPathNameW(a.c_str(), 4096, full, nullptr)) item = Normalize(full);
    }
    if (argv) LocalFree(argv);
    return item;
}

bool HasArg(const wchar_t* flag) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; argv && i < argc; ++i) found |= wcscmp(argv[i], flag) == 0;
    if (argv) LocalFree(argv);
    return found;
}

// Folder of the first tab when nothing is saved: the user's.
std::wstring DefaultPath() {
    wchar_t* p = nullptr;
    std::wstring r = L"C:\\";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

// Does it exist and is it a folder?
bool FolderExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(ToExtendedPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

#include "migrate.inl"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    if (cmdLine && wcsstr(cmdLine, L"--index-mft")) {  // elevated index helper (see indexer.inl)
        IdxElevatedMain();
        return 0;
    }
    if (HasArg(L"--quit") || HasArg(L"--salir")) {
        BgSendQuit();
        return 0;
    }
    if (HasArg(L"--uninstall") || HasArg(L"--desinstalar")) {
        InstUninstallMain();
        return 0;
    }
    std::wstring testScript, deskScript;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--test") == 0) testScript = argv[i + 1];
            if (wcscmp(argv[i], L"--test-desktop") == 0) deskScript = argv[i + 1];
            if (wcscmp(argv[i], L"--output") == 0) g_testOut = argv[i + 1];
        }
        if (argv) LocalFree(argv);
    }
    // on a separate Windows desktop: any system windows that open (properties, copy conflicts...)
    // stay there, invisible to the user
    if (!deskScript.empty()) return TestOnHiddenDesktop(deskScript, g_testOut);
    if (!testScript.empty()) {
        g_test = true;
        TestPrepareData();
        if (!TestLoadScript(testScript)) {
            TestLog(L"couldn't read the script: " + testScript);
            return 1;
        }
    }
    const bool hidden = !g_test && (HasArg(L"--hidden") || HasArg(L"--oculto"));
    const std::wstring selectItem = SelectArg();
    g_adminWindow = !g_test && HasArg(L"--elevated");
    g_elevated = RunningElevated();
    if (g_elevated) EnableBackupRight();
    if (!g_test && !g_adminWindow && BgHandOff(hidden ? L"" : selectItem.empty() ? ArgPath() : selectItem, HasArg(L"--toggle") || HasArg(L"--alternar"),
                             !hidden && !selectItem.empty()))
        return 0;
    BootMark(L"start");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetErrorMode(SEM_FAILCRITICALERRORS);  // no "insert a disk" dialogs
    OleInitialize(nullptr);  // apartment COM + drag and drop
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(g_dw.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    BootMark(L"COM and factories");
    CreateTextParams();
    MigrateOldData();
    LoadConfig();
    CreateFormats();
    LoadFavs();
    LoadTags();
    g_configSig = ConfigSignature();

    BootMark(L"config");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = g_test ? L"JoltTest" : g_adminWindow ? kAdminClass : kWindowClass;  // neither is mistaken for the open Jolt
    RegisterClassExW(&wc);
    RegisterPopupClass(inst);
    RegisterEditorClass(inst);

    UINT sysDpi = GetDpiForSystem();
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Jolt", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1100, sysDpi, 96), MulDiv(700, sysDpi, 96), nullptr, nullptr, inst, nullptr);
    g_sysDpi = GetDpiForWindow(g_hwnd);
    ApplyDpi();
    if (g_adminWindow) ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);  // the normal Jolt sends it folders

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF border = RGB(0x2B, 0x2B, 0x31);
    DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    BOOL noFade = TRUE;  // without this, Windows fades the window when hiding it
    DwmSetWindowAttribute(g_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noFade, sizeof(noFade));
    // applies WM_NCCALCSIZE: the Windows title bar disappears and we draw our own
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (g_test)  // tests: off screen and never shown
        SetWindowPos(g_hwnd, nullptr, -20000, -20000, MulDiv(1100, g_dpi, 96), MulDiv(700, g_dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);

    g_editBrush = CreateSolidBrush(RGB(0x1A, 0x1A, 0x1E));
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr, inst, nullptr);
    SetWindowSubclass(g_edit, EditProc, 1, 0);
    g_renameEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr, inst, nullptr);
    SetWindowSubclass(g_renameEdit, RenameProc, 2, 0);
    UpdateEditFont();

    g_watchEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(WorkerMain).detach();
    std::thread(WatcherMain).detach();
    std::thread(IconThreadMain).detach();
    std::thread(ConfigWatcherMain).detach();
    if (!g_test) {
        std::thread(LoadDirSizes).detach();  // folder sizes from previous sessions
        if (g_adminWindow) IdxStartReadOnly();
        else IdxStart();  // disk index for Ctrl+P
    } else {
        IdxStartReadOnly();  // in tests, the index is used but not modified
    }

    BootMark(L"window and threads");
    InitDragDrop();
    AddClipboardFormatListener(g_hwnd);
    BootMark(L"drag and drop");

    // previous session: window, sidebar, sort and tabs
    Session ses = LoadSession();
    if (g_adminWindow) {  // the position and the sidebar, but only the folder it came for
        ses.tabs.clear();
        ses.split = false;
    }
    BootMark(L"session");
    // views; the first time, the general one comes from the old sort (session) and folders_first (config)
    if (!LoadViews()) {
        g_viewAll.steps.clear();
        const int ff = g_foldersFirst.load();
        g_viewAll.steps.push_back({StepKind::Folders, ff == 1 ? FmFirst : ff == 2 ? FmFirstByName : FmMixed, 0, L""});
        int sb = std::clamp(ses.sortBy, 0, 2);
        g_viewAll.steps.push_back({StepKind::Sort, sb == 1 ? SfDate : sb == 2 ? SfSize : SfName, ses.sortAsc ? 1 : 0, L""});
        SaveViews();
    }
    SyncGlobalSort();
    RefreshConfigSig();
    g_sideVisible = ses.sidebar;
    g_preview = ses.preview;
    g_splitRatio = ses.ratio;
    if (ses.hasWindow) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        wp.rcNormalPosition = ses.rc;
        wp.showCmd = hidden ? SW_HIDE : ses.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(g_hwnd, &wp);
        g_showMaxOnce = hidden && ses.maximized;
    } else if (!g_test && !hidden) {
        ShowWindow(g_hwnd, show);
    }
    BootMark(L"views and position");
    // tabs: folders that no longer exist are skipped (and the saved indexes corrected)
    std::vector<int> newIdx(ses.tabs.size(), -1);
    bool both[2] = {false, false};
    for (auto& [p, pane] : ses.tabs) both[pane] = both[pane] || FolderExists(p);
    g_split = ses.split && both[0] && both[1];
    for (size_t i = 0; i < ses.tabs.size(); ++i) {
        auto& [p, pane] = ses.tabs[i];
        if (!FolderExists(p)) continue;
        g_act = g_split ? pane : 0;
        NewTab(p, false, g_act);
        newIdx[i] = static_cast<int>(g_tabs.size()) - 1;
    }
    auto mapIdx = [&](int old, int pane) {
        if (old >= 0 && old < static_cast<int>(newIdx.size()) && newIdx[old] >= 0 && g_tabs[newIdx[old]]->pane == pane)
            return newIdx[old];
        return NeighborInPane(pane, 0);
    };
    std::wstring arg = selectItem.empty() ? ArgPath() : RevealFolderOf(selectItem);
    if (g_tabs.empty()) {
        g_split = false;
        NewTab(arg.empty() ? DefaultPath() : arg);
    } else {
        if (g_split) {
            g_paneCur[0] = mapIdx(ses.visible[0], 0);
            g_paneCur[1] = mapIdx(ses.visible[1], 1);
        }
        int act = mapIdx(ses.active, g_split && ses.active >= 0 && ses.active < static_cast<int>(newIdx.size()) &&
                                             newIdx[ses.active] >= 0 ? g_tabs[newIdx[ses.active]]->pane : 0);
        if (act < 0) act = 0;
        g_act = g_split ? g_tabs[act]->pane : 0;
        g_cur = act;
        SelectTab(act);
        if (!arg.empty()) NewTab(arg);
    }
    BootMark(L"tabs");
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    BgApplyConfig();
    if (hidden) Render();  // the first frame is the most expensive: get it done before showing
    if (!g_test && !hidden && !g_adminWindow && (HasArg(L"--panel") || InstShouldOffer())) InstPanelOpen();
    S.navPending = false;  // startup isn't a navigation; not measured
    g_heatReady = !g_adminWindow;
    S.navMs = -1;
    if (g_test) {
        SetTimer(g_hwnd, kTimerTest, 400, nullptr);        // first script step
        SetTimer(g_hwnd, kTimerTestKill, 180000, nullptr); // just in case, never more than 3 minutes
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!g_relaunch.empty()) {
        if (g_instance) CloseHandle(g_instance);
        ShellExecuteW(nullptr, nullptr, g_relaunch.c_str(), g_relaunchArgs.c_str(), nullptr, SW_SHOWNORMAL);
    }
    // Direct exit: background threads don't need an orderly cleanup.
    ExitProcess(0);
}

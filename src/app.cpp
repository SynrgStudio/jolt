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
#include <sddl.h>
#include <lm.h>
#include <winnetwk.h>
#include <bcrypt.h>
#include <map>
#include <commctrl.h>
#include <commoncontrols.h>
#include <dwmapi.h>
#include <d2d1_3.h>  // and d2d1.h: SVG documents (Windows 10 1703+) for the preview
#include <dwrite_3.h>  // and dwrite.h: fonts from memory for the preview
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <fstream>
#include <functional>
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
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "bcrypt.lib")  // SHA-256 to compare duplicates
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
// The columns besides the name ([columns] show picks them and their order). Widths are configurable, and
// draggable from the header. The enum's order is where Settings puts a column it turns on.
enum ColKind { ColDate, ColDims, ColDur, ColAttrs, ColItems, ColSize, ColKinds };
struct ColDef {
    const wchar_t *key, *label;
    float def, min, max;
};
const ColDef kColDefs[ColKinds] = {
    {L"modified", L"Modified", 118, 64, 400}, {L"dimensions", L"Dimensions", 96, 56, 300}, {L"duration", L"Duration", 72, 48, 300},
    {L"attributes", L"Attributes", 76, 40, 300}, {L"items", L"Items", 60, 40, 300},     {L"size", L"Size", 72, 48, 300}};
float g_colW[ColKinds] = {118, 96, 72, 76, 60, 72};
constexpr float kPad = 12.f, kTabMaxW = 190.f, kTabMinW = 70.f, kTabPinMaxW = 112.f;
constexpr float kIcon = 16.f;

// Colors (configurable in config.txt; these are the defaults)
UINT32 cBg = 0x1A1A1E, cTabBar = 0x0F0F12, cTop = 0x202024, cSide = 0x151518, cBorder = 0x2B2B31, cText = 0xE9E9EE,
       cDim = 0x8C8C97, cFaint = 0x4C4C55, cAccent = 0x4F8CF7, cSel = 0x2C4C86, cError = 0xF07178, cDisk = 0x3B8EEA,
       cDiskFull = 0xE5484D, cRecent = 0x6BCB77;

constexpr wchar_t gBack = 0xE72B, gFwd = 0xE72A, gUp = 0xE74A, gAdd = 0xE710, gClose = 0xE8BB, gChevron = 0xE76C,
                  gPc = 0xE977, gStar = 0xE734, gSortUp = 0xE96D, gSortDown = 0xE96E, gFolder = 0xE8B7,
                  gFile = 0xE7C3, gDrive = 0xEDA2, gMin = 0xE921, gMax = 0xE922, gRestore = 0xE923,
                  gPaneClose = 0xE89F, gPaneOpen = 0xE8A0, gPin = 0xE718, gCloud = 0xE753, gCheck = 0xE73E;

constexpr UINT WM_APP_RESULT = WM_APP + 1, WM_APP_CHANGED = WM_APP + 2, WM_APP_ICON = WM_APP + 3;
constexpr UINT_PTR kTimerRevalidate = 1, kTimerPrefetch = 2, kTimerTabSlide = 12;
constexpr UINT_PTR kTimerRecCheck = 15;  // a Windows command or a drag may have changed what the results show

HWND g_hwnd = nullptr;

// Windows may finish a drop or a menu command in the background, a while later: the results on screen are read
// again a few times (0.8 s, then 2 s and 5 s after that) instead of once.
int g_recChecksLeft = 0;
void RecCheckSoon() {
    g_recChecksLeft = 3;
    SetTimer(g_hwnd, kTimerRecCheck, 800, nullptr);
}
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

// Folders that aren't on any disk. Their paths are Windows' own names for them.
const std::wstring kBinPath = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
const std::wstring kNetPath = L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}";
inline bool IsVirtual(const std::wstring& p) { return p.size() > 2 && p[0] == L':' && p[1] == L':'; }
inline bool IsBin(const std::wstring& p) { return IsVirtual(p) && _wcsicmp(p.c_str(), kBinPath.c_str()) == 0; }
inline bool IsNet(const std::wstring& p) { return IsVirtual(p) && _wcsicmp(p.c_str(), kNetPath.c_str()) == 0; }
inline std::wstring VirtualTitle(const std::wstring& p) { return IsBin(p) ? L"Recycle Bin" : IsNet(p) ? L"Network" : L""; }
// \\server, with no share after it, and \\server\share
inline bool IsServerPath(const std::wstring& p) { return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p.find(L'\\', 2) == std::wstring::npos; }
inline bool IsSharePath(const std::wstring& p) {
    if (p.size() < 5 || p[0] != L'\\' || p[1] != L'\\') return false;
    const size_t a = p.find(L'\\', 2);
    return a != std::wstring::npos && a + 1 < p.size() && p.find(L'\\', a + 1) == std::wstring::npos;
}
inline bool IsOnNetwork(const std::wstring& p) { return IsNet(p) || (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\'); }
std::wstring VirtualJoin(const std::wstring& dir, const wchar_t* name);  // bin.inl
void BinEnumerate(Listing& out);
void NetEnumerateComputers(Listing& out);  // net.inl
void NetEnumerateShares(const std::wstring& server, Listing& out);

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

void RunJob(const std::shared_ptr<Job>& j) {
    auto l = std::make_shared<Listing>();
    double t0 = NowMs();
    if (IsBin(j->path)) BinEnumerate(*l);
    else if (IsNet(j->path)) NetEnumerateComputers(*l);
    else if (IsServerPath(j->path)) NetEnumerateShares(j->path, *l);
    else EnumerateDir(j->path, EnumMethod::Nt, *l);
    double t1 = NowMs();
    SortBy by = static_cast<SortBy>(g_sortBy.load());
    SortListing(*l, by, g_sortAsc.load(), FoldersFirstFor(by));
    j->enumMs = t1 - t0;
    j->sortMs = NowMs() - t1;
    j->result = std::move(l);
    SetEvent(j->done);
    auto* hold = new std::shared_ptr<Job>(j);
    if (!PostMessageW(g_hwnd, WM_APP_RESULT, 0, reinterpret_cast<LPARAM>(hold))) delete hold;
}

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
        if (j->kind == JobKind::Prefetch && j->gen != g_navGen.load()) {
            SetEvent(j->done);
            continue;
        }
        // a server that is off can take half a minute to say so: everything else keeps going
        if (IsOnNetwork(j->path)) std::thread(RunJob, j).detach();
        else RunJob(j);
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
    if (IsVirtual(dir)) return VirtualJoin(dir, name);
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

bool IsDriveRoot(const std::wstring& p) { return p.size() == 3 && p[1] == L':' && p[2] == L'\\'; }

std::wstring ParentOf(const std::wstring& p) {
    if (p.empty() || IsDriveRoot(p)) return L"";
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    if (IsServerPath(p)) return kNetPath;
    if (IsSharePath(p)) return p.substr(0, pos);
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

void TestLog(const std::wstring& s);  // testmode.inl

// A test script only works on its own files: in test mode, changing anything outside the temp folder (or the
// Recycle Bin, where the bin tests recycle their own) is refused, so a script that goes astray can't touch the
// user's files.
bool TestRefuses(const std::wstring& path) {
    if (!g_test || path.empty()) return false;
    static const std::wstring temp = [] {
        wchar_t b[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH + 1, b);
        wchar_t full[MAX_PATH + 1] = {};
        GetLongPathNameW(b, full, MAX_PATH + 1);
        std::wstring k = Key(Normalize(full[0] ? full : b));
        return k.back() == L'\\' ? k : k + L"\\";
    }();
    std::wstring k = Key(Normalize(path));
    if (k.rfind(L"\\\\?\\", 0) == 0) k = k.substr(4);
    if (k.rfind(temp, 0) == 0 || k.find(L"\\$recycle.bin\\") != std::wstring::npos) return false;
    TestLog(L"refused outside the test area: " + path);
    return true;
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
    uint8_t closeness = 0;  // a name close to what was typed: how close, in percent. 0: a real match
};

// A set of identical files found by Ctrl+Alt+F (dups.inl): where each copy is, the oldest first.
struct DupFile {
    std::wstring dirKey, name;
    int hint = -1;  // its index in the folder's listing last time
};
struct DupSet {
    uint64_t size = 0;
    std::vector<DupFile> files;
    int shown = 0;  // copies in the rows right now
};

struct Tab {
    std::wstring path;
    std::shared_ptr<const Listing> list;
    bool loading = false;
    bool has = false;
    int pane = 0;                // 0 = left, 1 = right (split view)
    bool pinned = false;         // stays at the left of its pane, small, and doesn't close
    bool miller = false;         // Miller columns (miller.inl), whatever the folder's own view says
    mutable int millerFirst = -1;                    // the chain step in the leftmost column (-1: as far left as fits)
    mutable std::wstring millerFor;                  // the path millerChain was made for
    mutable std::vector<std::wstring> millerChain;   // from the top down to path
    std::wstring pendingSelect;  // name (in the folder) or full path to select when it appears
    int sel = 0;                 // cursor
    int anchor = 0;              // start of the Shift range
    std::vector<uint8_t> selMask;  // parallel to rows
    bool implicitSel = false;      // the selection is only the cursor the folder opened with: nobody chose it
    std::wstring filter;           // fuzzy filter text ("" = no filter)
    int nearRows = 0;              // how many of the rows are only close to it
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
    std::unordered_map<std::wstring, const Listing*> recKeys;  // the folders in rec, by key
    // duplicates (Ctrl+Alt+F): a subfolder search whose rows are the sets of identical files
    bool dups = false, dupHashing = false, dupDone = false;
    uint64_t dupGen = 0;
    std::vector<DupSet> dupSets;
    std::shared_ptr<Listing> dupHead;  // its groups are the sets' titles
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
enum class HitKind { None, Tab, TabClose, TabNew, SideToggle, AppMenu, Back, Fwd, Up, Crumb, CrumbChev, PathEmpty, Side, Header, Divider, ViewBtn, FoldBtn, ColEdge, PreviewEdge, FolderNote, HeadCheck, HeadCol, Miller };
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
// Zen mode (F11): only the tabs, the list and the status line. The sidebar slides out to the left and the navigation
// bar up; Ctrl+L brings the navigation bar back while the path is being typed. Kept in the session.
bool g_zen = false;
double g_zenAt = -1e9;  // when it last changed, for the slide
constexpr double kZenMs = 170;
bool ZenMoving() { return NowMs() - g_zenAt < kZenMs; }
float ZenT() {  // 0: the bars are there, 1: zen. Slows down at the end.
    const double p = std::clamp((NowMs() - g_zenAt) / kZenMs, 0.0, 1.0), e = 1 - (1 - p) * (1 - p) * (1 - p);
    return static_cast<float>(g_zen ? e : 1 - e);
}
float SideW() { return g_sideVisible ? std::round(kSideW * (1 - ZenT())) : 0.f; }
float ToolH() { return g_editing ? kToolH : std::round(kToolH * (1 - ZenT())); }
// Title bar buttons (non-client area): hover and pressed are tracked by HT* code.
D2D1_RECT_F g_capRects[3];
int g_capHover = 0, g_capPressed = 0;


void AddRecent(const std::wstring& p) {
    if (p.empty() || IsVirtual(p)) return;  // those are always in the sidebar
    std::wstring k = Key(p);
    g_recents.erase(std::remove_if(g_recents.begin(), g_recents.end(), [&](const std::wstring& r) { return Key(r) == k; }),
                    g_recents.end());
    g_recents.insert(g_recents.begin(), p);
    if (g_recents.size() > 300) g_recents.resize(300);
}

#include "heat.inl"
#include "bin.inl"
#include "net.inl"

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
bool FlatView(const Tab& t) { return !t.filter.empty() || t.recursive || IsVirtual(t.path); }
constexpr int kMiller = -1;  // LayoutOf's value for the Miller columns
int LayoutOf(const Tab& t);
void ToggleMiller();  // miller.inl

#include "fuzzy.inl"

// Is group gi collapsed? What was done by hand on this visit, otherwise what the view says.
bool GroupFolded(const Tab& t, const Listing* l, size_t gi) {
    auto it = t.fold.find(l->groups[gi].second);
    if (it != t.fold.end()) return it->second;
    return gi < l->groupFolded.size() && l->groupFolded[gi];
}

void AddTree(const Tab& t, const Listing* l, int depth, std::vector<Row>& out) {
    const bool miller = LayoutOf(t) == kMiller;  // the columns have neither tree nor groups
    const bool tree = !t.expanded.empty() && !miller;
    const bool groups = depth == 0 && !l->groups.empty() && t.filter.empty() && !miller;
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

// Every term of the filter but the plain words (and content:, tag:, which need more than the entry).
bool FilterTermsPass(const FilterSpec& s, const Entry& e, const wchar_t* name, size_t n) {
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
    return true;
}

// Does the entry pass the typed filter? score and pos come from the fuzzy words.
bool FilterPass(const FilterSpec& s, const Entry& e, const wchar_t* name, size_t n, int& score, std::vector<int>* pos,
                const NearPattern& close, int& percent) {
    if (!FilterTermsPass(s, e, name, n)) return false;
    const bool real = FuzzyMatch(name, n, s.fuzzy, score, pos);
    if (real && score > kNearFloor / 2) return true;
    percent = NearMatch(name, n, close);
    if (percent) score = NearScore(percent, n);
    return real || percent;
}

#include "tags.inl"

void BuildDupRows(Tab& t);  // dups.inl
std::wstring DupStatus(const Tab& t);

void BuildRows(Tab& t) {
    t.rows.clear();
    t.nearRows = 0;
    ++t.rowsGen;
    if (!t.list) return;
    if (t.recursive && t.dups) {
        BuildDupRows(t);
        return;
    }
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
    const NearPattern close(spec.fuzzy, g_cfg.typoMin);
    if (&t == &T() && spec.content != g_grepTerm) {
        if (spec.content.empty()) GrepStop();
        else GrepStart(spec.content);
    }
    std::vector<std::pair<int, int>> hits;  // (score, row)
    for (size_t i = 0; i < t.rows.size(); ++i) {
        const Row& r = t.rows[i];
        if (r.e < 0) continue;
        const Entry& e = r.l->entries[r.e];
        int sc, percent = 0;
        if (!FilterPass(spec, e, r.l->name(e), e.nameLen, sc, nullptr, close, percent)) continue;
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
        t.rows[i].closeness = static_cast<uint8_t>(percent);
        t.nearRows += percent != 0;
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
        const Listing* here = t.list.get();
        if (t.recursive)
            for (const auto& l : t.rec)
                if (Key(l->path) == Key(t.path)) {
                    here = l.get();
                    break;
                }
        for (int i = 0; i < n; ++i)
            if (t.rows[i].l == here && _wcsicmp(RowNameP(t, i), s.c_str()) == 0) return i;
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
float ListTop() { return kTabH + ToolH() + kHeadH; }
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
    const bool band = y < kTabH + ToolH() || y >= ListBottom();
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

int LayoutOf(const Tab& t) {
    if (t.miller && !t.recursive && !IsVirtual(t.path)) return kMiller;
    return t.path.empty() || t.recursive ? 0 : ViewFor(t.path)->layout;
}

// --- Miller columns: where they go (the rest is in miller.inl). Equal columns as wide as miller_width allows; the
// tab's folder and the next one always show, and otherwise the columns stay put, so what was clicked doesn't move.
struct MillerGeo {
    const std::vector<std::wstring>& chain;  // from the top (This PC) down to the tab's folder (the tab's own)
    int n = 0, first = 0;             // how many columns, and the chain step in the leftmost
    float L = 0, colW = 0;
    int depth() const { return static_cast<int>(chain.size()) - 1; }
};
std::unordered_map<std::wstring, float> g_millerScroll;      // a side column the wheel moved: "pane|folder" -> scroll
std::unordered_set<std::wstring> g_millerPending;            // folders asked for: their listing redraws
std::unordered_map<std::wstring, uint64_t> g_millerChecked;  // folder -> the navigation it was last checked in (+1)
bool g_millerKeep = false;  // the next navigation keeps the columns where they are (a click on them, or Left)

MillerGeo MillerLayout(const Tab& t) {
    if (t.millerFor != t.path || t.millerChain.empty()) {
        t.millerFor = t.path;
        t.millerChain.assign(1, t.path);
        for (int guard = 0; guard < 64 && !t.millerChain.back().empty(); ++guard) {
            const std::wstring up = ParentOf(t.millerChain.back());
            if (Key(up) == Key(t.millerChain.back())) break;
            t.millerChain.push_back(up);
        }
        std::reverse(t.millerChain.begin(), t.millerChain.end());
    }
    MillerGeo m{t.millerChain};
    const PaneX p = PaneGeo(t.pane);
    m.n = std::max(2, static_cast<int>((p.R - p.L) / g_cfg.millerW));
    m.colW = std::floor((p.R - p.L) / m.n);
    m.L = p.L;
    const int d = m.depth(), lo = std::max(0, d + 2 - m.n);
    t.millerFirst = t.millerFirst < 0 ? lo : std::clamp(t.millerFirst, lo, d);
    m.first = t.millerFirst;
    return m;
}

struct Geo {
    bool grid = false;
    bool miller = false;  // one column of rows (cells), where the Miller layout puts the tab's folder
    int size = 0, cols = 1;
    float cellW = 0, cellH = 0, x0 = 0;
};
Geo GeoFor(const Tab& t) {
    Geo g;
    g.size = LayoutOf(t);
    if (g.size == kMiller) {
        const MillerGeo m = MillerLayout(t);
        g.size = 0;
        g.miller = true;
        g.cellW = m.colW;
        g.cellH = kRow;
        g.x0 = m.L + (m.depth() - m.first) * m.colW;
        return g;
    }
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
    if (!g.grid && !g.miller) {
        t.cells.clear();
        t.cellsKey = 0;
        return;
    }
    const uint64_t key = (t.rowsGen << 24) ^ (static_cast<uint64_t>(g.cols) << 16) ^ static_cast<uint64_t>(g.size) ^
                         (static_cast<uint64_t>(std::lround(g.x0)) << 44) ^ (static_cast<uint64_t>(std::lround(kRow)) << 56) ^
                         (static_cast<uint64_t>(std::lround(g.cellW)) << 32) ^ (g.miller ? 0x9E3779B9ull : 0);
    if (key == t.cellsKey && t.cells.size() == t.rows.size()) return;
    t.cellsKey = key;
    t.cells.resize(t.rows.size());
    if (g.miller) {  // one row per line, in the tab's column
        for (size_t i = 0; i < t.rows.size(); ++i) t.cells[i] = D2D1::RectF(g.x0, i * kRow, g.x0 + g.cellW, (i + 1) * kRow);
        return;
    }
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

// With a filter, the close names below the real matches stay out: Ctrl+A and then Del must not take a file
// that only looks like what was typed. If there is nothing but close names, those are what there is.
void SelectAll(Tab& t) {
    t.implicitSel = false;
    const bool onlyReal = t.nearRows > 0 && t.nearRows < RowCount(t);
    for (int i = 0; i < static_cast<int>(t.selMask.size()); ++i) t.selMask[i] = !IsHeader(t, i) && !(onlyReal && t.rows[i].closeness);
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
    if ((t.path.empty() && !t.recursive) || IsVirtual(t.path)) return out;  // drives: not copied, and the rest aren't files
    for (int i = 0; i < RowCount(t); ++i)
        if (IsSel(t, i)) out.push_back(RowPath(t, i));
    return out;
}

// Is the selection spread over several folders? (subfolder search results, or the tree)
bool SelSpansFolders(const Tab& t) {
    if (t.sel < 0 || t.sel >= RowCount(t)) return false;
    const Listing* here = t.rows[t.sel].l;
    for (int i = 0; i < RowCount(t); ++i)
        if (IsSel(t, i) && t.rows[i].l != here && Key(RowDir(t, i)) != Key(here->path)) return true;
    return false;
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
    if (idx < 0 && keep && !keep->cur.empty() && !t.recursive) {
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
    if (!keep) t.implicitSel = true;
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
    if (IsVirtual(t.path)) return VirtualTitle(t.path);
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
        if (IsVirtual(t.path)) {
            if (IsBin(t.path))
                for (const std::wstring& dir : BinDirs()) v.push_back({dir, false});
            continue;
        }
        if (IsServerPath(t.path)) continue;
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
bool RecTake(Tab& t, const std::shared_ptr<const Listing>& l);  // search.inl
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
    t.millerFirst = g_millerKeep ? t.millerFirst : -1;
    g_millerKeep = false;
    if (current) g_millerScroll.clear();
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
    if (g_millerPending.erase(k)) Invalidate();  // a Miller column was waiting for it
    if (j->kind == JobKind::Prefetch) {
        if (g_preview) Invalidate();  // a folder's preview may be waiting for it
        return;
    }
    bool changed = g_preview;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.recursive && RecTake(t, l)) changed = true;
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
void KeepPinnedFirst();
bool FolderExists(const std::wstring& p);

// Closed tabs, to reopen them where they were and with their history.
struct ClosedTab {
    std::wstring path;
    int pane = 0, index = 0;
    std::vector<std::wstring> back, fwd;
};
std::vector<ClosedTab> g_closedTabs;

void CloseTab(int i) {
    if (g_tabs[i]->pinned) {
        S.message = L"This tab is pinned: unpin it to close it";
        Invalidate();
        return;
    }
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
        KeepPinnedFirst();
        Invalidate();
        TouchSession();
        return;
    }
    S.message = L"No closed tabs";
    Invalidate();
}

// The pinned tabs of each pane go first; the others keep their order.
void KeepPinnedFirst() {
    Tab* const cur = g_tabs[g_cur].get();
    Tab* const paneCur[2] = {g_tabs[g_paneCur[0]].get(), g_tabs[g_paneCur[1]].get()};
    for (int pane = 0; pane < 2; ++pane) {
        std::vector<size_t> at;
        for (size_t i = 0; i < g_tabs.size(); ++i)
            if (g_tabs[i]->pane == pane) at.push_back(i);
        std::vector<std::unique_ptr<Tab>> mine;
        for (size_t i : at) mine.push_back(std::move(g_tabs[i]));
        std::stable_partition(mine.begin(), mine.end(), [](const auto& t) { return t->pinned; });
        for (size_t k = 0; k < at.size(); ++k) g_tabs[at[k]] = std::move(mine[k]);
    }
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i) {
        if (g_tabs[i].get() == cur) g_cur = i;
        for (int p = 0; p < 2; ++p)
            if (g_tabs[i].get() == paneCur[p]) g_paneCur[p] = i;
    }
}

void SetPinned(int i, bool on) {
    g_tabs[i]->pinned = on;
    KeepPinnedFirst();
    Invalidate();
    TouchSession();
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
    if (to < 0 || to >= n || g_tabs[idx[to]]->pinned != T().pinned) return;  // the pinned ones stay together
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
        if (g_tabs[i].get() != keep && !g_tabs[i]->pinned && (!g_split || g_tabs[i]->pane == act)) CloseTab(i);
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
    g_millerKeep = true;
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
    if (IsBin(t.path)) {
        S.message = L"In the Recycle Bin: restore it to open it";
        Invalidate();
        return;
    }
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
    t.implicitSel = false;
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
constexpr float kChev = 14.f, kIndent = 18.f, kCheckW = 22.f;  // kCheckW: the selection checkbox, left of everything
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
    if (T().miller) {  // out of the columns, to the folder's own view
        ToggleMiller();
        return;
    }
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
    if (OnlineOnly(e.attrs)) return false;  // the icon of a program is inside it: reading it would download it
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
        } else if (IsServerPath(p) || IsSharePath(p)) {  // asking the server could take half a minute, and the icons wait in line
            SHSTOCKICONINFO sii{sizeof(sii)};
            if (SUCCEEDED(SHGetStockIconInfo(IsServerPath(p) ? SIID_SERVER : SIID_SERVERSHARE, SHGSI_SYSICONINDEX, &sii))) idx = sii.iSysImageIndex;
        } else if (IsVirtual(p)) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr))) {
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                    idx = sfi.iIcon;
                CoTaskMemFree(pidl);
            }
        } else if (OnlineOnlyPath(p)) {  // by its type: nothing is read
            const bool dir = (GetFileAttributesW(ToExtendedPath(p).c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (SHGetFileInfoW(p.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                               SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                idx = sfi.iIcon;
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
void MenuEditorReleaseBitmaps();  // menu.inl
void FolderTintRelease();         // foldercolor.inl
void ReleaseTarget() {
    PreviewReleaseBitmap();
    ThumbsReleaseBitmaps();
    MenuEditorReleaseBitmaps();
    FolderTintRelease();
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
    FolderTintRelease();
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

#include "foldercolor.inl"

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
    UINT32 fc;
    if (!drive && (!e || e->isDir()) && FolderColor(fullPath, fc))
        if (ID2D1Bitmap* tb = FolderTinted(0, fc)) {
            g_rt->DrawBitmap(tb, D2D1::RectF(x, y, x + kIcon, y + kIcon), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
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

// --- the mark on files changed lately ([general] recent_mark): a thin bar at the row's left edge (a dot in the grid's
// corner), strongest for what changed just now and fading toward the start of the window.
// From when it counts (FILETIME, UTC): local midnight, or N hours ago; 0: no mark.
int64_t RecentSince(int64_t now) {
    if (g_cfg.recentMark < 0) return 0;
    if (g_cfg.recentMark > 0) return now - g_cfg.recentMark * 36000000000ll;
    SYSTEMTIME local, utc;
    GetLocalTime(&local);
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    FILETIME f;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &f)) return 0;
    return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// How strong the mark is: 0, none; from 1 (just now) down to 0.35 (at the start of the window).
float RecentAlpha(int64_t mtime, int64_t since, int64_t now) {
    if (!since || mtime < since) return 0;
    const double frac = now > since ? std::clamp(static_cast<double>(now - mtime) / static_cast<double>(now - since), 0.0, 1.0) : 0;
    return static_cast<float>(1 - 0.65 * frac);
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
    // pinned tabs take what their short name needs, and the others share the rest
    std::vector<float> widths(n, 0.f);
    float pinnedW = 0;
    int rest = n;
    for (int k = 0; k < n; ++k)
        if (g_tabs[idx[k]]->pinned) {
            widths[k] = std::clamp(std::ceil(MeasureW(TabTitle(*g_tabs[idx[k]]), g_fmtTab.Get())) + 42.f, 58.f, kTabPinMaxW);
            pinnedW += widths[k];
            --rest;
        }
    const float avail = xEnd - x0 - 30 - pinnedW;  // room for the "+"
    float tw = std::min(kTabMaxW, std::max(36.f, avail / std::max(rest, 1)));
    if (avail / std::max(rest, 1) >= kTabMinW) tw = std::clamp(avail / std::max(rest, 1), kTabMinW, kTabMaxW);
    for (float& w : widths)
        if (w == 0) w = tw;
    auto xOf = [&](int k) {
        float x = x0;
        for (int j = 0; j < k; ++j) x += widths[j];
        return x;
    };
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
        const float vx = xOf(order.back()) + TabSlide(vt) * tw;
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
            Round(D2D1::RectF(pill.x, top, pill.x + widths[order.back()] - 4, bot + 8), 7, cTop);
        }
    }
    for (int k : order) {
        const int i = idx[k];
        const float x = xOf(k);
        const float off = TabSlide(*g_tabs[i]) * tw;
        if (off != 0) SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
        const D2D1_RECT_F home = D2D1::RectF(x, top, x + widths[k] - 4, bot);  // clicks go to its final place
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
        const bool pin = g_tabs[i]->pinned;
        bool showClose = (cur || hov) && !pin;
        float textR = showClose ? r.right - 26 : r.right - 10;
        Color(cur && activePane && activeWnd ? cText : cDim);
        if (pin) Glyph(gPin, g_fmtIconSm.Get(), r.left + 8, r.top, r.left + 24, r.bottom);
        Text(title, g_fmtTab.Get(), r.left + (pin ? 26 : 12), r.top, textR, r.bottom);
        if (showClose) {
            D2D1_RECT_F cr = D2D1::RectF(r.right - 24, (top + bot) / 2 - 8, r.right - 8, (top + bot) / 2 + 8);
            if (IsHover(HitKind::TabClose, i)) Round(cr, 4, 0xFFFFFF, 0.12f);
            Color(cur ? cDim : cFaint);
            Glyph(gClose, g_fmtIconSm.Get(), cr.left, cr.top, cr.right, cr.bottom);
            g_hits.push_back({D2D1::RectF(cr.left - off, cr.top, cr.right - off, cr.bottom), HitKind::TabClose, i});
        }
        g_hits.push_back({home, HitKind::Tab, i});
    }
    const float x = xOf(n);
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
    Glyph(g_sideVisible && !g_zen ? gPaneClose : gPaneOpen, g_fmtIcon.Get(), tg.left, tg.top, tg.right, tg.bottom);
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
    if (IsVirtual(t.path)) {
        const std::wstring title = VirtualTitle(t.path);
        crumbs.push_back({title, t.path, MeasureW(title, g_fmtCrumb.Get()) + 18});
    } else if (!t.path.empty()) {
        std::wstring acc;
        size_t start = 0;
        bool unc = t.path.rfind(L"\\\\", 0) == 0;
        if (unc) {
            start = 2;
            const std::wstring title = VirtualTitle(kNetPath);
            crumbs.push_back({title, kNetPath, MeasureW(title, g_fmtCrumb.Get()) + 18});
        }
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
    const float y0 = kTabH + ToolH();
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
    item(VirtualTitle(kNetPath), kNetPath, false, -1);
    item(VirtualTitle(kBinPath), kBinPath, false, -1);
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
#include "columns.inl"
Hit HitAt(float x, float y);
#include "miller.inl"
int g_colDrag = 0, g_colDragPane = 0;  // column being dragged: its ColKind + 1 (0: none)
float g_colDragX = 0, g_colDragW = 0;
struct Columns {
    float L, R, checkL, nameL, nameR, right;
    int n = 0;  // the extra columns that fit, left to right
    int kind[ColKinds];
    float l[ColKinds], r[ColKinds];  // where each one is (clicks go there)
    float dx[ColKinds];              // and how far from there it is drawn: sliding to its place, or being dragged
    bool sliding = false;
};
Columns Cols(int pane) {
    Columns c;
    const PaneX g = PaneGeo(pane);
    c.L = g.L;
    c.R = g.R;
    c.right = g.R - kPad - 6;
    c.checkL = g.L + kPad;
    c.nameL = g.L + kPad + (g_cfg.checkboxes ? kCheckW : 0.f);  // with checkboxes, everything moves right to make room
    // laid out from the right edge: on a narrow pane, the leftmost ones give way first and the name keeps ~290
    int ks[ColKinds], m = 0;
    float x = c.right, used = 0, ls[ColKinds];
    const bool millerCols = LayoutOf(PT(pane)) == kMiller;
    for (size_t i = g_cfg.cols.size(); !millerCols && i-- > 0 && m < ColKinds;) {
        const int k = g_cfg.cols[i];
        used += g_colW[k] + 18;
        if (g.R - g.L < used + 290) break;
        ks[m] = k;
        ls[m++] = x - g_colW[k];
        x -= g_colW[k] + 18;
    }
    c.n = m;
    for (int j = 0; j < m; ++j) {
        c.kind[j] = ks[m - 1 - j];
        c.l[j] = ls[m - 1 - j];
        c.r[j] = c.l[j] + g_colW[c.kind[j]];
    }
    c.nameR = m ? c.l[0] - 18 : c.right;
    for (int j = 0; j < m; ++j) {
        c.dx[j] = ColSlide(c.kind[j]);
        c.sliding |= c.dx[j] != 0;
        if (g_colMove.active && g_colMove.pane == pane && g_colMove.kind == c.kind[j])  // under the mouse, between the ends
            c.dx[j] = std::clamp(g_colMove.x - g_colMove.grab, c.l[0], c.right - g_colW[c.kind[j]]) - c.l[j];
    }
    return c;
}

// Tree indent for a row (0 with a filter or search: the list is flat there).
float RowIndent(const Tab& t, int row) { return FlatView(t) ? 0.f : t.rows[row].depth * kIndent; }
float RowTextX(const Columns& c, const Tab& t, int row) { return c.nameL + kChev + RowIndent(t, row) + 28; }

// --- checkbox animation. The box only fades in, and stays put: scaling it reads as movement. The check pops in a
// moment later with a little overshoot,
// and shrinks away when unchecked. Keyed by path, so it survives the list being rebuilt; entries not drawn for a
// while go away.
struct CheckAnim {
    bool on = false;
    double shownAt = 0, onAt = 0;  // 0: it was like that from the start, nothing to animate
    unsigned frame = 0;            // the last frame it was drawn in
};
std::unordered_map<std::wstring, CheckAnim> g_checkAnim;
unsigned g_checkFrame = 0;
double g_checkPruneAt = 0;
constexpr double kCheckBoxMs = 110, kCheckDelayMs = 50, kCheckPopMs = 170, kCheckOffMs = 90;

float EaseOut(float p) {
    p = std::clamp(p, 0.f, 1.f);
    return 1 - (1 - p) * (1 - p) * (1 - p);
}
// 0 -> past 1 -> back to 1: the check lands with a bounce.
float PopScale(float p) {
    p = std::clamp(p, 0.f, 1.f);
    const float c1 = 1.70158f, c3 = c1 + 1, q = p - 1;
    return 1 + c3 * q * q * q + c1 * q * q;
}

// Once per frame: what was not drawn last frame has disappeared, and old entries go.
void CheckAnimFrame() {
    ++g_checkFrame;
    const double now = NowMs();
    if (now - g_checkPruneAt < 1000) return;
    g_checkPruneAt = now;
    for (auto it = g_checkAnim.begin(); it != g_checkAnim.end();)
        it = it->second.frame + 2 < g_checkFrame ? g_checkAnim.erase(it) : std::next(it);
}

// A glyph scaled around its center.
void GlyphScaled(wchar_t g, IDWriteTextFormat* f, const D2D1_RECT_F& r, float scale) {
    if (scale <= 0.02f) return;
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2)));
    Glyph(g, f, r.left, r.top, r.right, r.bottom);
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

// A selection checkbox. mark: the glyph inside when on (a check, or the header's dash). appear: it animates in
// (the hover mode); otherwise it is just there. Over a thumbnail it gets a dark backing, so it reads on any picture.
void DrawCheckbox(const std::wstring& key, const D2D1_RECT_F& r, bool on, wchar_t mark, bool appear, bool backed, UINT32 boxOff, UINT32 boxOn, UINT32 markColor) {
    const double now = NowMs();
    auto it = g_checkAnim.find(key);
    if (it == g_checkAnim.end()) {
        it = g_checkAnim.emplace(key, CheckAnim{on, appear ? now : 0, appear && on ? now : 0, g_checkFrame}).first;
    } else {
        CheckAnim& a = it->second;
        if (a.frame + 1 < g_checkFrame) {  // it was gone: it comes back (animated, if that is the mode)
            a.shownAt = appear ? now : 0;
            a.onAt = appear && on ? now : 0;
            a.on = on;
        }
        if (a.on != on) {
            a.on = on;
            a.onAt = now;
        }
        a.frame = g_checkFrame;
    }
    const CheckAnim& a = it->second;
    const float pBox = a.shownAt ? static_cast<float>((now - a.shownAt) / kCheckBoxMs) : 1.f;
    const float alpha = EaseOut(pBox);
    const float q = !a.onAt ? 1.f : on ? static_cast<float>((now - a.onAt - kCheckDelayMs) / kCheckPopMs) : static_cast<float>((now - a.onAt) / kCheckOffMs);
    const float markScale = on ? (q <= 0 ? 0.f : PopScale(q)) : 1 - EaseOut(q);
    const bool busy = pBox < 1 || q < 1;
    if (busy) SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
    if (backed) Round(r, 5, 0x000000, 0.55f * alpha);
    Color(on || markScale > 0 ? boxOn : boxOff, alpha);
    Glyph(0xE739, g_fmtIcon.Get(), r.left, r.top, r.right, r.bottom);
    Color(markColor, alpha);
    GlyphScaled(mark, g_fmtIcon.Get(), r, 0.6f * markScale);
}

void DrawHeader(int pane) {
    const float y0 = kTabH + ToolH(), y1 = y0 + kHeadH;
    Columns c = Cols(pane);
    Fill(D2D1::RectF(c.L, y0, c.R, y1), cBg);
    HLine(c.L, c.R, y1 - 1);
    const View& view = *ViewFor(PT(pane).path);
    SortBy by = SortBy::Name;
    bool sortAsc = true;
    const bool hasSort = ViewPrimarySort(view, by, sortAsc);
    if (g_cfg.checkboxes) {  // select all, or none: checked, empty, or a dash when only some are
        const Tab& pt = PT(pane);
        const bool onlyReal = pt.nearRows > 0 && pt.nearRows < RowCount(pt);
        int can = 0, on = 0;
        for (int i = 0; i < RowCount(pt); ++i) {
            if (IsHeader(pt, i) || (onlyReal && pt.rows[i].closeness)) continue;
            ++can;
            on += IsSel(pt, i);
        }
        const D2D1_RECT_F cb = D2D1::RectF(c.checkL - 2, y0 + 3, c.checkL + kCheckW - 4, y1 - 4);
        const bool hov = IsHover(HitKind::HeadCheck, pane);
        if (hov) Round(cb, 5, 0xFFFFFF, 0.08f);
        DrawCheckbox(L"\x01header" + std::to_wstring(pane), cb, on > 0, on >= can ? 0xE73E : 0xE738, false, false, hov ? cText : cDim, cAccent, cAccent);
        g_hits.push_back({cb, HitKind::HeadCheck, pane});
    }
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
    // sort: 0 name, 1 date, 2 size; -1: the column doesn't sort. lifted: the one being dragged.
    auto title = [&](const std::wstring& label, int sort, float l, float rt, bool right, bool hover, bool lifted) {
        // "Name" starts after the view button
        const D2D1_RECT_F r = D2D1::RectF(right ? l - 6 : l + kChev + 26, y0 + 3, rt + 6, y1 - 4);
        const bool active = sort >= 0 && hasSort && by == static_cast<SortBy>(sort);
        if (lifted) {
            Round(r, 4, cBg);  // covers what it passes over
            Round(r, 4, 0xFFFFFF, 0.09f);
        } else if (hover) {
            Round(r, 4, 0xFFFFFF, 0.05f);
        }
        Color(active || lifted ? cText : cDim);
        float tl = l + (right ? 0 : kChev + 28), tr = rt;
        if (active) {
            wchar_t g = sortAsc ? gSortUp : gSortDown;
            float w = MeasureW(label, g_fmtHeader.Get());
            // right next to the title, even if the column is wide
            if (right) Glyph(g, g_fmtIconSm.Get(), tr - w - 18, y0, tr - w - 4, y1);
            else Glyph(g, g_fmtIconSm.Get(), tl + w + 4, y0, tl + w + 18, y1);
        }
        Text(label, right ? g_fmtRight.Get() : g_fmtHeader.Get(), tl, y0, tr, y1);
        return r;
    };
    if (LayoutOf(PT(pane)) == kMiller) {  // no sorting titles: each column's folder instead
        DrawMillerTitles(pane, y0, y1, c.nameL + 44);
        return;
    }
    g_hits.push_back({title(L"Name", 0, c.nameL, c.nameR, false, IsHover(HitKind::Header, pane * 3), false), HitKind::Header, pane * 3});
    // The other titles sort on release and can be dragged to another place (HeadCol: pane * ColKinds + kind).
    // Their hits stay where they end up, even while they slide there.
    int lifted = -1;
    for (int j = 0; j < c.n; ++j) {
        const int k = c.kind[j], id = pane * ColKinds + k;
        g_hits.push_back({D2D1::RectF(c.l[j] - 6, y0 + 3, c.r[j] + 6, y1 - 4), HitKind::HeadCol, id});
        if (g_colMove.active && g_colMove.pane == pane && g_colMove.kind == k) {
            lifted = j;  // drawn last, over the others
            continue;
        }
        title(ColLabel(k, pane), ColSortOf(k), c.l[j] + c.dx[j], c.r[j] + c.dx[j], true, !g_colMove.active && IsHover(HitKind::HeadCol, id), false);
        if (g_colMove.active) continue;  // no width edges while one moves
        // its left edge changes the width: the line only appears with the mouse over it
        const float x = c.l[j] - 9;
        if (IsHover(HitKind::ColEdge, id) || (g_colDrag == k + 1 && g_colDragPane == pane))
            Fill(D2D1::RectF(x, y0 + 6, x + 1, y1 - 7), cDim);
        g_hits.push_back({D2D1::RectF(x - 5, y0, x + 6, y1 - 1), HitKind::ColEdge, id});
    }
    if (lifted >= 0) {
        const int k = c.kind[lifted];
        title(ColLabel(k, pane), ColSortOf(k), c.l[lifted] + c.dx[lifted], c.r[lifted] + c.dx[lifted], true, false, true);
    }
    if (c.sliding) SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
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
                  float a, const std::wstring& filterPat, int closeness) {
    const float box = static_cast<float>(geo.size), boxL = cell.left + kCellPadX / 2, boxT = y + 6;
    bool drawn = false;
    UINT32 fc;
    if (e.isDir() && FolderColor(full, fc))  // its color instead of the Shell's thumbnail
        if (ID2D1Bitmap* tb = FolderTinted(box > 48 ? 2 : 1, fc)) {
            const float s = std::floor(std::min(box, tb->GetSize().width));
            const float x0 = std::floor(boxL + (box - s) / 2), y0 = std::floor(boxT + (box - s) / 2);
            g_rt->DrawBitmap(tb, D2D1::RectF(x0, y0, x0 + s, y0 + s), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            drawn = true;
        }
    if (Thumb* th = drawn ? nullptr : ThumbGet(full, geo.size, e.mtime)) {
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
    if (g_cfg.cloudStatus && (OnlineOnly(e.attrs) || (e.attrs & FILE_ATTRIBUTE_PINNED))) {  // same: over the picture
        const D2D1_RECT_F r = D2D1::RectF(cell.right - 28, boxT + box - 22, cell.right - 6, boxT + box - 2);
        Round(r, 4, 0x000000, 0.6f * a);
        Color(0xC9C9D1, a);
        Glyph(OnlineOnly(e.attrs) ? gCloud : gCheck, g_fmtCap.Get(), r.left, r.top, r.right, r.bottom);
    }
    if (closeness) {  // over the picture, so it needs a background
        const std::wstring pct = std::to_wstring(closeness) + L"%";
        const float w = MeasureW(pct, g_fmtTag.Get()) + 10;
        const D2D1_RECT_F r = D2D1::RectF(cell.right - w - 6, y + 8, cell.right - 6, y + 25);
        Round(r, 4, 0x000000, 0.6f * a);
        Color(0xC9C9D1, a);
        Text(pct, g_fmtTag.Get(), r.left + 5, r.top, r.right, r.bottom);
    }
}

void DrawRowCheck(const std::wstring& key, const D2D1_RECT_F& r, bool on, bool backed, bool onSel) {
    DrawCheckbox(key, r, on, 0xE73E, g_cfg.checkboxes == 1, backed, onSel ? 0xC6D4EE : cDim, onSel ? 0xFFFFFF : cAccent, onSel ? 0xFFFFFF : cAccent);
}

void DrawList(int pane, float H) {
    const float listTop = ListTop(), listBot = H - kStatus;
    Tab& t = PT(pane);
    Columns c = Cols(pane);
    const float L = c.L, R = c.R;
    g_rt->PushAxisAlignedClip(D2D1::RectF(L, listTop, R, listBot), D2D1_ANTIALIAS_MODE_ALIASED);
    const int n = RowCount(t);
    // with Miller columns, the other columns first; the messages go in the tab's own
    const Geo lg = GeoFor(t);
    if (lg.miller) DrawMillerSides(pane, listTop, listBot);
    const float mL = lg.miller ? lg.x0 : L, mR = lg.miller ? lg.x0 + lg.cellW : R;
    if (t.loading) {
        Color(cDim);
        Text(L"Loading…", g_fmtCenter.Get(), mL, listTop, mR, listTop + 80);
    } else if (t.list && t.list->error) {
        DrawFolderError(t, mL, mR, listTop);
    } else if (t.list && n == 0) {
        Color(cDim);
        Text(t.dups && !t.dupDone ? L"Looking for duplicates…" : t.recursive && !t.recDone ? L"Searching…" : t.dups && t.filter.empty() ? L"No duplicates" : !t.filter.empty() ? L"No matches" : IsServerPath(t.path) ? L"No shared folders" : L"Empty folder",
             g_fmtCenter.Get(), mL, listTop, mR, listTop + 80);
    } else if (t.list) {
        const bool focused = (g_test ? g_testActive && !g_renaming && !g_editing : GetFocus() == g_hwnd) && pane == g_act;
        const bool multi = SelCount(t) > 1;
        const bool flat = FlatView(t);
        const int hoverHeader = g_hoverHeader;
        const std::wstring filterPat = ParseFilter(t.filter).fuzzy;  // only the words light up in the names
        FILETIME nowFt;
        GetSystemTimeAsFileTime(&nowFt);
        const int64_t now = (static_cast<int64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime, since = RecentSince(now);
        const Geo geo = GeoFor(t);
        uint64_t barMax = 0;  // size bars: the largest item in the list
        if (g_cfg.sizeBars && !geo.grid && !geo.miller && std::find(c.kind, c.kind + c.n, ColSize) != c.kind + c.n)
            for (int i = 0; i < n; ++i) {
                if (IsHeader(t, i)) continue;
                const Row& r = t.rows[i];
                const Entry& e = r.l->entries[r.e];
                if (!e.isDir()) barMax = std::max(barMax, e.size);
                else if (g_dsAny && !r.l->path.empty()) barMax = std::max(barMax, ItemBytes(e, Join(r.l->path, r.l->name(e)), false));
            }
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
                if (t.dups && static_cast<size_t>(-1 - row.e) < t.dupSets.size()) {
                    const int copies = t.dupSets[-1 - row.e].shown;
                    cnt = copies == 1 ? L"only copy left" : Thousands(copies) + L" copies";
                }
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
            const D2D1_RECT_F rr = geo.grid    ? D2D1::RectF(cell.left + 2, y + 2, cell.right - 2, y + geo.cellH - 2)
                                   : geo.miller ? D2D1::RectF(cell.left + 4, y + 1, cell.right - 4, y + kRow - 1)
                                                : D2D1::RectF(L + 6, y + 1, R - 8, y + kRow - 1);
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
            const bool showCheck = g_cfg.checkboxes == 2 || (g_cfg.checkboxes == 1 && (hover || selected));
            const float recent = e.isDir() || drives ? 0.f : RecentAlpha(e.mtime, since, now);
            if (geo.miller) {
                DrawMillerItem(e, name, full, rr, y, a, selected && focused, drives, recent, t.filter.empty() ? std::wstring() : filterPat);
                continue;
            }
            if (geo.grid) {
                DrawGridCell(e, name, full, cell, y, geo, a, filterPat, row.closeness);
                if (recent > 0 && !row.closeness) {  // the corner the closeness percentage would use; over the picture
                    const D2D1_POINT_2F p = D2D1::Point2F(cell.right - 13, y + 13);
                    Color(0x000000, 0.45f * a);
                    g_rt->FillEllipse(D2D1::Ellipse(p, 5.5f, 5.5f), g_brush.Get());
                    Color(cRecent, recent * a);
                    g_rt->FillEllipse(D2D1::Ellipse(p, 3.5f, 3.5f), g_brush.Get());
                }
                if (showCheck) DrawRowCheck(std::to_wstring(pane) + Key(full), D2D1::RectF(cell.left + 6, y + 6, cell.left + 28, y + 28), selected, true, false);
                continue;
            }
            if (showCheck) DrawRowCheck(std::to_wstring(pane) + Key(full), D2D1::RectF(c.checkL - 2, y, c.checkL + kCheckW - 4, y + kRow), selected, false, selected && focused);
            if (recent > 0) Round(D2D1::RectF(rr.left + 1, y + 6, rr.left + 3.5f, y + kRow - 6), 1.25f, cRecent, recent * a);
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
            if (IsBin(row.l->path) && afterName < nameEnd - 30) {  // where it came from
                Color(selected && focused ? 0x9FB3D9 : cFaint, a);
                Text(ParentOf(BinItemOf(name).original), g_fmtName.Get(), afterName, y, nameEnd, y + kRow);
            }
            if (row.closeness && afterName < nameEnd - 24) {  // not a real match: how close it is
                const std::wstring pct = std::to_wstring(row.closeness) + L"%";
                Color(selected && focused ? 0x9FB3D9 : cFaint, a);
                Text(pct, g_fmtName.Get(), afterName - 6, y, nameEnd, y + kRow);
                afterName += MeasureW(pct, g_fmtName.Get()) + 8;
            }
            if (g_cfg.cloudStatus && !drives && (OnlineOnly(e.attrs) || (e.attrs & FILE_ATTRIBUTE_PINNED))) {  // where it is, in a cloud folder
                Color(selected && focused ? 0x9FB3D9 : cDim, a);
                Glyph(OnlineOnly(e.attrs) ? gCloud : gCheck, g_fmtCap.Get(), afterName - 8, y, afterName + 8, y + kRow);
                afterName += 16;
            }
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
            for (int j = 0; j < c.n; ++j) {
                bool faint = false;
                const std::wstring s = CellText(c.kind[j], e, full, drives, faint);
                if (c.kind[j] == ColSize && barMax)
                    if (const uint64_t b = ItemBytes(e, full, drives)) {  // right-aligned, like the size
                        const float r = c.r[j] + c.dx[j] + 4, full_w = c.r[j] - c.l[j] + 8;
                        const float w = std::round(full_w * static_cast<float>(static_cast<double>(b) / static_cast<double>(barMax)));
                        const bool onSel = selected && focused;
                        if (w >= 1) Round(D2D1::RectF(r - w, y + 5, r, y + kRow - 5), 3, onSel ? 0xFFFFFF : cAccent, (onSel ? 0.16f : 0.22f) * a);
                    }
                if (s.empty()) continue;
                Color(faint ? cFaint : selected && focused ? 0xC6D4EE : cDim, a);
                Text(s, g_fmtRight.Get(), c.l[j] + c.dx[j], y, c.r[j] + c.dx[j], y + kRow);
            }
        }
        // thin scrollbar, only when needed
        const float contentH = ContentHeight(t), viewH = listBot - listTop;
        if (contentH > viewH) {
            const float thumbH = std::max(24.f, viewH * viewH / contentH);
            const float thumbY = listTop + (viewH - thumbH) * (t.scroll / (contentH - viewH));
            const float sbR = geo.miller ? geo.x0 + geo.cellW : R;  // in the columns, the tab's own
            Round(D2D1::RectF(sbR - 5, thumbY, sbR - 2, thumbY + thumbH), 1.5f, 0xFFFFFF, 0.14f);
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
        if (IsNet(t.path)) left += g_netLooking ? L"   ·   looking for computers…" : L"   ·   Ctrl+L and \\\\name goes straight to one";
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
        std::wstring label = t.dups ? L"duplicates:" : t.recursive ? L"in subfolders:" : L"";
        float lw = label.empty() ? 0 : MeasureW(label, g_fmtName.Get()) + 6;
        float tw = MeasureW(t.filter, g_fmtName.Get());
        D2D1_RECT_F box = D2D1::RectF(lx - 4, y0 + 3, lx + 30 + lw + tw + 12, H - 3);
        Round(box, 5, cAccent, 0.18f);
        Color(cAccent);
        Glyph(t.dups ? 0xE8C8 : t.recursive ? 0xE8B7 : 0xE721, g_fmtIconSm.Get(), box.left + 4, box.top, box.left + 24, box.bottom);
        if (!label.empty()) {
            Color(0x9FB3D9);
            Text(label, g_fmtName.Get(), box.left + 26, box.top, box.right, box.bottom);
        }
        Color(cText);
        Text(t.filter, g_fmtName.Get(), box.left + 26 + lw, box.top, box.right, box.bottom);
        if (activePane) Fill(D2D1::RectF(box.left + 26 + lw + tw + 1, box.top + 5, box.left + 26 + lw + tw + 2, box.bottom - 5), cText);
        lx = box.right + 10;
        const std::wstring close = t.nearRows ? L", " + Thousands(t.nearRows) + L" close" : L"";
        if (t.dups) {
            left = DupStatus(t);
        } else if (t.recursive) {
            left = (t.filter.empty() ? Thousands(n) + L" items" : Thousands(n) + L" matches" + close) + L" in " +
                   Thousands(t.recDirs) + (t.recDirs == 1 ? L" folder" : L" folders") +
                   (t.recDone ? L"   ·   Esc goes back" : L"   ·   searching…");
        } else {
            left = Thousands(n) + (n == 1 ? L" match" : L" matches") + close + L"   ·   Esc clears";
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
void DrawMenuEditor(float W, float H);    // menu.inl
void DrawSettings(float W, float H);      // settings.inl
void DrawAttrs(float W, float H);         // attrs.inl
void DrawInUse(float W, float H);         // inuse.inl

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

void LayoutEdit();  // below
void Render() {
    if (!EnsureTarget()) return;
    EnsureImageList();
    if (g_iconFolder < 0) g_iconFolder = IconByAttrs(L"folder", FILE_ATTRIBUTE_DIRECTORY);
    if (g_iconFile < 0) g_iconFile = IconByAttrs(L"file", FILE_ATTRIBUTE_NORMAL);
    g_hits.clear();
    g_millerHits.clear();
    g_crumbPaths.clear();
    CheckAnimFrame();
    g_notePaths.clear();
    D2D1_SIZE_F sz = g_rt->GetSize();
    const float W = sz.width, H = sz.height;

    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(cBg));
    DrawTabs(W);
    const bool zenMoving = ZenMoving();
    if (const float th = ToolH(); th > 0) {
        const size_t hits = g_hits.size();
        g_rt->PushAxisAlignedClip(D2D1::RectF(0, kTabH, W, kTabH + th), D2D1_ANTIALIAS_MODE_ALIASED);
        g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, th - kToolH));
        for (int p = 0; p < PaneCount(); ++p) DrawToolbar(p);
        g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        g_rt->PopAxisAlignedClip();
        if (th < kToolH) g_hits.resize(hits);
    }
    if (const float sw = SideW(); sw > 0) {
        const size_t hits = g_hits.size();
        g_rt->PushAxisAlignedClip(D2D1::RectF(0, kTabH, sw, H), D2D1_ANTIALIAS_MODE_ALIASED);
        g_rt->SetTransform(D2D1::Matrix3x2F::Translation(sw - kSideW, 0));
        DrawSidebar(H);
        g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        g_rt->PopAxisAlignedClip();
        if (sw < kSideW) g_hits.resize(hits);
    }
    static bool zenWasMoving = false;
    if (zenMoving) SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
    else if (zenWasMoving) {  // settled: the edit boxes go to their final places
        LayoutEdit();
        LayoutRename();
    }
    zenWasMoving = zenMoving;
    for (int p = 0; p < PaneCount(); ++p) {
        DrawHeader(p);
        DrawList(p, H);
        DrawStatus(p, H);
    }
    if (g_split) {
        // divider between panes: drag to change the width (double click: half and half)
        const float x = PaneGeo(1).L;
        Fill(D2D1::RectF(x, kTabH, x + 1, H), cBorder);
        g_hits.push_back({D2D1::RectF(x - 3, kTabH + ToolH(), x + 4, H - kStatus), HitKind::Divider, 0});
    }
    DrawPreview(W, H);
    DrawViewEditor(W, H);
    DrawPalette(W, H);
    DrawInstallPanel(W, H);
    DrawRenamePanel(W, H);
    DrawMenuEditor(W, H);
    DrawSettings(W, H);
    DrawAttrs(W, H);
    DrawInUse(W, H);
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
    int y = MulDiv(static_cast<int>(std::lround((kTabH + ToolH() / 2) * 100)), g_dpi, 9600) - h / 2 + px(1);  // +1: DirectWrite leaves the line spacing on top
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
    SetWindowTextW(g_edit, IsVirtual(T().path) ? VirtualTitle(T().path).c_str() : T().path.c_str());
    g_editing = true;  // first: in zen mode it brings the navigation bar back
    LayoutEdit();
    LayoutRename();
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
    if (g_zen) {
        LayoutRename();
        for (auto& tp : g_tabs) ClampScroll(*tp);
    }
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
    if (_wcsicmp(t.c_str(), L"Recycle Bin") == 0 || _wcsicmp(t.c_str(), L"shell:RecycleBinFolder") == 0 || IsBin(t)) {
        Navigate(T(), kBinPath, true);
        return;
    }
    if (_wcsicmp(t.c_str(), L"Network") == 0 || _wcsicmp(t.c_str(), L"shell:NetworkPlacesFolder") == 0 || IsNet(t)) {
        Navigate(T(), kNetPath, true);
        return;
    }
    wchar_t expanded[4096];
    ExpandEnvironmentStringsW(t.c_str(), expanded, 4096);
    t = expanded;
    if (t.size() == 2 && t[1] == L':') t += L'\\';
    wchar_t full[4096];
    if (!GetFullPathNameW(t.c_str(), 4096, full, nullptr)) return;
    std::wstring p = Normalize(full);
    if (p.rfind(L"\\\\", 0) == 0) {  // asking here would freeze the window if the server doesn't answer: the folder's page will say
        Navigate(T(), p, true);
        return;
    }
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
    } else if (geo.miller) {  // over the name, up to the chevron
        int rowTop = px(y), rowH = px(kRow);
        top = rowTop + (rowH - editH) / 2;
        left = px(cell.left + 4 + 8 + kIcon + 8 - 2);
        right = px(cell.right - 24);
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
    if (g_renaming || !t.list || t.list->error || t.sel < 0 || t.sel >= Count() || IsVirtual(t.path)) return;
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

void InUseAfterFailure(const std::wstring& what, const std::vector<std::wstring>& files, const std::wstring& reason, std::function<void()> retry);  // inuse.inl

// The rename itself, and what follows it. If the file is open somewhere, the In use panel offers to close that
// and does the rename again.
void RenameApply(const std::wstring& dir, const std::wstring& oldName, const std::wstring& newName, bool follow) {
    const std::wstring from = Join(dir, oldName.c_str()), to = Join(dir, newName.c_str());
    if (!MoveFileExW(ToExtendedPath(from).c_str(), ToExtendedPath(to).c_str(), 0)) {
        const DWORD e = GetLastError();
        InUseAfterFailure(L"Couldn't rename " + oldName, {from}, ErrorText(e), [=] { RenameApply(dir, oldName, newName, follow); });
        return;
    }
    Tab& t = T();
    UndoRecordRename(from, to);
    if (follow) t.pendingSelect = Key(dir) == Key(t.path) ? newName : to;
    TreeFollow(t, from, to);
    TagsFollow(from, to);
    Enqueue(dir, JobKind::Revalidate);
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
    CancelRename();
    if (newName.empty() || newName == oldName) return;
    if (newName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        S.message = L"Names can't contain \\ / : * ? \" < > |";
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    if (TestRefuses(dir)) return;
    RenameApply(dir, oldName, newName, follow);
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
    if (t.path.empty() || !t.list || t.list->error || TestRefuses(t.path)) return;
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
#include "templates.inl"
#include "menu.inl"
#include "dragdrop.inl"
#include "boxselect.inl"
#include "sidebar.inl"
void RunCommand(Cmd cmd);
void SetFoldersFirst(int mode);
void ViewEditorOpen(int pane);
#include "indexer.inl"
#include "palette.inl"
#include "select.inl"
#include "viewedit.inl"
#include "settings.inl"
#include "attrs.inl"
#include "inuse.inl"
#include "ops.inl"
#include "search.inl"
#include "dups.inl"
#include "syntax.inl"
#include "grep.inl"
#include "markdown.inl"
#include "preview.inl"
void PreviewReleaseBitmap() {
    g_pv.d2d.Reset();
    g_pv.svg.Reset();
    g_pv.svgTried = false;
    g_pvChecker.Reset();
    g_runBrushes.clear();
}
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
    out << "zen=" << (g_zen ? 1 : 0) << '\n';
    out << "preview=" << (g_preview ? 1 : 0) << '\n';
    out << "show=" << (g_hideDirs ? 1 : 0) << ',' << (g_hideFiles ? 1 : 0) << ',' << (g_hideExt ? 1 : 0) << '\n';
    out << "sort=" << g_sortBy.load() << ',' << (g_sortAsc.load() ? 1 : 0) << '\n';
    out << "split=" << (g_split ? 1 : 0) << ',' << static_cast<int>(g_splitRatio * 1000) << '\n';
    g_paneCur[g_act] = g_cur;
    out << "active=" << g_cur << '\n';
    out << "visible=" << g_paneCur[0] << ',' << g_paneCur[1] << '\n';
    for (auto& t : g_tabs) out << (t->pane ? "tab2=" : "tab=") << Utf8(t->path) << '\n';
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (g_tabs[i]->pinned) out << "pinned=" << i << '\n';
        if (g_tabs[i]->miller) out << "miller=" << i << '\n';
    }
    std::wstring rf = RecentsFile();
    if (!rf.empty()) {
        std::ofstream ro(rf, std::ios::binary);
        for (const std::wstring& p : g_recents) ro << Utf8(p) << '\n';
    }
    HeatSave();
}

struct Session {
    std::vector<std::pair<std::wstring, int>> tabs;  // path, pane
    std::vector<int> pinned;                         // indexes into tabs
    std::vector<int> miller;                         // the tabs in Miller columns
    int active = 0, visible[2] = {-1, -1};
    bool hasWindow = false;
    RECT rc{};
    bool maximized = false;
    bool sidebar = true, preview = false, split = false, zen = false;
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
        else if (k == "pinned") s.pinned.push_back(atoi(v.c_str()));
        else if (k == "miller") s.miller.push_back(atoi(v.c_str()));
        else if (k == "active") s.active = atoi(v.c_str());
        else if (k == "visible") sscanf_s(v.c_str(), "%d,%d", &s.visible[0], &s.visible[1]);
        else if (k == "sidebar") s.sidebar = v != "0";
        else if (k == "zen") s.zen = v != "0";
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
// For the tests: tabs and which are pinned, as they come back from the file.
std::wstring SessionRoundTrip() {
    SaveSession();
    const Session ses = LoadSession();
    std::wstring out = L"session: " + std::to_wstring(ses.tabs.size()) + L" tabs, pinned";
    for (int i : ses.pinned) out += L" " + ses.tabs[i].first;
    return out;
}

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

void ToggleZen() {
    const double now = NowMs();
    g_zenAt = ZenMoving() ? now - (kZenMs - (now - g_zenAt)) : now;  // turned back halfway: from where it is
    g_zen = !g_zen;
    for (auto& tp : g_tabs) ClampScroll(*tp);
    S.message = g_zen ? L"Zen mode: " + ShortcutFor(Cmd::Zen) + L" brings the bars back" : L"";
    LayoutEdit();
    LayoutRename();
    Invalidate();
    TouchSession();
}

void ToggleSidebar() {
    if (g_zen) {  // the sidebar comes back with everything else
        if (!g_sideVisible) g_sideVisible = true;
        ToggleZen();
        return;
    }
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
    if (IsVirtual(t.path)) {  // what's in there isn't files to work with
        if (IsBin(t.path) && cmd == Cmd::Delete) {
            BinDeleteSelection();
            return;
        }
        if (BinRefuses(cmd) || cmd == Cmd::Delete) {
            S.message = IsBin(t.path) ? L"In the Recycle Bin: restore it first" : L"These are computers, not files";
            Invalidate();
            return;
        }
    }
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
        case Cmd::NewFromTemplate:  // the palette, showing only the templates
            PaletteOpen(0);
            g_pal.text = L"New: ";
            PaletteFilter();
            Invalidate();
            break;
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
            if (t.nearRows > 0 && t.nearRows < Count())
                S.message = Thousands(Count() - t.nearRows) + L" selected; the close names stay out";
            else if (S.message.ends_with(L"the close names stay out"))
                S.message.clear();
            Invalidate();
            break;
        case Cmd::Favorite: ToggleFavorite(); break;
        case Cmd::NewTab: NewTab(t.path); break;
        case Cmd::CloseTab: CloseTab(g_cur); break;
        case Cmd::NextTab: CycleTab(1); break;
        case Cmd::PreviousTab: CycleTab(-1); break;
        case Cmd::Path: ShowPathEdit(); break;
        case Cmd::Sidebar: ToggleSidebar(); break;
        case Cmd::Zen: ToggleZen(); break;
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
        case Cmd::Settings: SettingsOpen(); break;
        case Cmd::Checkboxes: {
            g_cfg.checkboxes = (g_cfg.checkboxes + 1) % 3;
            const wchar_t* const v[] = {L"no", L"hover", L"always"};
            ConfigSetValue(L"general", L"checkboxes", v[g_cfg.checkboxes]);
            RefreshConfigSig();  // written and applied already
            S.message = g_cfg.checkboxes == 0 ? L"Checkboxes: off"
                        : g_cfg.checkboxes == 1 ? L"Checkboxes: on the row under the mouse and the selected ones"
                                                : L"Checkboxes: on every row";
            LayoutRename();
            Invalidate();
            break;
        }
        case Cmd::GoTo: PaletteOpen(1); break;
        case Cmd::SearchSubfolders:
            if (t.recursive) StopRecursive(t, true);
            else StartRecursive(t);
            break;
        case Cmd::FindDuplicates: {
            const bool was = t.dups;
            if (t.recursive) StopRecursive(t, true);
            if (!was) StartRecursive(t, true);
            break;
        }
        case Cmd::SelectExtraCopies:
            SelectExtraCopies(t);
            Invalidate();
            break;
        case Cmd::SelectPattern: SelPatternOpen(); break;
        case Cmd::SelectSameExtension:
            SelectSameExtension(t);
            Invalidate();
            break;
        case Cmd::InvertSelection:
            InvertSelection(t);
            Invalidate();
            break;
        case Cmd::Expand: SetExpanded(t, t.sel, !IsExpanded(t, t.sel)); break;
        case Cmd::Collapse: Collapse(); break;
        case Cmd::GoToLocation: GoToLocation(); break;
        case Cmd::Preview: PreviewToggle(); break;
        case Cmd::Split: SetSplit(!g_split); break;
        case Cmd::OtherPane: ActivatePane(1 - g_act); break;
        case Cmd::CopyToOther: TransferToOther(false); break;
        case Cmd::MoveToOther: TransferToOther(true); break;
        case Cmd::CopyTo: TransferPick(false); break;
        case Cmd::MoveTo: TransferPick(true); break;
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
        case Cmd::Attributes: AttrsOpen(); break;
        case Cmd::InUse: InUseOpen(); break;
        case Cmd::Unblock: Unblock(); break;
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
        case Cmd::PinTab: SetPinned(g_cur, !t.pinned); break;
        case Cmd::Restore:
            if (IsBin(t.path)) BinRestoreSelection();
            else S.message = L"Restoring works in the Recycle Bin";
            Invalidate();
            break;
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
        case Cmd::PreviewMarkdown:
            g_cfg.previewMarkdown = !g_cfg.previewMarkdown;
            ConfigSetValue(L"general", L"preview_markdown", g_cfg.previewMarkdown ? L"yes" : L"no");
            RefreshConfigSig();
            S.message = g_cfg.previewMarkdown ? L"Preview: Markdown rendered" : L"Preview: Markdown as its source";
            Invalidate();
            break;
        case Cmd::PreviewWrap:
            g_cfg.previewWrap = !g_cfg.previewWrap;
            ConfigSetValue(L"general", L"preview_wrap", g_cfg.previewWrap ? L"yes" : L"no");
            RefreshConfigSig();
            S.message = g_cfg.previewWrap ? L"Preview: long lines wrap" : L"Preview: long lines run off the edge";
            Invalidate();
            break;
        case Cmd::Tag: TagPaletteOpen(); break;
        case Cmd::FolderColor: FolderColorMenu(); break;
        case Cmd::ViewList:
            if (!t.miller) SetLayout(0);
            else {
                ToggleMiller();
                if (LayoutOf(t)) SetLayout(0);
            }
            break;
        case Cmd::ViewGrid:
            if (t.miller) ToggleMiller();
            if (LayoutOf(t) <= 0) SetLayout(144);
            break;
        case Cmd::ViewColumns: ToggleMiller(); break;
        case Cmd::SizeBars: SetSizeBars(!g_cfg.sizeBars); break;
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
    if (row < 0 || row >= Count() || FlatView(t) || GeoFor(t).miller || !EntryAt(t, row).isDir()) return false;
    Columns c = Cols(g_act);
    float cx = c.nameL + RowIndent(t, row);
    return x >= cx - 4 && x < cx + kChev + 2;
}

// Is the point over a row's selection checkbox? In the list it is at the left edge, in the grid at the cell's corner.
// It counts wherever it would show: with "hover", the mouse is over the row, so it is there.
bool OnCheckbox(int row, float x, float y) {
    Tab& t = T();
    if (!g_cfg.checkboxes || row < 0 || row >= Count() || IsHeader(t, row) || GeoFor(t).miller) return false;
    if (GeoFor(t).grid) {
        const D2D1_RECT_F cell = RowRect(t, row);
        const float top = ListTop() + cell.top - t.scroll;
        return x >= cell.left + 2 && x < cell.left + 32 && y >= top + 2 && y < top + 32;
    }
    const Columns c = Cols(g_act);
    return x >= c.checkL - 4 && x < c.checkL + kCheckW - 2;
}

// A click on a row's checkbox: that row joins or leaves the selection and the rest stays, as with Ctrl+click.
// The selection a folder opens with (only the cursor) was nobody's choice: the first checkbox replaces it.
// With Shift, the whole range from the last one is checked.
void CheckboxToggle(int r) {
    Tab& t = T();
    if (KeyDown(VK_SHIFT) && Count() > 0) {
        const int a = std::clamp(t.anchor, 0, Count() - 1);
        for (int i = std::min(a, r); i <= std::max(a, r); ++i)
            if (!IsHeader(t, i)) t.selMask[i] = 1;
        t.sel = r;
    } else {
        if (t.implicitSel && SelCount(t) == 1 && IsSel(t, t.sel) && t.sel != r) t.selMask[t.sel] = 0;
        SelectToggle(t, r);
    }
    t.implicitSel = false;
    Invalidate();
}

bool g_divDrag = false;  // dragging the split view divider
bool g_eatSysChar = false;  // the Alt key combo was already used: its WM_SYSCHAR must not beep
int g_tabWheelAcc = 0;      // Ctrl+wheel on high-resolution wheels: accumulates until a full step

// New width of a column (a ColKind), always leaving room for the name and the other columns: as wide as it can be
// and still fit, by Cols()' own rule.
void SetColWidth(int kind, float w, int pane) {
    const PaneX g = PaneGeo(pane);
    const Columns c = Cols(pane);
    float others = 0;
    for (int j = 0; j < c.n; ++j)
        if (c.kind[j] != kind) others += g_colW[c.kind[j]] + 18;
    const ColDef& d = kColDefs[kind];
    g_colW[kind] = std::clamp(w, d.min, std::max(d.min, std::min(d.max, g.R - g.L - 308 - others)));
    LayoutRename();
    Invalidate();
}

void SaveColWidth(int kind) {
    ConfigSetValue(L"columns", std::wstring(kColDefs[kind].key) + L"_width", std::to_wstring(std::lround(g_colW[kind])));
    RefreshConfigSig();
}

void ColDragEnd() {
    if (!g_colDrag) return;
    const int kind = g_colDrag - 1;
    g_colDrag = 0;
    SaveColWidth(kind);
    Invalidate();
}

// --- a column's title: a click sorts (Modified, Size), a drag moves the column. Like the sidebar, a click acts on
// release, since until then it may become a drag.
int ColIndex(const Columns& c, int kind) {
    for (int j = 0; j < c.n; ++j)
        if (c.kind[j] == kind) return j;
    return -1;
}

bool ColMoveStart(const Hit& h, float x) {
    if (h.kind != HitKind::HeadCol) return false;
    const int pane = h.idx / ColKinds, kind = h.idx % ColKinds;
    const Columns c = Cols(pane);
    const int j = ColIndex(c, kind);
    if (j < 0) return false;
    g_colMove = {};
    g_colMove.press = true;
    g_colMove.pane = pane;
    g_colMove.kind = kind;
    g_colMove.x0 = g_colMove.x = x;
    g_colMove.grab = x - c.l[j];
    g_colMove.before = g_cfg.cols;
    SetCapture(g_hwnd);
    return true;
}

// When the dragged title's middle passes a neighbor's middle, they swap places and the neighbor slides to its new one.
// (Swapping back needs the middle to cross the neighbor's new middle, so it doesn't flicker between the two.)
void ColMoveTo(float x) {
    ColMove& d = g_colMove;
    if (!d.press || (!d.active && std::abs(x - d.x0) < 4)) return;
    d.active = true;
    d.x = x;
    const float mid = x - d.grab + g_colW[d.kind] / 2;
    for (int guard = 0; guard < ColKinds; ++guard) {
        const Columns c = Cols(d.pane);
        const int j = ColIndex(c, d.kind);
        if (j < 0) break;
        int to = -1;
        if (j > 0 && mid < c.l[j - 1] + g_colW[c.kind[j - 1]] / 2) to = j - 1;
        else if (j + 1 < c.n && mid > c.l[j + 1] + g_colW[c.kind[j + 1]] / 2) to = j + 1;
        if (to < 0) break;
        const int other = c.kind[to];
        std::iter_swap(std::find(g_cfg.cols.begin(), g_cfg.cols.end(), d.kind), std::find(g_cfg.cols.begin(), g_cfg.cols.end(), other));
        const Columns after = Cols(d.pane);
        const int k = ColIndex(after, other);
        if (k >= 0) ColSlideBy(other, c.l[to] - after.l[k]);
    }
    Invalidate();
}

// keep: drop it where it is (or, without a drag, the click); otherwise (Esc) everything goes back to how it was.
void ColMoveEnd(bool keep) {
    if (!g_colMove.press) return;
    const ColMove d = g_colMove;
    g_colMove = {};
    if (GetCapture() == g_hwnd) ReleaseCapture();
    Invalidate();
    if (!d.active) {
        if (keep && ColSortOf(d.kind) >= 0) SetSort(static_cast<SortBy>(ColSortOf(d.kind)));
        return;
    }
    // what was being dragged slides from under the mouse to its place
    const Columns c = Cols(d.pane);
    const int j = ColIndex(c, d.kind);
    if (j >= 0) ColSlideBy(d.kind, std::clamp(d.x - d.grab, c.l[0], c.right - g_colW[d.kind]) - c.l[j]);
    if (!keep) g_cfg.cols = d.before;
    if (g_cfg.cols != d.before) ColSave();
}

// Right click on the column titles: which ones show. Name always does.
void OpenColumnMenu(POINT screenPt) {
    std::vector<MenuItem> items;
    MenuItem name;
    name.text = L"Name";
    name.checked = true;
    name.enabled = false;
    items.push_back(std::move(name));
    for (int k = 0; k < ColKinds; ++k) {
        MenuItem m;
        m.text = kColDefs[k].label;
        m.id = static_cast<UINT>(k + 1);
        m.checked = ColShown(k);
        items.push_back(std::move(m));
    }
    MenuItem sep, bars;
    sep.sep = true;
    bars.text = L"Size bars";
    bars.id = 100;
    bars.checked = g_cfg.sizeBars;
    items.push_back(std::move(sep));
    items.push_back(std::move(bars));
    const RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(std::move(items), anchor, [](UINT id) {
        if (id >= 1 && id <= ColKinds) ColShow(static_cast<int>(id) - 1, !ColShown(static_cast<int>(id) - 1));
        if (id == 100) SetSizeBars(!g_cfg.sizeBars);
    });
}

// Is the point on a pane's column titles? (with the view button and the checkbox at their left)
bool OnColumnTitles(float x, float y) {
    const float y0 = kTabH + ToolH();
    const int pane = PaneAt(x, y);
    if (pane < 0 || y < y0 || y >= y0 + kHeadH) return false;
    const PaneX g = PaneGeo(pane);
    return x >= g.L && x < g.R;
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
void FitColumn(int kind, int pane) {
    Tab& t = PT(pane);
    float w = MeasureW(ColLabel(kind, pane), g_fmtRight.Get()) + 16;
    const int n = std::min(RowCount(t), 3000);
    for (int i = 0; i < n; ++i) {
        if (IsHeader(t, i)) continue;
        const Entry& e = EntryAt(t, i);
        const bool drives = t.rows[i].l->path.empty();
        bool faint = false;
        // what is not known yet is not asked for here: it would queue thousands of files at once
        const std::wstring s = CellText(kind, e, drives ? RowName(t, i) + L"\\" : RowPath(t, i), drives, faint, false);
        if (!s.empty()) w = std::max(w, MeasureW(s, g_fmtRight.Get()) + 4);
    }
    SetColWidth(kind, std::ceil(w), pane);
    SaveColWidth(kind);
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
        case HitKind::Miller: MillerOpen(h.idx); return;
        case HitKind::HeadCol:
            if (ColSortOf(h.idx % ColKinds) >= 0) SetSort(static_cast<SortBy>(ColSortOf(h.idx % ColKinds)));
            return;
        case HitKind::HeadCheck: {  // all of them, or (when all are) none
            ActivatePane(h.idx);
            Tab& t = T();
            const bool onlyReal = t.nearRows > 0 && t.nearRows < Count();
            bool all = true;
            for (int i = 0; i < Count() && all; ++i)
                if (!IsHeader(t, i) && !(onlyReal && t.rows[i].closeness) && !IsSel(t, i)) all = false;
            if (all) std::fill(t.selMask.begin(), t.selMask.end(), uint8_t{0});
            else SelectAll(t);
            t.implicitSel = false;
            S.message = all ? L"Nothing selected" : Thousands(SelCount(t)) + L" selected";
            Invalidate();
            return;
        }
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
                g_colDrag = h.idx % ColKinds + 1;
                g_colDragPane = h.idx / ColKinds;
                g_colDragX = x;
                g_colDragW = g_colW[g_colDrag - 1];
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
    // the context menu editor: a panel over everything, with the mouse and the keyboard
    if (msg == WM_APP_MENUED) {
        MenuEditorLoad(static_cast<int>(wp));
        return 0;
    }
    if (g_ed.open) {
        if (msg == WM_KEYDOWN) {
            MenuEditorKey(wp);
            return 0;
        }
        if (msg == WM_CHAR) return 0;
        if (msg == WM_MOUSEMOVE) {
            MenuEditorMouseMove(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            MenuEditorClick(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONUP) {
            MenuEditorMouseUp();
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {
            MenuEditorWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        if (msg == WM_CAPTURECHANGED) g_ed.dragging = false;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN) return 0;
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g_ed.dragging ? IDC_SIZENS : IDC_ARROW));
            return TRUE;
        }
    }
    // in use: a panel over everything
    if (g_iu.open) {
        if (msg == WM_KEYDOWN || (msg == WM_SYSKEYDOWN && wp != VK_F4)) {
            InUseKey(wp);
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0;
        if (msg == WM_MOUSEMOVE) {
            InUseMouseMove(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            InUseClick(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL) return 0;
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g_iu.hovering ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
    }
    // attributes and dates: a panel over everything
    if (g_at.open) {
        if (msg == WM_KEYDOWN || (msg == WM_SYSKEYDOWN && wp != VK_F4)) {
            AttrsKey(wp);
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_SYSCHAR) return 0;
        if (msg == WM_MOUSEMOVE) {
            AttrsMouseMove(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            AttrsClick(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL) return 0;
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g_at.hovering && g_at.hover.k == AtK::Field ? IDC_IBEAM : g_at.hovering ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
    }
    // settings: a panel over everything, with the mouse and the keyboard (its text box has its own)
    if (g_set.open) {
        if (msg == WM_KEYDOWN || (msg == WM_SYSKEYDOWN && wp != VK_F4)) {
            SettingsKey(wp);
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_SYSCHAR) {
            if (wp >= 32) SettingsChar(static_cast<wchar_t>(wp));
            return 0;
        }
        if (msg == WM_MOUSEMOVE) {
            SettingsMouseMove(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            SettingsClick(DipX(lp), DipY(lp));
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {
            SettingsWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        if (msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN) return 0;
        if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
            const bool field = g_set.hovering && g_set.hoverHit.k == SetK::Field;
            SetCursor(LoadCursorW(nullptr, field ? IDC_IBEAM : g_set.hovering ? IDC_HAND : IDC_ARROW));
            return TRUE;
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
            else RecRevalidateShown();  // something may have been done to the results elsewhere
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
        case WM_APP_DUP:
            DupApply(wp, lp);
            return 0;
        case WM_APP_GREP:
            ApplyGrepResult(reinterpret_cast<GrepResult*>(lp));
        case WM_APP_THUMB:
            g_thumbDirty = false;
            Invalidate();
            return 0;
        case WM_APP_META:
            g_metaDirty = false;
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
            PaletteRefresh();
            return 0;
        }
        case WM_APP_IDXGONE: {
            std::unique_ptr<IdxGone> g(reinterpret_cast<IdxGone*>(lp));
            auto at = std::find(g_ixRes.hits.begin(), g_ixRes.hits.end(), g->path);
            if (g->gen != g_ixGen || at == g_ixRes.hits.end()) return 0;
            g_ixRes.hits.erase(at);
            --g_ixRes.total;
            PaletteRefresh();
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
            if (wp == kTimerRecCheck) {
                RecRevalidateShown();
                if (--g_recChecksLeft > 0) SetTimer(hwnd, kTimerRecCheck, g_recChecksLeft == 2 ? 2000 : 5000, nullptr);
            }
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
            if (wp == kTimerInUse) InUseTick();
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
            if (g_colMove.press) {
                if (wp == VK_ESCAPE) ColMoveEnd(false);
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
            if (ColMoveStart(HitAt(x, y), x)) return 0;
            if (HitAt(x, y).kind == HitKind::Miller) {  // another Miller column: the tab goes there
                MillerOpen(HitAt(x, y).idx);
                return 0;
            }
            if (HitAt(x, y).kind == HitKind::FolderNote) {  // before the row under it takes the click
                OnClick(x, y, false);
                return 0;
            }
            int r = HitRow(x, y);
            if (r >= 0 && OnCheckbox(r, x, y)) {  // before the row takes the click: no drag, no box
                CheckboxToggle(r);
                return 0;
            }
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
            if (g_colMove.press) {
                ColMoveTo(DipX(lp));
                ColMoveEnd(true);
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
            ColMoveEnd(false);
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
            else if (side.kind == HitKind::Tab || side.kind == HitKind::TabClose) OpenTabMenu(side.idx, pt);
            else if (side.kind == HitKind::Crumb) CrumbMenu(CrumbSiblingsDir(g_crumbPaths[side.idx]), side.r);
            else if (side.kind == HitKind::CrumbChev) CrumbMenu(g_crumbPaths[side.idx], side.r);
            else if (OnColumnTitles(x, y)) OpenColumnMenu(pt);
            else if (side.kind == HitKind::Miller) {  // there first, then the menu of what was clicked
                const bool item = side.idx < static_cast<int>(g_millerHits.size()) && !g_millerHits[side.idx].name.empty();
                MillerOpen(side.idx);
                g_millerClickAt = 0;
                OpenShellMenu(pt, item ? T().sel : -1);
            }
            else if (PaneAt(x, y) == g_act && y >= ListTop() && y < ListBottom()) OpenShellMenu(pt, HitRow(x, y));
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            PressEnd(false);
            if (g_millerClickAt && NowMs() - g_millerClickAt < GetDoubleClickTime()) {  // an item of another column
                g_millerClickAt = 0;
                OpenSelected(false);
                return 0;
            }
            Hit dh = HitAt(DipX(lp), DipY(lp));
            if (dh.kind == HitKind::FolderNote) return 0;  // the first click already did it
            if (dh.kind == HitKind::ColEdge) {
                FitColumn(dh.idx % ColKinds, dh.idx / ColKinds);
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
            if (r >= 0 && OnCheckbox(r, DipX(lp), DipY(lp))) {  // a second quick click on a checkbox: another toggle, not open
                CheckboxToggle(r);
                return 0;
            }
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
        case WM_MOUSEHWHEEL: {  // the wheel tilted, or two fingers sideways: tables in the preview
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x * 96.f / g_dpi, y = pt.y * 96.f / g_dpi;
            if (g_preview && x >= ClientDip().width - PreviewW() && y >= kTabH + ToolH()) PreviewWheel(-GET_WHEEL_DELTA_WPARAM(wp) / 120.f, true);
            return 0;
        }
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
            if (g_preview && x >= ClientDip().width - PreviewW() && y >= kTabH + ToolH()) {
                PreviewWheel(delta / 120.f, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) || (g_test && KeyDown(VK_SHIFT)));
                return 0;
            }
            if (MillerWheel(x, y, delta)) return 0;
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
            if (g_colMove.press) {
                if (wp & MK_LBUTTON) ColMoveTo(x);
                else ColMoveEnd(true);
                return 0;
            }
            if (g_colDrag) {
                // columns are anchored to the right: the left edge follows the mouse
                SetColWidth(g_colDrag - 1, g_colDragW + g_colDragX - x, g_colDragPane);
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
        if (IsBin(a)) return kBinPath;
        if (IsNet(a)) return kNetPath;
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
    if (IsVirtual(p) || IsServerPath(p)) return true;
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
    for (const std::wstring& r : g_recents) NetRememberPath(r);
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
    g_zen = ses.zen;
    g_preview = ses.preview;
    g_splitRatio = ses.ratio;
    if (g_adminWindow && BgAdminPlace(show)) {
        // beside the normal window
    } else if (ses.hasWindow) {
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
        g_tabs.back()->pinned = std::find(ses.pinned.begin(), ses.pinned.end(), static_cast<int>(i)) != ses.pinned.end();
        g_tabs.back()->miller = std::find(ses.miller.begin(), ses.miller.end(), static_cast<int>(i)) != ses.miller.end();
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

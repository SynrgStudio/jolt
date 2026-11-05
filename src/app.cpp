// Jolt — explorador de archivos para Windows, pensado para sentirse instantáneo.
//
// Ideas centrales:
//  - Listado con NtQueryDirectoryFile en lotes de 64 KiB (un hilo aparte).
//  - Cada carpeta visitada queda en memoria: volver a ella es instantáneo,
//    y en segundo plano se revisa si cambió.
//  - Se vigila la carpeta actual; si cambia algo, se actualiza sola.
//  - Se precarga la carpeta seleccionada para que Enter sea instantáneo.
//  - Los iconos reales de Windows se resuelven en otro hilo, solo para las filas visibles.
//  - Se dibuja solo lo visible (Direct2D + DirectWrite).
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

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "oleaut32.lib")  // UI Automation del modo de prueba (SysFreeString)

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- diseño (DIPs)
constexpr float kTabH = 36.f, kToolH = 42.f, kHeadH = 28.f, kStatus = 26.f;
float kRow = 25.f;  // configurable (config.txt)
constexpr float kCapBtnW = 46.f;  // minimizar / maximizar / cerrar
constexpr float kTitleLeft = 118.f;  // zona fija a la izquierda de las pestañas: futuro menú + botón de barra lateral
float kSideW = 208.f;  // configurable
bool g_preview = false;  // vista previa a la derecha
float g_previewW = 360.f;  // configurable
constexpr float kSideRow = 25.f;
float kDateW = 118.f, kSizeW = 72.f;  // configurables (y arrastrables desde el encabezado)
constexpr float kDateMin = 64.f, kDateMax = 400.f, kSizeMin = 48.f, kSizeMax = 300.f;
constexpr float kPad = 12.f, kTabMaxW = 190.f, kTabMinW = 70.f;
constexpr float kIcon = 16.f;

// Colores (configurables en config.txt; estos son los valores por defecto)
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
// g_dpi es el DPI con el que se dibuja: el del monitor por el zoom de la interfaz. Como todo el diseño
// está en DIPs, cambiarlo escala la app entera. El zoom va en píxeles de fuente sobre 12 para que el
// texto caiga en tamaños enteros (12 = 100 %).
UINT g_dpi = 96, g_sysDpi = 96;
constexpr int kUiPxMin = 9, kUiPxMax = 24;
int g_uiPx = 12;
void ApplyDpi() { g_dpi = static_cast<UINT>(MulDiv(static_cast<int>(g_sysDpi), g_uiPx, 12)); }

// Registro opcional: con JOLT_DEBUG=1 escribe en %TEMP%\jolt.log.
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
// Modo de prueba (testmode.inl): la ventana no se muestra y Ctrl/Shift/Alt se simulan.
bool g_test = false;
UINT g_testMods = 0;      // 1 = Ctrl, 2 = Shift, 4 = Alt
bool g_testActive = true; // la ventana "activa" en el modo de prueba (paso "activar si|no")
std::wstring g_dataDir;   // carpeta de datos (vacío = %APPDATA%\Jolt)
bool KeyDown(int vk) {
    if (g_test)
        return (vk == VK_CONTROL && (g_testMods & 1)) || (vk == VK_SHIFT && (g_testMods & 2)) || (vk == VK_MENU && (g_testMods & 4));
    return GetKeyState(vk) < 0;
}

std::atomic<int> g_sortBy{static_cast<int>(SortBy::Name)};
std::atomic<bool> g_sortAsc{true};
std::atomic<int> g_foldersFirst{2};  // 0 = mezcladas, 1 = siempre primero, 2 = primero solo por nombre
bool FoldersFirstFor(SortBy by) {
    int m = g_foldersFirst.load();
    return m == 1 || (m == 2 && by == SortBy::Name);
}

// ---------------------------------------------------------------- trabajo en segundo plano
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
        // Una precarga vieja ya no sirve si el usuario se movió a otro lado.
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

// ---------------------------------------------------------------- vigilancia de cambios
// Se vigilan las carpetas visibles (una por panel); con el árbol abierto, también sus subcarpetas.
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

// ---------------------------------------------------------------- rutas
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
    if (p.rfind(L"\\\\", 0) == 0 && p.find(L'\\', 2) == pos) return L"";  // \\servidor\recurso
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

// Unidades: "Stuff (D:)". Se calcula una vez por letra.
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
        label = type == DRIVE_FIXED      ? L"Disco local"
                : type == DRIVE_REMOVABLE ? L"Unidad extraíble"
                : type == DRIVE_REMOTE    ? L"Unidad de red"
                : type == DRIVE_CDROM     ? L"Unidad de CD"
                                          : L"Unidad";
    }
    std::wstring s = label + L" (" + std::wstring(1, letter) + L":)";
    return g_driveLabels.emplace(letter, s).first->second;
}

// Fracción usada del disco (0..1), o -1 si no aplica. Se recalcula cada 10 s por unidad.
float DriveUsage(wchar_t letter) {
    struct Cached {
        float usage = -1;
        double at = -1e9;
    };
    static std::unordered_map<wchar_t, Cached> cache;
    Cached& c = cache[letter];
    double now = NowMs();
    if (now - c.at < 10000) return c.usage;
    c.at = now;
    wchar_t root[4] = {letter, L':', L'\\', 0};
    UINT type = GetDriveTypeW(root);
    c.usage = -1;
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
        ULARGE_INTEGER freeB, totalB;
        if (GetDiskFreeSpaceExW(root, nullptr, &totalB, &freeB) && totalB.QuadPart > 0)
            c.usage = static_cast<float>(1.0 - static_cast<double>(freeB.QuadPart) / static_cast<double>(totalB.QuadPart));
    }
    return c.usage;
}

// ---------------------------------------------------------------- favoritos
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
    return dir + L"\\favoritos.txt";
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

void SaveFavs() {
    std::wstring f = FavFile();
    if (f.empty()) return;
    std::ofstream out(f, std::ios::binary);
    out << "# Favoritos de Jolt: una linea por carpeta, \"Nombre|Ruta\" o solo la ruta.\n";
    for (const Favorite& v : g_favs) out << Utf8(v.name) << '|' << Utf8(v.path) << '\n';
}

void LoadFavs() {
    g_favs.clear();
    std::wstring f = FavFile();
    std::ifstream in(f, std::ios::binary);
    if (!in) {
        struct Def {
            const wchar_t* name;
            const KNOWNFOLDERID* id;
        } defs[] = {{L"Escritorio", &FOLDERID_Desktop},
                    {L"Descargas", &FOLDERID_Downloads},
                    {L"Documentos", &FOLDERID_Documents},
                    {L"Imágenes", &FOLDERID_Pictures},
                    {L"Usuario", &FOLDERID_Profile}};
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

// ---------------------------------------------------------------- estado
// Una fila visible: un elemento de algún listado (la carpeta de la pestaña, una subcarpeta
// desplegada en el árbol, o una carpeta de la búsqueda en subcarpetas).
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
    int pane = 0;                // 0 = izquierda, 1 = derecha (vista dividida)
    std::wstring pendingSelect;  // nombre (en la carpeta) o ruta completa a seleccionar cuando aparezca
    int sel = 0;                 // cursor
    int anchor = 0;              // inicio del rango con Shift
    std::vector<uint8_t> selMask;  // paralelo a rows
    std::wstring filter;           // texto del filtro difuso ("" = sin filtro)
    std::vector<Row> rows;         // filas visibles: árbol aplanado y, con filtro, ordenadas por puntaje
    // árbol: carpetas desplegadas (clave -> ruta) y sus listados
    std::unordered_map<std::wstring, std::wstring> expanded;
    std::unordered_map<std::wstring, std::shared_ptr<const Listing>> kids;
    // grupos plegados o desplegados a mano en esta visita (título -> plegado); al volver a entrar,
    // manda lo que diga la vista
    std::unordered_map<std::wstring, bool> fold;
    // búsqueda en subcarpetas
    bool recursive = false, recDone = false;
    uint64_t recGen = 0;
    int recDirs = 0;
    std::vector<std::shared_ptr<const Listing>> rec;
    float scroll = 0;
    std::vector<std::wstring> back, fwd;
    float slide = 0;     // desplazamiento inicial de la animación, en lugares de pestaña
    double slideAt = 0;  // cuándo empezó
};

std::vector<std::unique_ptr<Tab>> g_tabs;
int g_cur = 0;              // pestaña activa (la visible del panel activo)
bool g_split = false;       // vista dividida
int g_act = 0;              // panel activo
int g_paneCur[2] = {0, 0};  // pestaña visible de cada panel
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
std::unordered_map<std::wstring, std::wstring> g_lastSel;
std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> g_expandMem;  // árbol por carpeta
std::vector<std::wstring> g_recents;  // carpetas y archivos visitados, lo más reciente primero
bool g_editing = false;
bool g_renaming = false;
int g_hoverRow = -1, g_hoverPane = 0, g_hoverHeader = -1;
std::unordered_set<std::wstring> g_cutKeys;  // rutas "cortadas" (se dibujan atenuadas hasta pegar)
bool g_ignoreClipUpdate = false;
int g_dropRow = -1, g_dropPane = 0, g_dropSide = -1, g_dropTab = -1;  // destino resaltado durante un arrastre

// Zonas clickeables, recalculadas en cada dibujo.
enum class HitKind { None, Tab, TabClose, TabNew, SideToggle, AppMenu, Back, Fwd, Up, Crumb, PathEmpty, Side, Header, Divider, ViewBtn, FoldBtn, ColEdge };
struct Hit {
    D2D1_RECT_F r;
    HitKind kind;
    int idx;
};
std::vector<Hit> g_hits;
std::vector<std::wstring> g_crumbPaths;
std::vector<std::wstring> g_sidePaths;
Hit g_hover{{}, HitKind::None, -1};
bool g_sideVisible = true;
float SideW() { return g_sideVisible ? kSideW : 0.f; }
// Botones de la barra de título (zona no cliente): hover y presionado se siguen por código HT*.
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

// --- filas
int RowCount(const Tab& t) { return static_cast<int>(t.rows.size()); }
int Count() { return RowCount(T()); }
// Las filas con e < 0 son títulos de grupo (vistas agrupadas): no se seleccionan ni se abren.
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

// ¿El grupo gi está plegado? Lo hecho a mano en esta visita, o si no lo que diga la vista.
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
            out.push_back({l, -1 - static_cast<int>(gi), 0});  // título del grupo
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

// Arma las filas visibles: el árbol (o lo encontrado en subcarpetas) y, si hay filtro, solo lo que
// coincide, ordenado por puntaje como fzf.
void BuildRows(Tab& t) {
    t.rows.clear();
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
    if (t.filter.empty()) return;
    std::wstring pat = Lower(t.filter);
    std::vector<std::pair<int, int>> hits;  // (puntaje, fila)
    for (size_t i = 0; i < t.rows.size(); ++i) {
        const Row& r = t.rows[i];
        if (r.e < 0) continue;
        const Entry& e = r.l->entries[r.e];
        int sc;
        if (FuzzyMatch(r.l->name(e), e.nameLen, pat, sc, nullptr)) hits.push_back({sc, static_cast<int>(i)});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Row> out;
    out.reserve(hits.size());
    for (auto& h : hits) out.push_back(t.rows[h.second]);
    t.rows.swap(out);
}

// Fila de un nombre de la carpeta de la pestaña, o de una ruta completa (si tiene "\").
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
    return std::clamp(g_previewW, 200.f, std::max(200.f, W * 0.6f));
}

// Límites horizontales de cada panel: [L, R) para encabezado y lista; [TL, TR) para la barra de ruta y
// la de estado (que pasan por encima de la barra lateral y de la vista previa).
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

// Panel bajo un punto (barra de ruta, lista o estado), o -1.
int PaneAt(float x, float y) {
    if (y < kTabH) return -1;
    const bool band = y < kTabH + kToolH || y >= ListBottom();
    for (int p = 0; p < PaneCount(); ++p) {
        PaneX g = PaneGeo(p);
        if (band ? (x >= g.TL && x < g.TR) : (x >= g.L && x < g.R)) return p;
    }
    return -1;
}

void ClampScroll(Tab& t) {
    int n = RowCount(t);
    float maxS = std::max(0.f, n * kRow - ListHeight());
    t.scroll = std::clamp(t.scroll, 0.f, maxS);
}

void EnsureVisible(Tab& t) {
    float top = t.sel * kRow, h = ListHeight();
    if (top < t.scroll) t.scroll = top;
    if (top + kRow > t.scroll + h) t.scroll = top + kRow - h;
    ClampScroll(t);
}

std::wstring SelectedName(const Tab& t) {
    if (t.sel < 0 || t.sel >= RowCount(t)) return L"";
    return RowName(t, t.sel);
}

void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }

// --- selección
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

// Si i cae en un título de grupo, la fila de contenido más cercana en esa dirección (o en la otra).
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

// Carpeta de la fila del cursor (en el árbol o la búsqueda puede no ser la de la pestaña).
std::wstring SelDir(const Tab& t) { return t.sel >= 0 && t.sel < RowCount(t) ? RowDir(t, t.sel) : t.path; }

// Nombres seleccionados que están en la carpeta del cursor, listos para el Shell ("C:" -> "C:\").
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

// Rutas completas de todo lo seleccionado (de cualquier carpeta).
std::vector<std::wstring> SelectedFullPaths(const Tab& t) {
    std::vector<std::wstring> out;
    if (t.path.empty() && !t.recursive) return out;  // unidades: no se copian
    for (int i = 0; i < RowCount(t); ++i)
        if (IsSel(t, i)) out.push_back(RowPath(t, i));
    return out;
}

// Listado listo para mostrar: con la vista de su carpeta aplicada (filtros, orden, grupos) y sin
// ocultos si así se pidió. Se guarda junto al listado: mientras nada cambie, no se recalcula.
std::shared_ptr<const Listing> Prepared(std::shared_ptr<const Listing> l) {
    if (!l || l->error || l->isView) return l;
    if (l->view && l->viewSig == g_viewEpoch) return l->view;
    auto out = ApplyView(*l, *ViewFor(l->path));
    l->view = out;
    l->viewSig = g_viewEpoch;
    return out;
}

// Lugar del cursor y de la selección, por ruta: sobrevive a recargas, al árbol y al filtro.
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

// Rearma las filas y ubica el cursor: pendingSelect, o el lugar guardado (keep), o prefer.
void Rebuild(Tab& t, const Place* keep, const std::wstring& prefer) {
    BuildRows(t);
    const int n = RowCount(t);
    int idx = -1;
    if (!t.pendingSelect.empty()) {
        idx = FindRow(t, t.pendingSelect);
        if (idx >= 0 || !keep) t.pendingSelect.clear();
    }
    if (idx < 0 && keep && !keep->cur.empty()) {
        // si el elemento ya no está (p. ej. se plegó su carpeta), el ancestro más cercano visible
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
        if (idx >= 0 && keep->row >= 0) t.scroll += (t.sel - keep->row) * kRow;  // que no salte en pantalla
        (void)old;
        ClampScroll(t);
    } else {
        t.sel = idx >= 0 ? idx : 0;
        t.scroll = t.sel * kRow - ListHeight() / 2 + kRow / 2;  // la selección queda centrada
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
    if (keepPlace) p = SavePlace(t);  // antes de soltar el listado viejo: las filas apuntan a él
    t.list = Prepared(std::move(l));
    t.loading = false;
    Rebuild(t, keepPlace ? &p : nullptr, prefer);
}

std::wstring TabTitle(const Tab& t) {
    if (t.path.empty()) return L"Este equipo";
    if (IsDriveRoot(t.path)) return DriveLabel(t.path[0]);
    return LeafOf(t.path);
}

void UpdateTitle() { SetWindowTextW(g_hwnd, (TabTitle(T()) + L" — Jolt").c_str()); }

// Vigila las carpetas visibles (las dos de la vista dividida); con el árbol abierto, también las subcarpetas.
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
void TreeFollow(Tab& t, const std::wstring& from, const std::wstring& to);
void TouchSession();
void StopRecursive(Tab& t, bool rebuild);
std::wstring g_renameAfterSelect;  // carpeta o archivo nuevo: entrar en renombrado cuando aparezca en la lista

// Carga (o toma de la memoria) el listado de una subcarpeta desplegada.
void LoadKid(Tab& t, const std::wstring& k, const std::wstring& path) {
    auto it = g_cache.find(k);
    if (it != g_cache.end()) {
        t.kids[k] = Prepared(it->second);
        Enqueue(path, JobKind::Revalidate);
        return;
    }
    auto j = Enqueue(path, JobKind::Load);
    if (WaitForSingleObject(j->done, 30) == WAIT_OBJECT_0 && j->result && !j->taken.exchange(true)) {
        if (j->result->error == 0) g_cache[k] = j->result;
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
    t.has = true;
    t.path = path;
    t.filter.clear();
    t.rows.clear();
    t.selMask.clear();
    t.expanded.clear();
    t.kids.clear();
    t.fold.clear();  // los grupos arrancan como diga la vista
    AddRecent(path);
    ++g_navGen;
    auto ls = g_lastSel.find(Key(path));
    t.pendingSelect = ls != g_lastSel.end() ? ls->second : L"";
    // el árbol que quedó abierto la última vez en esta carpeta
    auto em = g_expandMem.find(Key(path));
    if (em != g_expandMem.end()) {
        t.expanded = em->second;
        for (auto& [k, p] : t.expanded) LoadKid(t, k, p);
    }

    auto it = g_cache.find(Key(path));
    if (it != g_cache.end()) {
        // En memoria: se muestra ya, y se revisa en segundo plano por si cambió.
        ShowListing(t, it->second, t.pendingSelect, false);
        if (current) S.fromCache = true;
        Enqueue(path, JobKind::Revalidate);
    } else {
        auto j = Enqueue(path, JobKind::Load);
        // Para carpetas locales casi siempre está listo antes de 30 ms: así evitamos
        // un cuadro intermedio de "cargando". Si tarda más (red), no bloqueamos.
        if (WaitForSingleObject(j->done, 30) == WAIT_OBJECT_0 && j->result && !j->taken.exchange(true)) {
            if (j->result->error == 0) g_cache[Key(path)] = j->result;
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
        UpdateWindow(g_hwnd);  // dibujar ya, sin esperar a la cola de mensajes
    }
    TouchSession();
}

void ApplyJob(const std::shared_ptr<Job>& j) {
    if (j->taken.exchange(true) || !j->result) return;
    auto l = j->result;
    const std::wstring k = Key(l->path);
    if (l->error == 0) g_cache[k] = l;
    if (j->kind == JobKind::Prefetch) {
        if (g_preview) Invalidate();  // la vista previa de una carpeta puede estar esperándolo
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

// Pestaña de un panel cercana a la posición i (la de la derecha, o si no la de la izquierda).
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

// Pestañas cerradas, para reabrirlas donde estaban y con su historial.
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
        // se cerró la última pestaña de un panel: se vuelve a un solo panel
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
    // si la carpeta ya no existe se sigue con la anterior
    while (!g_closedTabs.empty()) {
        ClosedTab c = std::move(g_closedTabs.back());
        g_closedTabs.pop_back();
        if (!c.path.empty() && !FolderExists(c.path)) continue;
        NewTab(c.path, true, c.pane);
        T().back = std::move(c.back);
        T().fwd = std::move(c.fwd);
        // NewTab la deja al final: vuelve a su lugar
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
    S.message = L"No hay pestañas cerradas";
    Invalidate();
}

// Pestañas del panel activo, en orden.
std::vector<int> ActiveTabs() {
    std::vector<int> idx;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        if (!g_split || g_tabs[i]->pane == g_act) idx.push_back(i);
    return idx;
}

// Pasa a la siguiente (d = 1) o a la anterior (d = -1) del panel activo, dando la vuelta.
void CycleTab(int d) {
    const std::vector<int> idx = ActiveTabs();
    const int n = static_cast<int>(idx.size());
    const int pos = static_cast<int>(std::find(idx.begin(), idx.end(), g_cur) - idx.begin());
    if (n > 1) SelectTab(idx[(pos + d + n) % n]);
}

// Cuánto le falta a una pestaña para llegar a su lugar, en lugares de pestaña. Frena al final.
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

// Mueve la pestaña activa un lugar dentro de su panel (d = 1, a la derecha); en los extremos se queda.
// El cambio es inmediato: la animación solo dibuja las dos pestañas llegando a su lugar.
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
        if (inNewTab)
            NewTab(full);
        else
            Navigate(t, full, true);
        return;
    }
    AddRecent(full);
    std::thread([full] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, nullptr, full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }).detach();
}

// Mueve el cursor. extend = Shift (rango desde el ancla); toggle = Ctrl (alterna ese elemento).
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

// --- árbol: desplegar carpetas dentro de la lista
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

// Plegar: la carpeta del cursor si está abierta, o la carpeta que contiene al cursor (y va a ella).
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

// Filtro difuso: escribir en la lista filtra y ordena por coincidencia (como fzf).
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

// Los globales de orden reflejan la vista general (los usa el hilo que lista, para venir casi ordenado).
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
    ++g_viewEpoch;  // todas las vistas se vuelven a aplicar
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.list) continue;
        Place p = SavePlace(t);  // antes de reemplazar los listados: las filas apuntan a ellos
        for (auto& [k, kl] : t.kids)
            if (kl) kl = Prepared(g_cache.count(k) ? g_cache[k] : kl);
        if (g_cache.count(Key(t.path))) t.list = Prepared(g_cache[Key(t.path)]);
        Rebuild(t, &p, L"");
    }
    EnsureVisible(T());
    Invalidate();
    TouchSession();
}

// Cambió una vista: se guarda y se vuelve a mostrar todo.
void RefreshConfigSig();
void ViewsChanged() {
    SaveViews();
    RefreshConfigSig();  // lo escribimos nosotros: no hace falta recargar
    SyncGlobalSort();
    ResortAll();
}

// Vista de la carpeta activa (la propia, la heredada o la general).
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
// Agrupa la carpeta actual (gb < 0: sin grupos). Agrupar es cosa de cada carpeta: si la carpeta usa
// la vista general, se le crea una propia a partir de ella para no agrupar todas.
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
void SetSort(SortBy by) {
    SortBy cur;
    bool asc;
    if (ViewPrimarySort(CurView(), cur, asc) && cur == by)
        SetSortExplicit(by, !asc);
    else
        SetSortExplicit(by, by == SortBy::Name);  // fecha y tamaño: lo más grande/nuevo primero
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
        S.message = L"Quitado de favoritos";
    } else {
        g_favs.push_back({TabTitle(t), t.path});
        S.message = L"Agregado a favoritos";
    }
    SaveFavs();
    Invalidate();
}

// ---------------------------------------------------------------- iconos
// Los iconos "por extensión" se resuelven en el momento (no tocan el disco).
// Los que dependen del archivo (carpetas, .exe, accesos directos, unidades)
// se piden a otro hilo y se dibuja un icono genérico hasta que lleguen.
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
    std::wstring probe = ext.empty() ? L"archivo" : L"archivo" + ext;
    int idx = IconByAttrs(probe.c_str(), FILE_ATTRIBUTE_NORMAL);
    g_iconByExt[ext] = idx;
    return idx;
}

void RequestIcon(const std::wstring& path) {
    std::lock_guard lk(g_im);
    std::wstring k = Key(path);
    if (g_iconByPath.count(k) || g_iconPending.count(k)) return;
    if (g_iconQueue.size() > 400) {  // lo que quedó atrás ya no está a la vista
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
            // "Este equipo": el icono real de la carpeta virtual de unidades
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

// ---------------------------------------------------------------- dibujo
ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dw;
ComPtr<IWICImagingFactory> g_wic;
ComPtr<ID2D1RenderTarget> g_rt;           // donde se dibuja (la ventana, o un bitmap en el modo de prueba)
ComPtr<ID2D1HwndRenderTarget> g_hwndRt;   // el de la ventana
ComPtr<IWICBitmap> g_testBmp;             // el bitmap del modo de prueba
ComPtr<ID2D1SolidColorBrush> g_brush, g_brushAccent;
ComPtr<IDWriteRenderingParams> g_textParams;
// definidas en palette.inl
void TextHighlighted(const std::wstring& s, const std::wstring& patLower, IDWriteTextFormat* fmt, float l, float t,
                     float r, float b, UINT32 color, UINT32 hiColor, float alpha = 1.f);
void DrawPalette(float W, float H);  // rasterizado "GDI clásico": igual que el EDIT y que File Pilot

void CreateTextParams() {
    ComPtr<IDWriteRenderingParams> def;
    if (FAILED(g_dw->CreateRenderingParams(&def))) return;
    g_dw->CreateCustomRenderingParams(def->GetGamma(), def->GetEnhancedContrast(), def->GetClearTypeLevel(),
                                      def->GetPixelGeometry(), DWRITE_RENDERING_MODE_GDI_CLASSIC, &g_textParams);
}
ComPtr<IDWriteTextFormat> g_fmtName, g_fmtNameBold, g_fmtRight, g_fmtIcon, g_fmtIconSm, g_fmtCap, g_fmtCrumb, g_fmtTab, g_fmtSide,
    g_fmtSection, g_fmtHeader, g_fmtStatus, g_fmtStatusR, g_fmtCenter, g_fmtMono, g_fmtMonoR;

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
                           L"es-ar", &f);
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

// Armar un texto es lo más caro de cada cuadro, y casi todos se repiten: se guardan ya armados.
std::unordered_map<uint64_t, ComPtr<IDWriteTextLayout>> g_textCache;

void CreateFormats() {
    g_textCache.clear();
    // Por defecto, la misma tipografía que File Pilot: Segoe UI clásica a 9 pt (12 DIP).
    const wchar_t* ui = FontExists(g_cfg.font.c_str()) ? g_cfg.font.c_str() : L"Segoe UI";
    const wchar_t* icons = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const float sz = g_cfg.fontSize, smallSz = std::max(8.f, sz - 2);  // ("small" es una macro de Windows)
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
    // vista previa de texto: monoespaciada, sin recortar con "…"
    const wchar_t* mono = FontExists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    g_fmtMono = MakeFormat(mono, 12.f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_NORMAL, false);
    g_fmtMonoR = MakeFormat(mono, 12.f, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_NORMAL, false);
}

void PreviewReleaseBitmap();  // preview.inl
void ReleaseTarget() {
    PreviewReleaseBitmap();
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
        // sin ventana visible: se dibuja en un bitmap (que "captura" guarda como PNG)
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
    int want = g_dpi >= 144 || g_dpi > g_sysDpi ? SHIL_LARGE : SHIL_SMALL;  // con zoom: achicar, no agrandar
    if (g_imgList && g_imgListSize == want) return;
    g_imgList.Reset();
    SHGetImageList(want, IID_PPV_ARGS(&g_imgList));
    g_imgListSize = want;
    g_iconBmp.clear();
}

ID2D1Bitmap* IconBitmap(int idx) {
    if (idx < 0 || !g_imgList) return nullptr;
    auto it = g_iconBmp.find(idx);
    if (it != g_iconBmp.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap> bmp;
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
    g_iconBmp[idx] = bmp;
    return bmp.Get();
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
    // con zoom un DIP no es un píxel entero: se alinea para que las líneas finas sigan nítidas
    const float s = g_dpi / 96.f;
    auto snap = [s](float v) { return std::round(v * s) / s; };
    D2D1_RECT_F q{snap(r.left), snap(r.top), snap(r.right), snap(r.bottom)};
    if (r.right > r.left && r.right - r.left <= 1.01f) q.right = q.left + 1 / s;  // una línea: siempre 1 píxel
    if (r.bottom > r.top && r.bottom - r.top <= 1.01f) q.bottom = q.top + 1 / s;
    g_rt->FillRectangle(q, g_brush.Get());
}
void Round(const D2D1_RECT_F& r, float rad, UINT32 c, float a = 1.f) {
    Color(c, a);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_brush.Get());
}
void HLine(float x1, float x2, float y, UINT32 c = cBorder) { Fill(D2D1::RectF(x1, y, x2, y + 1), c); }

bool IsHover(HitKind k, int idx) { return g_hover.kind == k && g_hover.idx == idx; }

// Dibuja el icono real del elemento, o uno genérico mientras se resuelve.
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
    for (wchar_t* p = buf; *p; ++p)
        if (*p == L'.') *p = L',';
}

std::wstring Thousands(int n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, L".");
    return s;
}

std::wstring Ms(double v) {
    wchar_t b[32];
    swprintf(b, 32, v < 10 ? L"%.2f ms" : L"%.0f ms", v);
    for (wchar_t* p = b; *p; ++p)
        if (*p == L'.') *p = L',';
    return b;
}

std::wstring ErrorText(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"error desconocido";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

// Editor de vistas (viewedit.inl): lo que el dibujo de la lista necesita saber.
struct ViewEditorState {
    bool open = false;
    int pane = 0;
} g_ve;

// --- pestañas (también es la barra de título: se arrastra la ventana desde acá)
// Con la vista dividida, cada panel tiene su grupo de pestañas encima.
// El fondo de la pestaña visible viaja hasta la nueva. Solo se dibuja: el cambio ya ocurrió.
struct TabPill {
    const Tab* tab = nullptr;
    float x = 0, from = 0;  // dónde quedó dibujado; distancia a recorrer al empezar
    double at = 0;
} g_pill[2];
constexpr double kTabPillMs = 120;

void DrawTabGroup(int pane, float x0, float xEnd, bool activeWnd) {
    std::vector<int> idx;
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i)
        if (!g_split || g_tabs[i]->pane == pane) idx.push_back(i);
    const int n = static_cast<int>(idx.size());
    if (!n) return;
    const float avail = xEnd - x0 - 30;  // lugar para el "+"
    float tw = std::min(kTabMaxW, std::max(36.f, avail / n));
    if (avail / n >= kTabMinW) tw = std::clamp(avail / n, kTabMinW, kTabMaxW);
    const float top = 7, bot = kTabH;
    const int visible = g_split ? g_paneCur[pane] : g_cur;
    // la pestaña visible va última, para que al moverse pase por encima de la otra
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
            // por debajo de los títulos, para no taparlos al pasar
            SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
            Round(D2D1::RectF(pill.x, top, pill.x + tw - 4, bot + 8), 7, cTop);
        }
    }
    for (int k : order) {
        const int i = idx[k];
        const float x = x0 + k * tw;
        const float off = TabSlide(*g_tabs[i]) * tw;
        if (off != 0) SetTimer(g_hwnd, kTimerTabSlide, 15, nullptr);
        const D2D1_RECT_F home = D2D1::RectF(x, top, x + tw - 4, bot);  // los clicks van a su lugar final
        D2D1_RECT_F r = home;
        r.left += off;
        r.right += off;
        bool cur = i == visible;
        bool activePane = !g_split || pane == g_act;
        bool hov = IsHover(HitKind::Tab, i) || IsHover(HitKind::TabClose, i);
        if (cur) {
            // pestaña visible: se funde con la barra de abajo
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

    // Zona izquierda de ancho fijo, siempre libre para arrastrar, con el botón de la
    // barra lateral al final. No se mueve cuando la barra se oculta.
    D2D1_RECT_F tg = D2D1::RectF(kTitleLeft - 36, 7, kTitleLeft - 8, kTabH - 7);
    if (IsHover(HitKind::SideToggle, 0)) Round(tg, 5, 0xFFFFFF, 0.08f);
    Color(active ? cDim : cFaint);
    Glyph(g_sideVisible ? gPaneClose : gPaneOpen, g_fmtIcon.Get(), tg.left, tg.top, tg.right, tg.bottom);
    g_hits.push_back({tg, HitKind::SideToggle, 0});

    // menú general de la app
    D2D1_RECT_F mb = D2D1::RectF(10, 7, 38, kTabH - 7);
    if (IsHover(HitKind::AppMenu, 0)) Round(mb, 5, 0xFFFFFF, 0.08f);
    Color(active ? cDim : cFaint);
    Glyph(0xE700, g_fmtIcon.Get(), mb.left, mb.top, mb.right, mb.bottom);
    g_hits.push_back({mb, HitKind::AppMenu, 0});

    // botones de ventana, a la derecha
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

    const float x0 = kTitleLeft + 2, xEnd = W - 3 * kCapBtnW - 14;
    if (!g_split) {
        DrawTabGroup(0, x0, xEnd, active);
    } else {
        const float mid = PaneGeo(1).TL;
        DrawTabGroup(0, x0, mid - 6, active);
        Fill(D2D1::RectF(mid, 12, mid + 1, kTabH - 8), cBorder);
        DrawTabGroup(1, mid + 6, xEnd, active);
    }
}

// --- barra de navegación (una por panel)
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

    // ruta como "migas de pan"
    const float crumbL = x, crumbR = g.TR - kPad;
    D2D1_RECT_F bar = D2D1::RectF(crumbL, cy - 15, crumbR, cy + 15);
    Round(bar, 7, cBg);
    if (g_editing && activePane) return;
    struct Crumb {
        std::wstring label, path;
        float w;
    };
    std::vector<Crumb> crumbs;
    crumbs.push_back({L"", L"", 30});  // icono de "Este equipo"
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
    // si no entra, se ocultan los del principio detrás de "…"
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
    g_rt->PushAxisAlignedClip(bar, D2D1_ANTIALIAS_MODE_ALIASED);
    if (first > 0) {
        D2D1_RECT_F r = D2D1::RectF(cx, cy - 12, cx + 24, cy + 12);
        if (IsHover(HitKind::Crumb, static_cast<int>(g_crumbPaths.size()))) Round(r, 5, 0xFFFFFF, 0.08f);
        Color(cDim);
        Text(L"…", g_fmtCrumb.Get(), r.left, r.top, r.right, r.bottom);
        g_hits.push_back({r, HitKind::Crumb, static_cast<int>(g_crumbPaths.size())});
        g_crumbPaths.push_back(crumbs[first - 1].path);
        cx += 24;
        Color(cFaint);
        Glyph(gChevron, g_fmtIconSm.Get(), cx, cy - 8, cx + chevW, cy + 8);
        cx += chevW;
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
        if (!last) {
            Color(cFaint);
            Glyph(gChevron, g_fmtIconSm.Get(), cx, cy - 8, cx + chevW, cy + 8);
            cx += chevW;
        }
    }
    g_rt->PopAxisAlignedClip();
    if (cx < bar.right) g_hits.push_back({D2D1::RectF(cx, bar.top, bar.right, bar.bottom), HitKind::PathEmpty, pane});
}

// --- barra lateral
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
    // name, ruta, es unidad, uso del disco (0..1, <0 = sin barra)
    auto item = [&](const std::wstring& name, const std::wstring& path, bool drive, float usage) {
        const float rowH = usage >= 0 ? kSideRow + 12 : kSideRow;
        D2D1_RECT_F r = D2D1::RectF(6, y, kSideW - 7, y + rowH);
        int idx = static_cast<int>(g_sidePaths.size());
        bool active = Key(path) == cur;
        if (active)
            Round(r, 5, cAccent, 0.22f);
        else if (IsHover(HitKind::Side, idx))
            Round(r, 5, 0xFFFFFF, 0.05f);
        if (idx == g_dropSide) {
            Color(cAccent, 0.9f);
            g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 5, 5), g_brush.Get(), 1.5f);
        }
        DrawIcon(path, nullptr, nullptr, 0, drive, r.left + 10, y + 5, 1.f);
        Color(active ? cText : 0xC9C9D1);
        Text(name, g_fmtSide.Get(), r.left + 34, y, r.right - 6, y + kSideRow);
        if (usage >= 0) {
            // barra de uso, como File Pilot: azul, roja cuando queda poco lugar.
            // Mismo margen a ambos lados respecto del borde de la barra lateral.
            const float margin = kSideW - 1 - (r.right - 10);
            const float bx = margin, bw = r.right - 10 - bx, by = y + kSideRow - 1, bh = 6;
            Round(D2D1::RectF(bx, by, bx + bw, by + bh), bh / 2, 0xFFFFFF, 0.10f);
            Round(D2D1::RectF(bx, by, bx + std::max(bh, bw * usage), by + bh), bh / 2, usage >= 0.9f ? cDiskFull : cDisk);
        }
        g_hits.push_back({r, HitKind::Side, idx});
        g_sidePaths.push_back(path);
        y += rowH;
    };
    section(L"FAVORITOS");
    for (const Favorite& v : g_favs) item(v.name, v.path, false, -1);
    y += 12;
    section(L"UNIDADES");
    item(L"Este equipo", L"", false, -1);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = static_cast<wchar_t>(L'A' + i);
        std::wstring root = std::wstring(1, letter) + L":\\";
        item(DriveLabel(letter), root, true, DriveUsage(letter));
    }
}

// --- encabezado de columnas y lista
int g_colDrag = 0, g_colDragPane = 0;  // columna que se está arrastrando (1 = fecha, 2 = tamaño)
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

// Sangría del árbol para una fila (0 con filtro o búsqueda: ahí la lista es plana).
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
    // botón de vista (filtro): resaltado si la carpeta tiene vista propia (o heredada)
    {
        const bool own = &view != &g_viewAll;
        D2D1_RECT_F vb = D2D1::RectF(c.nameL - 4, y0 + 4, c.nameL + 20, y1 - 5);
        if (IsHover(HitKind::ViewBtn, pane) || (g_ve.open && g_ve.pane == pane)) Round(vb, 5, 0xFFFFFF, 0.08f);
        Color(own ? cAccent : IsHover(HitKind::ViewBtn, pane) ? cDim : cFaint);
        Glyph(0xE71C, g_fmtIconSm.Get(), vb.left, vb.top, vb.right, vb.bottom);
        g_hits.push_back({vb, HitKind::ViewBtn, pane});
        // plegar / desplegar todos los grupos (solo si la lista tiene grupos)
        const Tab& pt = PT(pane);
        if (pt.list && !pt.list->groups.empty() && pt.filter.empty() && !pt.recursive) {
            bool allFolded = true;
            for (size_t gi = 0; gi < pt.list->groups.size(); ++gi) allFolded &= GroupFolded(pt, pt.list.get(), gi);
            D2D1_RECT_F fb = D2D1::RectF(c.nameL + 18, y0 + 4, c.nameL + 40, y1 - 5);
            if (IsHover(HitKind::FoldBtn, pane)) Round(fb, 5, 0xFFFFFF, 0.08f);
            Color(IsHover(HitKind::FoldBtn, pane) ? cText : cDim);
            // dos chevrones: hacia adentro = plegar todo, hacia afuera = desplegar todo
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
    } cols[] = {{L"Nombre", SortBy::Name, c.nameL, c.nameR, false, true},
                {L"Modificado", SortBy::Date, c.dateL, c.dateL + kDateW, true, c.showDate},
                {L"Tamaño", SortBy::Size, c.sizeL, c.right, true, c.showSize}};
    int i = 0;
    for (const Col& col : cols) {
        if (col.show) {
            // "Nombre" empieza después del botón de vista
            D2D1_RECT_F r = D2D1::RectF(col.right ? col.l - 6 : col.l + kChev + 26, y0 + 3, col.r + 6, y1 - 4);
            bool active = hasSort && by == col.by;
            int hid = pane * 3 + i;
            if (IsHover(HitKind::Header, hid)) Round(r, 4, 0xFFFFFF, 0.05f);
            Color(active ? cText : cDim);
            float tl = col.l + (col.right ? 0 : kChev + 28), tr = col.r;
            if (active) {
                wchar_t g = sortAsc ? gSortUp : gSortDown;
                float w = MeasureW(col.label, g_fmtHeader.Get());
                // pegada al título, aunque la columna sea ancha
                if (col.right) Glyph(g, g_fmtIconSm.Get(), tr - w - 18, y0, tr - w - 4, y1);
                else Glyph(g, g_fmtIconSm.Get(), tl + w + 4, y0, tl + w + 18, y1);
            }
            Text(col.label, wcslen(col.label), col.right ? g_fmtRight.Get() : g_fmtHeader.Get(), tl, y0, tr, y1);
            g_hits.push_back({r, HitKind::Header, hid});
        }
        ++i;
    }
    // bordes para cambiar el ancho: la línea solo aparece con el mouse encima
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

void DrawList(int pane, float H) {
    const float listTop = ListTop(), listBot = H - kStatus;
    Tab& t = PT(pane);
    Columns c = Cols(pane);
    const float L = c.L, R = c.R;
    g_rt->PushAxisAlignedClip(D2D1::RectF(L, listTop, R, listBot), D2D1_ANTIALIAS_MODE_ALIASED);
    const int n = RowCount(t);
    if (t.loading) {
        Color(cDim);
        Text(L"Cargando…", g_fmtCenter.Get(), L, listTop, R, listTop + 80);
    } else if (t.list && t.list->error) {
        Color(cError);
        Text(L"No se pudo abrir: " + ErrorText(t.list->error), g_fmtCenter.Get(), L + kPad, listTop, R - kPad,
             listTop + 80);
    } else if (t.list && n == 0) {
        Color(cDim);
        Text(t.recursive && !t.recDone ? L"Buscando…" : !t.filter.empty() ? L"Nada coincide" : L"Carpeta vacía",
             g_fmtCenter.Get(), L, listTop, R, listTop + 80);
    } else if (t.list) {
        const bool focused = (g_test ? g_testActive && !g_renaming && !g_editing : GetFocus() == g_hwnd) && pane == g_act;
        const bool multi = SelCount(t) > 1;
        const bool flat = FlatView(t);
        const int hoverHeader = g_hoverHeader;
        const std::wstring filterPat = Lower(t.filter);
        wchar_t buf[64];
        for (int i = std::max(0, static_cast<int>(t.scroll / kRow)); i < n; ++i) {
            const float y = listTop + i * kRow - t.scroll;
            if (y >= listBot) break;
            const Row& row = t.rows[i];
            if (row.e < 0) {
                // título de grupo: texto chico y una línea fina hasta el borde
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
            const D2D1_RECT_F rr = D2D1::RectF(L + 6, y + 1, R - 8, y + kRow - 1);
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
            const float indent = flat ? 0.f : row.depth * kIndent;
            const float chevX = c.nameL + indent;
            // guías del árbol: una línea tenue por nivel
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
            // sin extensión, si así se pidió (las carpetas se muestran enteras)
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
                TextHighlighted(std::wstring(name, shownLen), filterPat, g_fmtName.Get(), textX, y, c.nameR, y + kRow, cText, cAccent, a);
            } else {
                Color(cText, a);
                Text(name, shownLen, g_fmtName.Get(), textX, y, c.nameR, y + kRow);
            }
            // en la lista plana (filtro o búsqueda), dónde está cada cosa
            if (flat && row.depth > 0 && row.l->path.size() > t.path.size()) {
                std::wstring rel = row.l->path.substr(t.path.size());
                while (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
                float w = MeasureW(std::wstring(name, shownLen), g_fmtName.Get());
                if (textX + w + 14 < nameEnd - 30) {
                    Color(selected && focused ? 0x9FB3D9 : cFaint, a);
                    Text(rel, g_fmtName.Get(), textX + w + 14, y, nameEnd, y + kRow);
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
        // barra de desplazamiento finita, solo si hace falta
        const float contentH = n * kRow, viewH = listBot - listTop;
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
        // lo que está dentro de los grupos plegados también cuenta
        if (!FlatView(t) && !t.list->groups.empty())
            for (size_t gi = 0; gi < t.list->groups.size(); ++gi)
                if (GroupFolded(t, t.list.get(), gi)) {
                    size_t end = gi + 1 < t.list->groups.size() ? t.list->groups[gi + 1].first : t.list->entries.size();
                    items += static_cast<int>(end - t.list->groups[gi].first);
                }
        left = Thousands(items) + (items == 1 ? L" elemento" : L" elementos");
        int s = SelCount(t);
        if (s > 1) left += L"   ·   " + Thousands(s) + L" seleccionados";
    }
    float lx = g.TL + kPad + 2;
    if (!t.filter.empty() || t.recursive) {
        // cuadro del filtro, al estilo File Pilot: lupa + texto + cantidad
        std::wstring label = t.recursive ? L"en subcarpetas:" : L"";
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
            left = (t.filter.empty() ? Thousands(n) + L" elementos" : Thousands(n) + L" coincidencias") + L" en " +
                   Thousands(t.recDirs) + (t.recDirs == 1 ? L" carpeta" : L" carpetas") +
                   (t.recDone ? L"   ·   Esc vuelve" : L"   ·   buscando…");
        } else {
            left = Thousands(n) + (n == 1 ? L" coincidencia" : L" coincidencias") + L"   ·   Esc limpia";
        }
        if (!S.message.empty() && activePane) left = S.message;
    }
    if (g_dsBusy && activePane && S.message.empty()) left += L"   ·   calculando tamaños…";
    if (S.message.empty() || !activePane) {
        if (g_hideDirs) left += L"   ·   sin carpetas";
        if (g_hideFiles) left += L"   ·   sin archivos";
    }
    // tiempos a la derecha, solo si entran sin pisar el texto
    std::wstring right;
    float rw = 0;
    if (activePane && S.show && S.navMs >= 0) {
        right = L"abrir " + Ms(S.navMs) + L"   ·   ";
        right += S.fromCache ? L"desde memoria" : L"disco " + Ms(S.enumMs) + L" + orden " + Ms(S.sortMs);
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

// Milisegundos desde que arrancó el proceso (incluye la carga del ejecutable).
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
    if (g_iconFolder < 0) g_iconFolder = IconByAttrs(L"carpeta", FILE_ATTRIBUTE_DIRECTORY);
    if (g_iconFile < 0) g_iconFile = IconByAttrs(L"archivo", FILE_ATTRIBUTE_NORMAL);
    g_hits.clear();
    g_crumbPaths.clear();
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
        // divisor entre paneles: se arrastra para cambiar el ancho (doble click: mitad y mitad)
        const float x = PaneGeo(1).L;
        Fill(D2D1::RectF(x, kTabH, x + 1, H), cBorder);
        g_hits.push_back({D2D1::RectF(x - 3, kTabH + kToolH, x + 4, H - kStatus), HitKind::Divider, 0});
    }
    DrawPreview(W, H);
    DrawViewEditor(W, H);
    DrawPalette(W, H);
    DrawInstallPanel(W, H);
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseTarget();
    if (g_firstFrameMs == 0) g_firstFrameMs = MsSinceProcessStart();

    // Tiempo desde la tecla/click hasta el cuadro terminado. Se muestra en el siguiente cuadro.
    if (S.navPending && !T().loading) {
        S.navMs = NowMs() - S.navStart;
        S.navPending = false;
        Invalidate();
    }
}

// ---------------------------------------------------------------- barra de ruta editable
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
    // como en renombrar: el EDIT dibuja el texto pegado arriba, asÃ­ que se le da la altura justa del
    // texto y se lo centra en la barra (igual que las migas, que DirectWrite centra verticalmente)
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_editFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    int h = tm.tmHeight + 2;
    int y = MulDiv(static_cast<int>(std::lround((kTabH + kToolH / 2) * 100)), g_dpi, 9600) - h / 2 + px(1);  // +1: DirectWrite deja el interlineado arriba
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
    if (t.empty() || _wcsicmp(t.c_str(), L"Este equipo") == 0) {
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
        S.message = L"No existe: " + p;
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
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // sin pitido
    if (m == WM_KILLFOCUS) HidePathEdit();
    return DefSubclassProc(h, m, w, l);
}

// ---------------------------------------------------------------- renombrar en la lista (F2)
std::wstring g_renameOld;  // nombre original
std::wstring g_renameDir;  // carpeta del elemento (en el árbol puede no ser la de la pestaña)

void LayoutRename() {
    if (!g_renaming) return;
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return;
    Columns c = Cols(g_act);
    float y = ListTop() + t.sel * kRow - t.scroll;
    auto px = [](float dip) { return MulDiv(static_cast<int>(std::lround(dip)), g_dpi, 96); };
    // El EDIT dibuja el texto pegado arriba: se le da la altura justa del texto y se centra en la fila.
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_editFont);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    int editH = tm.tmHeight + 2;
    int rowTop = px(y), rowH = px(kRow);
    int top = rowTop + (rowH - editH) / 2;
    int left = px(RowTextX(c, t, t.sel) - 2), right = px(c.nameR);
    bool visible = y + kRow > ListTop() && y < ListBottom();
    SetWindowPos(g_renameEdit, nullptr, left, top, std::max(40, right - left), editH,
                 SWP_NOZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void BeginRename() {
    Tab& t = T();
    if (g_renaming || !t.list || t.list->error || t.sel < 0 || t.sel >= Count()) return;
    if (RowDir(t, t.sel).empty()) return;  // unidades
    g_renameOld = SelectedName(t);
    g_renameDir = RowDir(t, t.sel);
    g_renaming = true;
    SetWindowTextW(g_renameEdit, g_renameOld.c_str());
    LayoutRename();
    SetFocus(g_renameEdit);
    // como el Explorador: queda seleccionado el nombre sin la extensión
    int selEnd = static_cast<int>(g_renameOld.size());
    if (!EntryAt(t, t.sel).isDir()) {
        size_t dot = g_renameOld.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) selEnd = static_cast<int>(dot);
    }
    SendMessageW(g_renameEdit, EM_SETSEL, 0, selEnd);
    Invalidate();
}

void CancelRename() {
    if (!g_renaming) return;
    g_renaming = false;
    ShowWindow(g_renameEdit, SW_HIDE);
    if (GetFocus() == g_renameEdit) SetFocus(g_hwnd);
    Invalidate();
}

// follow: el cursor sigue al nombre nuevo cuando se recarga la lista.
void CommitRename(bool follow = true) {
    if (!g_renaming) return;
    wchar_t buf[1024];
    GetWindowTextW(g_renameEdit, buf, 1024);
    std::wstring nuevo = buf;
    while (!nuevo.empty() && (nuevo.back() == L' ' || nuevo.back() == L'.')) nuevo.pop_back();
    std::wstring viejo = g_renameOld, dir = g_renameDir;
    Tab& t = T();
    CancelRename();
    if (nuevo.empty() || nuevo == viejo) return;
    if (nuevo.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        S.message = L"El nombre no puede contener \\ / : * ? \" < > |";
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    std::wstring from = ToExtendedPath(Join(dir, viejo.c_str()));
    std::wstring to = ToExtendedPath(Join(dir, nuevo.c_str()));
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        S.message = L"No se pudo renombrar: " + ErrorText(GetLastError());
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    UndoRecordRename(Join(dir, viejo.c_str()), Join(dir, nuevo.c_str()));
    if (follow) t.pendingSelect = Key(dir) == Key(t.path) ? nuevo : Join(dir, nuevo.c_str());
    TreeFollow(t, Join(dir, viejo.c_str()), Join(dir, nuevo.c_str()));
    Enqueue(dir, JobKind::Revalidate);
}

// Tab / Shift+Tab mientras se renombra: guarda y pasa a renombrar el siguiente / el anterior.
void RenameStep(int delta) {
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

// Nombre libre en una carpeta: "base ext", "base (2) ext", ...
std::wstring FreeName(const std::wstring& dir, const std::wstring& base, const std::wstring& ext) {
    std::wstring name = base + ext;
    for (int i = 2; GetFileAttributesW(ToExtendedPath(Join(dir, name.c_str())).c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        name = base + L" (" + std::to_wstring(i) + L")" + ext;
    return name;
}

// Carpeta o archivo nuevo con nombre único, y renombrado inmediato.
void NewItem(bool folder) {
    Tab& t = T();
    if (t.path.empty() || !t.list || t.list->error) return;
    std::wstring name = folder ? FreeName(t.path, L"Nueva carpeta", L"") : FreeName(t.path, L"Nuevo archivo", L".txt");
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
        S.message = std::wstring(folder ? L"No se pudo crear la carpeta: " : L"No se pudo crear el archivo: ") +
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
void RunCommand(Cmd cmd);
void SetFoldersFirst(int mode);
void ViewEditorOpen(int pane);
#include "indexer.inl"
#include "palette.inl"
#include "viewedit.inl"
#include "ops.inl"
#include "search.inl"
#include "preview.inl"
void PreviewReleaseBitmap() { g_pv.d2d.Reset(); }
#include "background.inl"
#include "install.inl"
#include "testmode.inl"

// ---------------------------------------------------------------- sesión (pestañas, ventana, paneles)
std::wstring SessionFile() {
    std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\') + 1) + L"sesion.txt";
}
std::wstring RecentsFile() {
    std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\') + 1) + L"recientes.txt";
}

void SaveSession() {
    std::wstring f = SessionFile();
    if (f.empty() || g_tabs.empty()) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(g_hwnd, &wp);
    std::ofstream out(f, std::ios::binary);
    out << "# Sesion de Jolt. Se reescribe sola; se puede borrar sin problema.\n";
    const RECT& r = wp.rcNormalPosition;
    out << "ventana=" << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << ','
        << (IsZoomed(g_hwnd) || g_showMaxOnce ? 1 : 0) << '\n';
    out << "sidebar=" << (g_sideVisible ? 1 : 0) << '\n';
    out << "previa=" << (g_preview ? 1 : 0) << '\n';
    out << "ver=" << (g_hideDirs ? 1 : 0) << ',' << (g_hideFiles ? 1 : 0) << ',' << (g_hideExt ? 1 : 0) << '\n';
    out << "orden=" << g_sortBy.load() << ',' << (g_sortAsc.load() ? 1 : 0) << '\n';
    out << "dividida=" << (g_split ? 1 : 0) << ',' << static_cast<int>(g_splitRatio * 1000) << '\n';
    g_paneCur[g_act] = g_cur;
    out << "activa=" << g_cur << '\n';
    out << "visible=" << g_paneCur[0] << ',' << g_paneCur[1] << '\n';
    for (auto& t : g_tabs) out << (t->pane ? "tab2=" : "tab=") << Utf8(t->path) << '\n';
    std::wstring rf = RecentsFile();
    if (!rf.empty()) {
        std::ofstream ro(rf, std::ios::binary);
        for (const std::wstring& p : g_recents) ro << Utf8(p) << '\n';
    }
}

struct Session {
    std::vector<std::pair<std::wstring, int>> tabs;  // ruta, panel
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
        else if (k == "activa") s.active = atoi(v.c_str());
        else if (k == "visible") sscanf_s(v.c_str(), "%d,%d", &s.visible[0], &s.visible[1]);
        else if (k == "sidebar") s.sidebar = v != "0";
        else if (k == "previa") s.preview = v != "0";
        else if (k == "ver") {
            int a = 0, b = 0, c = 0;
            sscanf_s(v.c_str(), "%d,%d,%d", &a, &b, &c);
            g_hideDirs = a != 0;
            g_hideFiles = b != 0 && !g_hideDirs;
            g_hideExt = c != 0;
        }
        else if (k == "dividida") {
            int on = 0, ratio = 500;
            sscanf_s(v.c_str(), "%d,%d", &on, &ratio);
            s.split = on != 0;
            s.ratio = std::clamp(ratio / 1000.f, 0.15f, 0.85f);
        } else if (k == "orden") {
            s.sortBy = atoi(v.c_str());
            size_t c = v.find(',');
            s.sortAsc = c == std::string::npos || v.substr(c + 1) != "0";
        } else if (k == "ventana") {
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
    return s;
}

constexpr UINT_PTR kTimerSession = 3;
void TouchSession() { SetTimer(g_hwnd, kTimerSession, 2000, nullptr); }  // guarda un rato después del último cambio

// Recarga todo lo que vive en %APPDATA%\Jolt (config, favoritos, menú) tras un cambio en disco.
constexpr UINT_PTR kTimerConfig = 4;
// Firma (fecha y tamaño) de los archivos que el usuario edita; sesion.txt y recientes.txt no cuentan.
std::wstring ConfigSignature() {
    std::wstring sig;
    for (const wchar_t* n : {L"config.txt", L"favoritos.txt", L"menu.txt", L"vistas.txt"}) {
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
    if (sig == g_configSig) return;  // cambió otra cosa de la carpeta (la sesión, los recientes)
    g_configSig = sig;
    const UINT oldDpi = g_dpi;
    LoadConfig();
    ApplyDpi();
    if (g_dpi != oldDpi) ReleaseTarget();
    LoadFavs();
    LoadMenuCfg();
    LoadViews();
    SyncGlobalSort();
    CreateFormats();
    UpdateEditFont();
    ResortAll();
    for (auto& tp : g_tabs) ClampScroll(*tp);
    BgApplyConfig();
    LayoutEdit();
    LayoutRename();
    S.message = L"Configuración recargada";
    Invalidate();
}

// Zoom de toda la interfaz; se guarda en config.txt.
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
        ConfigSetValue(L"general", L"escala", std::to_wstring(pct));
        RefreshConfigSig();
    }
    S.message = L"Interfaz al " + std::to_wstring(pct) + L" %";
    Invalidate();
}

// Espacio entre filas; se guarda en config.txt.
void SetRowHeight(float h) {
    h = std::clamp(h, 18.f, 60.f);
    if (h != kRow) {
        const float ratio = h / kRow;
        kRow = h;
        for (auto& tp : g_tabs) {
            tp->scroll *= ratio;  // que se siga viendo lo mismo
            ClampScroll(*tp);
        }
        EnsureVisible(T());
        LayoutRename();
        ConfigSetValue(L"general", L"fila", std::to_wstring(static_cast<int>(h)));
        RefreshConfigSig();
    }
    S.message = L"Filas de " + std::to_wstring(static_cast<int>(kRow)) + L" px";
    Invalidate();
}

// Pliega o despliega todos los grupos, solo por esta visita: la vista no cambia.
void FoldAllGroups(Tab& t, bool folded) {
    if (!t.list || t.list->groups.empty()) {
        S.message = L"Esta carpeta no tiene grupos";
        Invalidate();
        return;
    }
    Place p = SavePlace(t);
    for (auto& g : t.list->groups) t.fold[g.second] = folded;
    Rebuild(t, &p, L"");
    if (!folded) t.scroll = 0;  // al desplegar todo, desde arriba
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

// Revisa en segundo plano las carpetas visibles y sus subcarpetas desplegadas.
void RevalidateVisible() {
    for (int p = 0; p < PaneCount(); ++p) {
        Tab& t = PT(p);
        if (!t.has) continue;
        Enqueue(t.path, JobKind::Revalidate);
        for (auto& [k, path] : t.expanded) Enqueue(path, JobKind::Revalidate);
    }
}

// Ejecuta un comando de la tabla de atajos.
void RunCommand(Cmd cmd) {
    Tab& t = T();
    switch (cmd) {
        case Cmd::None: break;
        case Cmd::Subir:
            if (t.recursive) StopRecursive(t, true);
            else if (!Collapse()) GoUp();
            break;
        case Cmd::Entrar:
            if (t.sel >= 0 && t.sel < Count() && EntryAt(t, t.sel).isDir()) OpenSelected(false);
            break;
        case Cmd::Abrir: OpenSelected(false); break;
        case Cmd::AbrirPestana: OpenSelected(true); break;
        case Cmd::Atras: GoBack(); break;
        case Cmd::Adelante: GoForward(); break;
        case Cmd::Renombrar: BeginRename(); break;
        case Cmd::Eliminar: DeleteSelection(); break;
        case Cmd::CarpetaNueva: NewItem(true); break;
        case Cmd::ArchivoNuevo: NewItem(false); break;
        case Cmd::Copiar:
            if (ClipboardSetFiles(SelectedFullPaths(t), false)) {
                g_ignoreClipUpdate = true;
                g_cutKeys.clear();
                Invalidate();
            }
            break;
        case Cmd::Cortar: {
            auto paths = SelectedFullPaths(t);
            if (ClipboardSetFiles(paths, true)) {
                g_ignoreClipUpdate = true;
                g_cutKeys.clear();
                for (const auto& p : paths) g_cutKeys.insert(Key(p));
                Invalidate();
            }
            break;
        }
        case Cmd::Pegar:
            if (ClipboardPaste(t.path) && !g_cutKeys.empty()) {
                g_cutKeys.clear();
                Invalidate();
            }
            break;
        case Cmd::SeleccionarTodo:
            SelectAll(t);
            Invalidate();
            break;
        case Cmd::Favorito: ToggleFavorite(); break;
        case Cmd::PestanaNueva: NewTab(t.path); break;
        case Cmd::CerrarPestana: CloseTab(g_cur); break;
        case Cmd::PestanaSiguiente: CycleTab(1); break;
        case Cmd::PestanaAnterior: CycleTab(-1); break;
        case Cmd::Ruta: ShowPathEdit(); break;
        case Cmd::BarraLateral: ToggleSidebar(); break;
        case Cmd::Recargar: RevalidateVisible(); break;
        case Cmd::Tiempos:
            S.show = !S.show;
            Invalidate();
            break;
        case Cmd::MenuContextual: {
            int r = Count() > 0 ? t.sel : -1;
            float y = r >= 0 ? ListTop() + r * kRow - t.scroll + kRow / 2 : ListTop() + 40;
            float x = PaneGeo(g_act).L + 60;
            RECT sr = ScreenRect(D2D1::RectF(x, y, x, y));
            OpenShellMenu(POINT{sr.left, sr.top}, r);
            break;
        }
        case Cmd::MenuApp: {
            RECT r = ScreenRect(D2D1::RectF(6, 7, 38, kTabH - 3));
            OpenAppMenu(r);
            break;
        }
        case Cmd::Paleta: PaletteOpen(0); break;
        case Cmd::IrA: PaletteOpen(1); break;
        case Cmd::BuscarSubcarpetas:
            if (t.recursive) StopRecursive(t, true);
            else StartRecursive(t);
            break;
        case Cmd::Desplegar: SetExpanded(t, t.sel, !IsExpanded(t, t.sel)); break;
        case Cmd::Plegar: Collapse(); break;
        case Cmd::IrAUbicacion: GoToLocation(); break;
        case Cmd::VistaPrevia: PreviewToggle(); break;
        case Cmd::Dividir: SetSplit(!g_split); break;
        case Cmd::OtroPanel: ActivatePane(1 - g_act); break;
        case Cmd::CopiarAlOtro: TransferToOther(false); break;
        case Cmd::MoverAlOtro: TransferToOther(true); break;
        case Cmd::CopiarRuta: CopyPathText(); break;
        case Cmd::Duplicar: Duplicate(); break;
        case Cmd::Instalacion: InstPanelOpen(); break;
        case Cmd::Salir: BgQuit(); break;
        case Cmd::Deshacer: UndoStep(true); break;
        case Cmd::Rehacer: UndoStep(false); break;
        case Cmd::Propiedades: Properties(); break;
        case Cmd::TamanoCarpetas: ComputeFolderSizes(); break;
        case Cmd::VerCarpetas:
            g_hideDirs = !g_hideDirs;
            if (g_hideDirs) g_hideFiles = false;  // nunca las dos cosas a la vez: la lista quedaría vacía
            S.message = g_hideDirs ? L"Sin carpetas: solo archivos" : L"Carpetas visibles";
            ResortAll();
            break;
        case Cmd::VerArchivos:
            g_hideFiles = !g_hideFiles;
            if (g_hideFiles) g_hideDirs = false;
            S.message = g_hideFiles ? L"Sin archivos: solo carpetas" : L"Archivos visibles";
            ResortAll();
            break;
        case Cmd::VerOcultos:
            g_cfg.showHidden = !g_cfg.showHidden;
            ConfigSetValue(L"general", L"ocultos", g_cfg.showHidden ? L"si" : L"no");
            RefreshConfigSig();
            S.message = g_cfg.showHidden ? L"Mostrando los archivos ocultos" : L"Escondiendo los archivos ocultos";
            ResortAll();
            break;
        case Cmd::VerExtensiones:
            g_hideExt = !g_hideExt;
            S.message = g_hideExt ? L"Extensiones ocultas" : L"Extensiones visibles";
            TouchSession();
            Invalidate();
            break;
        case Cmd::ReabrirPestana: ReopenTab(); break;
        case Cmd::CerrarOtras: CloseOtherTabs(); break;
        case Cmd::Terminal: OpenTerminal(); break;
        case Cmd::AbrirExplorador: OpenInExplorer(); break;
        case Cmd::UbicacionReal: GoToRealLocation(); break;
        case Cmd::PlegarGrupos: FoldAllGroups(t, true); break;
        case Cmd::DesplegarGrupos: FoldAllGroups(t, false); break;
        case Cmd::AgruparDia: SetGroup(GbDay); break;
        case Cmd::AgruparTipo: SetGroup(GbType); break;
        case Cmd::SinGrupos: SetGroup(-1); break;
        case Cmd::OrdenarTipo: SetSortField(SfExt, true); break;
        case Cmd::AumentarInterfaz: SetUiZoom(g_uiPx + 1); break;
        case Cmd::ReducirInterfaz: SetUiZoom(g_uiPx - 1); break;
        case Cmd::RestablecerInterfaz: SetUiZoom(12); break;
        case Cmd::FilaMas: SetRowHeight(kRow + 1); break;
        case Cmd::FilaMenos: SetRowHeight(kRow - 1); break;
        case Cmd::VaciarPapelera: EmptyRecycleBin(); break;
    }
}

// ---------------------------------------------------------------- ventana
float DipX(LPARAM l) { return GET_X_LPARAM(l) * 96.f / g_dpi; }

RECT ScreenRect(const D2D1_RECT_F& r) {
    POINT a{MulDiv(static_cast<int>(r.left), g_dpi, 96), MulDiv(static_cast<int>(r.top), g_dpi, 96)};
    POINT b{MulDiv(static_cast<int>(r.right), g_dpi, 96), MulDiv(static_cast<int>(r.bottom), g_dpi, 96)};
    ClientToScreen(g_hwnd, &a);
    ClientToScreen(g_hwnd, &b);
    return {a.x, a.y, b.x, b.y};
}
float DipY(LPARAM l) { return GET_Y_LPARAM(l) * 96.f / g_dpi; }

// Fila bajo el punto en un panel (sin importar cuál está activo), o -1.
// Fila bajo el punto, incluyendo los títulos de grupo, o -1.
int RawRowIn(int pane, float x, float y) {
    if (pane < 0 || pane >= PaneCount()) return -1;
    PaneX g = PaneGeo(pane);
    if (x < g.L || x >= g.R || y < ListTop() || y >= ListBottom()) return -1;
    Tab& t = PT(pane);
    int r = static_cast<int>(std::floor((y - ListTop() + t.scroll) / kRow));
    return r >= 0 && r < RowCount(t) ? r : -1;
}
int HitRowIn(int pane, float x, float y) {
    int r = RawRowIn(pane, x, y);
    return r >= 0 && !IsHeader(PT(pane), r) ? r : -1;
}
int HeaderRowIn(int pane, float x, float y) {
    int r = RawRowIn(pane, x, y);
    return r >= 0 && IsHeader(PT(pane), r) ? r : -1;
}

// Click en el título de un grupo: lo pliega o lo despliega.
void ToggleGroup(Tab& t, int row) {
    const Row& r = t.rows[row];
    const Listing* l = r.l;
    const int gi = -1 - r.e;
    const bool opening = GroupFolded(t, l, static_cast<size_t>(gi));
    t.fold[l->groups[gi].second] = !opening;
    Place p = SavePlace(t);
    const float scroll = t.scroll;
    Rebuild(t, &p, L"");
    t.scroll = scroll;  // el tÃ­tulo queda donde estaba (no se sigue al cursor)
    if (opening) {
        // que se vea el grupo entero, empezando por su tÃ­tulo
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
            const float top = h * kRow, bottom = e * kRow, view = ListHeight();
            if (top < t.scroll) t.scroll = top;
            if (bottom > t.scroll + view) t.scroll = std::min(top, bottom - view);
        }
    }
    ClampScroll(t);
    Invalidate();
}
// Fila bajo el punto en el panel activo.
int HitRow(float x, float y) { return HitRowIn(g_act, x, y); }

Hit HitAt(float x, float y) {
    // los últimos agregados (más específicos) tienen prioridad
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return *it;
    return {{}, HitKind::None, -1};
}

// ¿El punto está sobre el chevrón (desplegar) de una fila de carpeta?
bool OnChevron(int row, float x) {
    Tab& t = T();
    if (row < 0 || row >= Count() || FlatView(t) || !EntryAt(t, row).isDir()) return false;
    Columns c = Cols(g_act);
    float cx = c.nameL + RowIndent(t, row);
    return x >= cx - 4 && x < cx + kChev + 2;
}

bool g_divDrag = false;  // arrastrando el divisor de la vista dividida
bool g_eatSysChar = false;  // la tecla con Alt ya se usó: su WM_SYSCHAR no debe sonar
int g_tabWheelAcc = 0;      // Ctrl+rueda en ruedas de alta resolución: se acumula hasta completar un paso

// Ancho nuevo de una columna (1 = fecha, 2 = tamaño), siempre dejándole lugar al nombre.
void SetColWidth(int which, float w, int pane) {
    const PaneX g = PaneGeo(pane);
    const float room = g.R - g.L - 350;
    if (which == 1) kDateW = std::clamp(w, kDateMin, std::max(kDateMin, std::min(kDateMax, room - kSizeW)));
    else kSizeW = std::clamp(w, kSizeMin, std::max(kSizeMin, std::min(kSizeMax, room - (Cols(pane).showDate ? kDateW : -42.f))));
    LayoutRename();
    Invalidate();
}

void SaveColWidth(int which) {
    ConfigSetValue(L"columnas", which == 1 ? L"ancho_modificado" : L"ancho_tamano",
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

// Doble click en el borde: el ancho justo para lo que hay en la carpeta.
void FitColumn(int which, int pane) {
    Tab& t = PT(pane);
    float w = MeasureW(which == 1 ? L"Modificado" : L"Tamaño", g_fmtRight.Get()) + 16;
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
        SelectOnly(T(), T().sel);  // click en el vacío: queda solo el cursor
        std::fill(T().selMask.begin(), T().selMask.end(), uint8_t{0});
        Invalidate();
    }
}

// Grosor del marco invisible que Windows deja alrededor de una ventana sin barra de título.
void FrameMetrics(HWND hwnd, int& fx, int& fy) {
    UINT dpi = GetDpiForWindow(hwnd);
    int pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + pad;
    fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + pad;
}

bool IsCapButton(WPARAM ht) { return ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE; }

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // con el panel de instalación abierto, nada llega a lo de atrás
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
    // con la paleta abierta, el teclado es de la paleta
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
    // con un menú emergente abierto, el teclado es del menú
    if (g_popup.open()) {
        if (msg == WM_KEYDOWN) {
            PopupKey(wp);
            return 0;
        }
        if (msg == WM_CHAR || msg == WM_SYSKEYDOWN) return 0;
    }
    // con el editor de vistas abierto, el mouse y el teclado de la lista son del editor
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
        // --- barra de título propia: el área cliente llega hasta arriba de todo
        case WM_NCCALCSIZE: {
            if (!wp) break;
            RECT& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
            int fx, fy;
            FrameMetrics(hwnd, fx, fy);
            r.left += fx;
            r.right -= fx;
            r.bottom -= fy;
            if (IsZoomed(hwnd)) r.top += fy;  // maximizada, el marco queda fuera de la pantalla
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
                return 0;  // que Windows no dibuje su propio botón
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
            if (g_test) ReleaseTarget();  // el bitmap se rehace con el tamaño nuevo
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
            LayoutRename();  // la lista pudo moverse debajo del cuadro de renombrar
            return 0;
        }
        case WM_APP_DIRSIZE:
            g_dsNotify = false;
            if (g_dsFinished.exchange(false) && static_cast<SortBy>(g_sortBy.load()) == SortBy::Size) ResortAll();
            if (!g_dsBusy && S.message.rfind(L"Calculando el tamaño", 0) == 0) S.message = L"Tamaños calculados";
            Invalidate();
            return 0;
        case WM_APP_PREVIEW:
            PreviewApply(reinterpret_cast<PreviewResult*>(lp));
            return 0;
        case WM_APP_REC:
            ApplyRecBatch(reinterpret_cast<RecBatch*>(lp));
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
                // conservar lo elegido si ya se había movido
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
            SetTimer(hwnd, kTimerRevalidate, 80, nullptr);  // agrupa ráfagas de cambios
            return 0;
        case WM_APP_ICON:
            g_iconDirty = false;
            Invalidate();
            return 0;
        case WM_TIMER:
            KillTimer(hwnd, wp);
            if (wp == kTimerRevalidate) RevalidateVisible();
            if (wp == kTimerPreview) PreviewLoad();
            if (wp == kTimerTest) {
                if (g_testBlocking) SetTimer(hwnd, kTimerTest, 30, nullptr);  // un paso todavía está esperando
                else TestRunStep();
            }
            if (wp == kTimerTestKill) {
                TestLog(L"se cortó por tiempo");
                ExitProcess(1);
            }
            if (wp == kTimerPrefetch) Prefetch();
            if (wp == kTimerBox) BoxTick();
            if (wp == kTimerTabSlide) Invalidate();
            if (wp == kTimerSession) SaveSession();
            if (wp == kTimerConfig) ReloadConfig();
            return 0;
        case WM_APP_CONFIG:
            SetTimer(hwnd, kTimerConfig, 250, nullptr);  // los editores guardan en varias escrituras
            return 0;
        case WM_CLIPBOARDUPDATE:
            // si el portapapeles cambió desde afuera, lo "cortado" deja de estarlo
            if (g_ignoreClipUpdate) g_ignoreClipUpdate = false;
            else if (!g_cutKeys.empty()) {
                g_cutKeys.clear();
                Invalidate();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (g_box.active) {  // mientras se dibuja el recuadro, el teclado no cambia la lista
                if (wp == VK_ESCAPE) BoxCancel();
                return 0;
            }
            const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT),
                       alt = KeyDown(VK_MENU);
            // con filtro activo, Retroceso y Esc son del filtro (y Espacio se escribe)
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
            // en la búsqueda en subcarpetas sin texto, Esc y Retroceso vuelven a la carpeta
            if (T().recursive && T().filter.empty() && msg == WM_KEYDOWN && !ctrl && !alt &&
                (wp == VK_ESCAPE || wp == VK_BACK)) {
                StopRecursive(T(), true);
                return 0;
            }
            // primero los atajos configurables (config.txt)
            const UINT mods = (ctrl ? ModCtrl : 0) | (shift ? ModShift : 0) | (alt ? ModAlt : 0);
            Cmd cmd = FindCommand(mods, static_cast<UINT>(wp));
            if (cmd != Cmd::None) {
                g_eatSysChar = msg == WM_SYSKEYDOWN;
                RunCommand(cmd);
                return 0;
            }
            // Alt+1..8: esa pestaña del panel; Alt+9: la última
            if (alt && !ctrl && !shift && wp >= '1' && wp <= '9') {
                const std::vector<int> idx = ActiveTabs();
                const size_t i = wp == '9' ? idx.size() - 1 : static_cast<size_t>(wp - '1');
                if (i < idx.size()) SelectTab(idx[i]);
                g_eatSysChar = true;
                return 0;
            }
            if (msg == WM_SYSKEYDOWN) break;  // Alt+F4, Alt+Espacio, etc. siguen siendo del sistema
            // teclas fijas: cursor y selección
            const int page = std::max(1, static_cast<int>(ListHeight() / kRow) - 1);
            switch (wp) {
                case VK_UP: MoveCursor(T().sel - 1, shift); break;
                case VK_DOWN: MoveCursor(T().sel + 1, shift); break;
                case VK_PRIOR: MoveCursor(T().sel - page, shift); break;
                case VK_NEXT: MoveCursor(T().sel + page, shift); break;
                case VK_HOME: MoveCursor(0, shift); break;
                case VK_END: MoveCursor(Count() - 1, shift); break;
                case VK_SPACE: if (ctrl) MoveCursor(T().sel, false, true); break;
                case VK_ESCAPE:
                    // (sin buffer de tipeo)
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
                // una letra sin modificadores que sea un atajo configurado no filtra
                SHORT vks = VkKeyScanW(static_cast<wchar_t>(wp));
                UINT vk = LOBYTE(vks), kmods = (HIBYTE(vks) & 1) ? ModShift : 0;
                bool spaceInFilter = wp == L' ' && !T().filter.empty();
                if (spaceInFilter || FindCommand(kmods, vk) == Cmd::None) FilterAppend(static_cast<wchar_t>(wp));
            }
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = DipX(lp), y = DipY(lp);
            if (HitAt(x, y).kind != HitKind::Divider) ActivatePane(PaneAt(x, y));
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
                // ya estaba seleccionado: no se toca la selección hasta soltar (puede ser un arrastre)
                T().sel = T().anchor = r;
                Invalidate();
                PressBegin(r, pt, true);
                g_press.box = box;
                return 0;
            }
            if (!(r < 0 && mods && box)) OnClick(x, y, false);  // Ctrl o Shift en el vacío no pierden la selección
            if (box || (r >= 0 && !mods)) {
                PressBegin(r, pt, false);
                g_press.box = box;
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_colDrag) {
                ColDragEnd();
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
            BoxEnd();
            g_divDrag = false;
            ColDragEnd();
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
            if (r >= 0 && !IsSel(T(), r)) SetSel(r);  // sobre algo ya seleccionado, el menú actúa sobre todo
            else if (r >= 0) { T().sel = r; Invalidate(); }
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = DipX(lp), y = DipY(lp);
            if (PaneAt(x, y) == g_act && y >= ListTop() && y < ListBottom()) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(hwnd, &pt);
                OpenShellMenu(pt, HitRow(x, y));
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            PressEnd(false);
            Hit dh = HitAt(DipX(lp), DipY(lp));
            if (dh.kind == HitKind::ColEdge) {
                FitColumn(dh.idx % 2 + 1, dh.idx / 2);
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
                OnClick(DipX(lp), DipY(lp), false);  // dos clicks seguidos en un botón = dos acciones
                return 0;
            }
            int r = HitRow(DipX(lp), DipY(lp));
            // el segundo click rápido llega como doble click: en un título de grupo o un chevrón cuenta
            // como otro click normal (plegar y desplegar al instante, sin esperar)
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
            // Ctrl+rueda: pestañas del panel que está debajo (arriba = siguiente, abajo = anterior).
            // Ctrl viene en el propio mensaje de la rueda, así que no depende del foco.
            if ((GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) || (g_test && KeyDown(VK_CONTROL))) {
                if ((g_tabWheelAcc > 0) != (delta > 0)) g_tabWheelAcc = 0;
                g_tabWheelAcc += delta;
                const int steps = g_tabWheelAcc / WHEEL_DELTA;
                g_tabWheelAcc -= steps * WHEEL_DELTA;
                if (steps) ActivatePane(PaneAt(x, y));
                // con Shift, en vez de cambiar de pestaña, mueve la activa
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
            Tab& wt = PT(pane);  // la rueda mueve el panel que está debajo, sin activarlo
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
            if (g_colDrag) {
                // las columnas están ancladas a la derecha: el borde izquierdo sigue al mouse
                SetColWidth(g_colDrag, g_colDragW + g_colDragX - x, g_colDragPane);
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
                    g_press = {};  // la captura sigue: ahora es del recuadro
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
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
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
                const bool sizeWE = h.kind == HitKind::Divider || h.kind == HitKind::ColEdge;
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
            if (g_cfg.background && !g_quitting) {
                BgHide();  // la X esconde: ☰ → Salir cierra de verdad
                return 0;
            }
            if (g_test) break;  // las pruebas no guardan nada
            SaveSession();
            if (g_dsAny) SaveDirSizes();  // lo ya calculado, aunque quede algo a medias
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp && !g_test) {
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
            if (cd->dwData == 1 && cd->cbData >= sizeof(wchar_t)) {
                const std::wstring p(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t) - 1);
                if (!p.empty()) BgOpenPath(p);
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
    if (g_taskbarCreated && msg == g_taskbarCreated) {  // el Explorador se reinició: la bandeja se perdió
        g_trayOn = false;
        BgApplyConfig();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// La carpeta de la línea de comandos. En una unidad llega como "C:\\" y Windows lee esa barra como comilla escapada.
std::wstring ArgPath() {
    if (g_test) return L"";
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring p;
    for (int i = 1; argv && i < argc && p.empty(); ++i) {
        std::wstring a = argv[i];
        if (a.rfind(L"--", 0) == 0) continue;
        while (!a.empty() && a.back() == L'"') a.pop_back();
        if (a.size() == 2 && a[1] == L':') a += L'\\';
        wchar_t full[4096];
        if (GetFullPathNameW(a.c_str(), 4096, full, nullptr)) p = Normalize(full);
    }
    if (argv) LocalFree(argv);
    return p;
}

bool HasArg(const wchar_t* flag) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; argv && i < argc; ++i) found |= wcscmp(argv[i], flag) == 0;
    if (argv) LocalFree(argv);
    return found;
}

// Carpeta de la primera pestaña cuando no hay nada guardado: la del usuario.
std::wstring DefaultPath() {
    wchar_t* p = nullptr;
    std::wstring r = L"C:\\";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

// ¿Existe y es una carpeta?
bool FolderExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(ToExtendedPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    if (cmdLine && wcsstr(cmdLine, L"--indexar-mft")) {  // ayudante elevado del índice (ver indexer.inl)
        IdxElevatedMain();
        return 0;
    }
    if (HasArg(L"--salir")) {
        BgSendQuit();
        return 0;
    }
    if (HasArg(L"--desinstalar")) {
        InstUninstallMain();
        return 0;
    }
    std::wstring testScript, deskScript;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--prueba") == 0) testScript = argv[i + 1];
            if (wcscmp(argv[i], L"--prueba-escritorio") == 0) deskScript = argv[i + 1];
            if (wcscmp(argv[i], L"--salida") == 0) g_testOut = argv[i + 1];
        }
        if (argv) LocalFree(argv);
    }
    // en un escritorio de Windows aparte: las ventanas del sistema que se abran (propiedades, conflictos
    // al copiar...) quedan ahí, invisibles para el usuario
    if (!deskScript.empty()) return TestOnHiddenDesktop(deskScript, g_testOut);
    if (!testScript.empty()) {
        g_test = true;
        TestPrepareData();
        if (!TestLoadScript(testScript)) {
            TestLog(L"no se pudo leer el guion: " + testScript);
            return 1;
        }
    }
    const bool hidden = !g_test && HasArg(L"--oculto");
    if (!g_test && BgHandOff(hidden ? L"" : ArgPath())) return 0;
    BootMark(L"inicio");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetErrorMode(SEM_FAILCRITICALERRORS);  // nada de cuadros "inserte un disco"
    OleInitialize(nullptr);  // COM de apartamento + arrastrar y soltar
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(g_dw.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    BootMark(L"com y fabricas");
    CreateTextParams();
    LoadConfig();
    CreateFormats();
    LoadFavs();
    g_configSig = ConfigSignature();

    BootMark(L"configuracion");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = g_test ? L"JoltPrueba" : kWindowClass;  // una prueba no se confunde con el Jolt abierto
    RegisterClassExW(&wc);
    RegisterPopupClass(inst);
    RegisterEditorClass(inst);

    UINT sysDpi = GetDpiForSystem();
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Jolt", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1100, sysDpi, 96), MulDiv(700, sysDpi, 96), nullptr, nullptr, inst, nullptr);
    g_sysDpi = GetDpiForWindow(g_hwnd);
    ApplyDpi();

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF border = RGB(0x2B, 0x2B, 0x31);
    DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    // aplica WM_NCCALCSIZE: la barra de título de Windows desaparece y la dibujamos nosotros
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (g_test)  // pruebas: fuera de la pantalla y sin mostrarse nunca
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
        std::thread(LoadDirSizes).detach();  // tamaños de carpetas de sesiones anteriores
        IdxStart();                          // índice de los discos para Ctrl+P
    } else {
        IdxStartReadOnly();  // en las pruebas, el índice se usa pero no se toca
    }

    BootMark(L"ventana e hilos");
    InitDragDrop();
    AddClipboardFormatListener(g_hwnd);
    BootMark(L"arrastre");

    // sesión anterior: ventana, barra lateral, orden y pestañas
    Session ses = LoadSession();
    BootMark(L"sesion");
    // vistas; la primera vez, la general sale del orden que había (sesión) y de carpetas_primero (config)
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
    BootMark(L"vistas y posicion");
    // pestañas: se saltean las carpetas que ya no existen (y se corrigen los índices guardados)
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
    std::wstring arg = ArgPath();
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
    BootMark(L"pestanas");
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    BgApplyConfig();
    if (hidden) Render();  // el primer cuadro es el más caro: que quede hecho antes de mostrarse
    if (!g_test && !hidden && (HasArg(L"--panel") || InstShouldOffer())) InstPanelOpen();
    S.navPending = false;  // el arranque no es una navegación; no se mide
    S.navMs = -1;
    if (g_test) {
        SetTimer(g_hwnd, kTimerTest, 400, nullptr);        // primer paso del guion
        SetTimer(g_hwnd, kTimerTestKill, 180000, nullptr); // por las dudas, nunca más de 3 minutos
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
    // Salida directa: los hilos de fondo no necesitan limpieza ordenada.
    ExitProcess(0);
}

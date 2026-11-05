// Instalación al estilo File Pilot: el mismo .exe es el instalador. Abierto desde otro lado ofrece copiarse a
// %LOCALAPPDATA%\Jolt, y desde el panel se prenden y apagan el PATH, el explorador predeterminado y el inicio con Windows.
// Todo es del usuario actual (HKCU): no pide permisos de administrador.
// En el modo de prueba, el registro y las carpetas se redirigen: nunca se toca el sistema real.

constexpr wchar_t kRegUninstall[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Jolt";
constexpr wchar_t kRegRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRegBackup[] = L"Software\\Jolt\\Anterior";
constexpr wchar_t kRegEnv[] = L"Environment";
const wchar_t* const kFolderClasses[] = {L"Directory", L"Drive"};
// Win+E no pasa por Directory ni Drive: Windows abre este objeto, el de la vista de inicio del Explorador.
constexpr wchar_t kRegWinE[] = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";

std::wstring ExePath() {
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size())));
    return p;
}

std::wstring RegKey(const std::wstring& sub) { return g_test ? L"Software\\JoltPrueba\\" + sub : sub; }

bool RegRead(const std::wstring& sub, const wchar_t* name, std::wstring& out, DWORD* type = nullptr) {
    const std::wstring k = RegKey(sub);
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), name, flags, type, nullptr, &size) != ERROR_SUCCESS) return false;
    out.resize(size / sizeof(wchar_t) + 1);
    if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), name, flags, type, out.data(), &size) != ERROR_SUCCESS) return false;
    out.resize(wcsnlen(out.c_str(), out.size()));
    return true;
}

void RegWrite(const std::wstring& sub, const wchar_t* name, const std::wstring& v, DWORD type = REG_SZ) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegKey(sub).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, type, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

void RegWriteDword(const std::wstring& sub, const wchar_t* name, DWORD v) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegKey(sub).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(h);
}

void RegRemoveValue(const std::wstring& sub, const wchar_t* name) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RegKey(sub).c_str(), 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return;
    RegDeleteValueW(h, name);
    RegCloseKey(h);
}

void RegRemoveTree(const std::wstring& sub) { RegDeleteTreeW(HKEY_CURRENT_USER, RegKey(sub).c_str()); }
void RegRemoveIfEmpty(const std::wstring& sub) { RegDeleteKeyW(HKEY_CURRENT_USER, RegKey(sub).c_str()); }  // con subclaves, falla y queda

std::wstring InstDir() {
    if (g_test) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        return std::wstring(tmp) + L"jolt-prueba\\instalado";
    }
    return LocalDataDir();
}
std::wstring InstExe() { return InstDir() + L"\\jolt.exe"; }

std::wstring InstShortcut() {
    if (g_test) return InstDir() + L"\\Jolt.lnk";
    wchar_t* p = nullptr;
    std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) f = std::wstring(p) + L"\\Jolt.lnk";
    CoTaskMemFree(p);
    return f;
}

bool InstIsInstalled() { return PathExists(InstExe()); }
bool InstRunningInstalled() { return Key(ExePath()) == Key(InstExe()); }

// ¿Este ejecutable es distinto del instalado?
bool InstDiffers() {
    if (!InstIsInstalled() || InstRunningInstalled()) return false;
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    if (!GetFileAttributesExW(ExePath().c_str(), GetFileExInfoStandard, &a) || !GetFileAttributesExW(InstExe().c_str(), GetFileExInfoStandard, &b))
        return false;
    return a.nFileSizeLow != b.nFileSizeLow || CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) != 0;
}

// Las compilaciones de desarrollo llevan "jolt.dev" al lado: no ofrecen instalarse solas.
bool InstShouldOffer() {
    const std::wstring exe = ExePath();
    if (InstRunningInstalled() || PathExists(exe.substr(0, exe.find_last_of(L'\\')) + L"\\jolt.dev")) return false;
    return !InstIsInstalled() || InstDiffers();
}

// ---- PATH del usuario

void InstEnvChanged() {
    if (g_test) return;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 200, nullptr);
}

std::vector<std::wstring> InstPathParts(DWORD& type) {
    std::wstring v;
    type = REG_EXPAND_SZ;
    RegRead(kRegEnv, L"Path", v, &type);
    std::vector<std::wstring> parts;
    size_t at = 0;
    while (at <= v.size() && !v.empty()) {
        const size_t semi = std::min(v.find(L';', at), v.size());
        if (semi > at) parts.push_back(v.substr(at, semi - at));
        at = semi + 1;
    }
    return parts;
}

bool InstPathMatches(const std::wstring& part) {
    std::wstring p = part;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return Key(p) == Key(InstDir());
}

bool InstInPath() {
    DWORD type;
    for (const auto& p : InstPathParts(type))
        if (InstPathMatches(p)) return true;
    return false;
}

void InstSetPath(bool on) {
    DWORD type;
    std::vector<std::wstring> parts = InstPathParts(type);
    parts.erase(std::remove_if(parts.begin(), parts.end(), InstPathMatches), parts.end());
    if (on) parts.push_back(InstDir());
    std::wstring v;
    for (const auto& p : parts) v += (v.empty() ? L"" : L";") + p;
    RegWrite(kRegEnv, L"Path", v, type);
    InstEnvChanged();
}

// ---- explorador predeterminado

std::wstring InstOpenCommand() { return L"\"" + InstExe() + L"\" \"%1\""; }
std::wstring InstClassShell(const wchar_t* cls) { return std::wstring(L"Software\\Classes\\") + cls + L"\\shell"; }
std::wstring InstWinEVerb() { return std::wstring(kRegWinE) + L"\\shell\\opennewwindow"; }
std::wstring InstWinECommand() { return L"\"" + InstExe() + L"\" --alternar"; }

bool InstIsDefault() {
    for (const wchar_t* cls : kFolderClasses) {
        std::wstring cmd;
        if (!RegRead(InstClassShell(cls) + L"\\open\\command", nullptr, cmd) || Key(cmd) != Key(InstOpenCommand())) return false;
    }
    std::wstring cmd;
    return RegRead(InstWinEVerb() + L"\\command", nullptr, cmd) && Key(cmd) == Key(InstWinECommand());
}

// DelegateExecute vacío hace que Windows use este comando y no el manejador propio del Explorador.
void InstSetWinE(bool on) {
    const std::wstring verb = InstWinEVerb(), command = verb + L"\\command";
    std::wstring v;
    if (on) {
        if (!RegRead(kRegBackup, L"WinE.guardado", v)) {
            if (RegRead(command, nullptr, v)) RegWrite(kRegBackup, L"WinE.comando", v);
            if (RegRead(command, L"DelegateExecute", v)) RegWrite(kRegBackup, L"WinE.delegado", v);
            RegWrite(kRegBackup, L"WinE.guardado", L"1");
        }
        RegWrite(command, nullptr, InstWinECommand());
        RegWrite(command, L"DelegateExecute", L"");
        return;
    }
    if (!RegRead(kRegBackup, L"WinE.guardado", v)) return;
    if (RegRead(kRegBackup, L"WinE.comando", v)) {
        RegWrite(command, nullptr, v);
        if (RegRead(kRegBackup, L"WinE.delegado", v)) RegWrite(command, L"DelegateExecute", v);
        else RegRemoveValue(command, L"DelegateExecute");
    } else {
        RegRemoveTree(verb);
        RegRemoveIfEmpty(std::wstring(kRegWinE) + L"\\shell");
        RegRemoveIfEmpty(kRegWinE);
    }
    RegRemoveValue(kRegBackup, L"WinE.guardado");
    RegRemoveValue(kRegBackup, L"WinE.comando");
    RegRemoveValue(kRegBackup, L"WinE.delegado");
}

// Antes de pisar nada se guarda lo que había, una sola vez: al apagar la opción vuelve exactamente eso.
void InstSetDefault(bool on) {
    for (const wchar_t* cls : kFolderClasses) {
        const std::wstring shell = InstClassShell(cls), command = shell + L"\\open\\command", c = cls;
        const std::wstring saved = c + L".guardado", verbName = c + L".verbo", cmdName = c + L".comando";
        std::wstring v;
        if (on) {
            if (!RegRead(kRegBackup, saved.c_str(), v)) {
                if (RegRead(shell, nullptr, v)) RegWrite(kRegBackup, verbName.c_str(), v);
                if (RegRead(command, nullptr, v)) RegWrite(kRegBackup, cmdName.c_str(), v);
                RegWrite(kRegBackup, saved.c_str(), L"1");
            }
            RegWrite(shell, nullptr, L"open");
            RegWrite(command, nullptr, InstOpenCommand());
        } else {
            if (!RegRead(kRegBackup, saved.c_str(), v)) continue;
            if (RegRead(kRegBackup, verbName.c_str(), v)) RegWrite(shell, nullptr, v);
            else RegRemoveValue(shell, nullptr);
            if (RegRead(kRegBackup, cmdName.c_str(), v)) RegWrite(command, nullptr, v);
            else RegRemoveTree(shell + L"\\open");
            RegRemoveValue(kRegBackup, saved.c_str());
            RegRemoveValue(kRegBackup, verbName.c_str());
            RegRemoveValue(kRegBackup, cmdName.c_str());
        }
    }
    InstSetWinE(on);
    if (!g_test) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// ---- inicio con Windows

bool InstStartsWithWindows() {
    std::wstring v;
    return RegRead(kRegRun, L"Jolt", v);
}

void InstSetStartup(bool on) {
    if (on) RegWrite(kRegRun, L"Jolt", L"\"" + InstExe() + L"\" --oculto");
    else RegRemoveValue(kRegRun, L"Jolt");
}

// ---- instalar y desinstalar

bool InstMakeShortcut() {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(InstExe().c_str());
    link->SetDescription(L"Explorador de archivos");
    return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(InstShortcut().c_str(), TRUE));
}

// Copia este ejecutable a la carpeta de instalación. Si ya hay uno, lo reemplaza.
bool InstInstall() {
    const std::wstring exe = InstExe(), tmp = exe + L".tmp";
    CreateDirectoryW(InstDir().c_str(), nullptr);
    if (!CopyFileW(ExePath().c_str(), tmp.c_str(), FALSE) || !MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    InstMakeShortcut();
    RegWrite(kRegUninstall, L"DisplayName", L"Jolt");
    RegWrite(kRegUninstall, L"Publisher", L"Jolt");
    RegWrite(kRegUninstall, L"InstallLocation", InstDir());
    RegWrite(kRegUninstall, L"DisplayIcon", exe);
    RegWrite(kRegUninstall, L"UninstallString", L"\"" + exe + L"\" --desinstalar");
    RegWriteDword(kRegUninstall, L"NoModify", 1);
    RegWriteDword(kRegUninstall, L"NoRepair", 1);
    return true;
}

// Deshace todo. El ejecutable en uso no se puede borrar: lo borra un proceso aparte un momento después.
// Los datos del usuario (configuración, favoritos, índices) quedan.
void InstUninstall() {
    InstSetDefault(false);
    InstSetPath(false);
    InstSetStartup(false);
    DeleteFileW(InstShortcut().c_str());
    RegRemoveTree(kRegUninstall);
    RegRemoveTree(L"Software\\Jolt");
    if (g_test || !InstRunningInstalled()) {
        DeleteFileW(InstExe().c_str());
        return;
    }
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + InstExe() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---- panel

enum class InstK { None, Install, Path, Default, Startup, Uninstall, Close };
struct InstHit {
    D2D1_RECT_F r;
    InstK k;
};
struct InstPanel {
    bool open = false;
    std::vector<InstHit> hits;
    InstK hover = InstK::None;
    D2D1_RECT_F box{};
    std::wstring note;
} g_inst;

void InstPanelOpen() {
    g_inst.open = true;
    g_inst.note.clear();
    Invalidate();
}
void InstPanelClose() {
    g_inst.open = false;
    Invalidate();
}

void DrawInstallPanel(float W, float H) {
    if (!g_inst.open) return;
    g_inst.hits.clear();
    constexpr float kW = 470, kRowH = 58, kHead = 62, kFoot = 52;
    const bool installed = InstIsInstalled();
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    const float h = kHead + 4 * kRowH + kFoot, x = std::floor((W - kW) / 2), y = std::floor((H - h) / 2);
    g_inst.box = D2D1::RectF(x, y, x + kW, y + h);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x - i, y - i + 2, x + kW + i, y + h + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_inst.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_inst.box, 12, 12), g_brush.Get(), 1.f);

    Color(0xFFB23A);
    Glyph(0xE945, g_fmtIcon.Get(), x + 20, y + 16, x + 44, y + 44);
    Color(cText);
    Text(L"Instalación de Jolt", g_fmtName.Get(), x + 50, y + 12, x + kW - 20, y + 32);
    Color(cDim);
    Text(installed ? (InstRunningInstalled() ? L"Instalado en tu carpeta de usuario" : L"Ya hay un Jolt instalado") : L"Sin permisos de administrador: es solo para tu usuario",
         g_fmtName.Get(), x + 50, y + 30, x + kW - 20, y + 50);

    auto row = [&](int i, const wchar_t* title, const wchar_t* help, bool on, InstK k, bool enabled) {
        const float ry = y + kHead + i * kRowH, a = enabled ? 1.f : 0.45f;
        Fill(D2D1::RectF(x + 20, ry, x + kW - 20, ry + 1), cBorder);
        Color(cText, a);
        Text(title, g_fmtName.Get(), x + 20, ry + 10, x + kW - 90, ry + 30);
        Color(cDim, a);
        Text(enabled ? help : L"Primero instalá Jolt", g_fmtName.Get(), x + 20, ry + 29, x + kW - 90, ry + 49);
        const D2D1_RECT_F t = D2D1::RectF(x + kW - 62, ry + 19, x + kW - 20, ry + 41);
        Round(t, 11, on ? cAccent : 0xFFFFFF, on ? a : 0.12f * a);
        const float kx = on ? t.right - 19 : t.left + 3;
        Round(D2D1::RectF(kx, t.top + 3, kx + 16, t.bottom - 3), 8, 0xFFFFFF, a);
        if (enabled) g_inst.hits.push_back({D2D1::RectF(x + 12, ry + 2, x + kW - 12, ry + kRowH - 2), k});
    };
    {
        const float ry = y + kHead;
        const bool update = InstDiffers();
        Fill(D2D1::RectF(x + 20, ry, x + kW - 20, ry + 1), cBorder);
        Color(cText);
        Text(L"Instalar", g_fmtName.Get(), x + 20, ry + 10, x + kW - 130, ry + 30);
        Color(cDim);
        Text(update ? L"Este archivo es distinto del instalado" : L"Copia Jolt a tu usuario y lo suma al menú Inicio", g_fmtName.Get(), x + 20, ry + 29,
             x + kW - 130, ry + 49);
        const bool can = !InstRunningInstalled() && (!installed || update);
        const D2D1_RECT_F b = D2D1::RectF(x + kW - 118, ry + 15, x + kW - 20, ry + 45);
        if (can) {
            Round(b, 7, cAccent, g_inst.hover == InstK::Install ? 1.f : 0.85f);
            Color(0xFFFFFF);
            g_inst.hits.push_back({b, InstK::Install});
        } else {
            Color(cDim);
        }
        Text(can ? (update ? L"Actualizar" : L"Instalar") : L"Instalado", g_fmtCenter.Get(), b.left, b.top, b.right, b.bottom);
    }
    row(1, L"Agregar al PATH", L"Para abrirlo desde la terminal escribiendo jolt", installed && InstInPath(), InstK::Path, installed);
    row(2, L"Explorador predeterminado", L"Las carpetas se abren con Jolt; se puede volver atrás", installed && InstIsDefault(), InstK::Default, installed);
    row(3, L"Iniciar con Windows", L"Arranca escondido, listo para el atajo", installed && InstStartsWithWindows(), InstK::Startup, installed);

    const float fy = y + kHead + 4 * kRowH;
    Fill(D2D1::RectF(x + 20, fy, x + kW - 20, fy + 1), cBorder);
    if (!g_inst.note.empty()) {
        Color(cDim);
        Text(g_inst.note, g_fmtName.Get(), x + 20, fy + 16, x + kW - 110, fy + 38);
    } else if (installed) {
        const D2D1_RECT_F u = D2D1::RectF(x + 12, fy + 12, x + 104, fy + 40);
        if (g_inst.hover == InstK::Uninstall) Round(u, 7, 0xFFFFFF, 0.06f);
        Color(cDim);
        Text(L"Desinstalar", g_fmtCenter.Get(), u.left, u.top, u.right, u.bottom);
        g_inst.hits.push_back({u, InstK::Uninstall});
    }
    const D2D1_RECT_F c = D2D1::RectF(x + kW - 104, fy + 12, x + kW - 20, fy + 40);
    Round(c, 7, 0xFFFFFF, g_inst.hover == InstK::Close ? 0.14f : 0.08f);
    Color(cText);
    Text(L"Cerrar", g_fmtCenter.Get(), c.left, c.top, c.right, c.bottom);
    g_inst.hits.push_back({c, InstK::Close});
}

InstK InstHitAt(float x, float y) {
    for (const InstHit& h : g_inst.hits)
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h.k;
    return InstK::None;
}

void InstHover(float x, float y) {
    const InstK k = InstHitAt(x, y);
    if (k != g_inst.hover) {
        g_inst.hover = k;
        Invalidate();
    }
}

void InstClick(float x, float y) {
    g_inst.note.clear();
    switch (InstHitAt(x, y)) {
        case InstK::Install:
            if (!InstInstall()) {
                g_inst.note = L"No se pudo copiar el ejecutable";
            } else if (g_test) {
                g_inst.note = L"Instalado";
            } else {
                g_relaunch = InstExe();  // sigue desde el instalado, con el panel abierto
                g_relaunchArgs = L"--panel";
                BgQuit();
            }
            break;
        case InstK::Path: InstSetPath(!InstInPath()); break;
        case InstK::Default: InstSetDefault(!InstIsDefault()); break;
        case InstK::Startup: InstSetStartup(!InstStartsWithWindows()); break;
        case InstK::Uninstall:
            if (g_test || MessageBoxW(g_hwnd, L"¿Desinstalar Jolt? Tu configuración y tus favoritos quedan guardados.", L"Jolt", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                InstUninstall();
                if (g_test) g_inst.note = L"Desinstalado";
                else BgQuit();
            }
            break;
        case InstK::Close: InstPanelClose(); break;
        case InstK::None: break;
    }
    Invalidate();
}

// "jolt.exe --desinstalar", desde Aplicaciones instaladas de Windows.
void InstUninstallMain() {
    if (MessageBoxW(nullptr, L"¿Desinstalar Jolt? Tu configuración y tus favoritos quedan guardados.", L"Jolt", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    BgSendQuit();
    InstUninstall();
}

// Jolt always ready: a single instance that hides instead of closing and comes back instantly with a global
// shortcut, the tray, or when another process asks it to open a folder.

constexpr UINT WM_APP_TRAY = WM_APP + 40, WM_APP_QUIT = WM_APP + 41;
constexpr int kHotkeyId = 1;
constexpr UINT kTrayShow = 1, kTrayQuit = 2;
constexpr wchar_t kWindowClass[] = L"JoltWindow";

HANDLE g_instance = nullptr;
bool g_quitting = false;    // really close, not hide
bool g_showMaxOnce = false;  // started hidden and the window was maximized
bool g_testShown = true;     // in tests the window is never visible: it is tracked here
bool g_hotkeyOk = false, g_trayOn = false;
UINT g_taskbarCreated = 0;
std::wstring g_relaunch, g_relaunchArgs;  // on quit, open this (after installing)

void TestLog(const std::wstring& s);
bool FolderExists(const std::wstring& p);
void SaveSession();
void InstPanelClose();

// ---- single instance

// If a Jolt already exists, passes it the folder and returns true. The starting process may come to the front; it hands that over.
// With toggle ("--toggle", what Win+E runs) it does the same as the global shortcut: show or hide.
bool BgHandOff(const std::wstring& path, bool toggle) {
    g_instance = CreateMutexW(nullptr, FALSE, L"Local\\JoltInstancia");
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    HWND w = nullptr;
    for (int i = 0; i < 40 && !w; ++i) {  // it may be starting up
        w = FindWindowW(kWindowClass, nullptr);
        if (!w) Sleep(50);
    }
    if (!w) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cd{static_cast<ULONG_PTR>(toggle ? 2 : 1), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(path.c_str())};
    SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 3000, nullptr);
    return true;
}

// "jolt.exe --quit": really closes the open instance and waits for it to finish.
void BgSendQuit() {
    HWND w = FindWindowW(kWindowClass, nullptr);
    if (!w) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(w, WM_APP_QUIT, 0, 0);
    if (p) {
        WaitForSingleObject(p, 8000);
        CloseHandle(p);
    }
}

// ---- show and hide

bool BgIsUp() {
    if (g_test) return g_testShown;
    return IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd) && GetForegroundWindow() == g_hwnd;
}

void BgShow() {
    if (T().has) Enqueue(T().path, JobKind::Revalidate);  // it may have changed while hidden
    Invalidate();
    if (g_test) {
        g_testShown = true;
        TestLog(L"window: shown");
        return;
    }
    if (g_showMaxOnce) ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    else ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    g_showMaxOnce = false;
    SetForegroundWindow(g_hwnd);
}

void BgHide() {
    PopupClose();
    if (g_pal.open) PaletteClose();
    InstPanelClose();
    if (g_test) {
        g_testShown = false;
        TestLog(L"window: hidden");
        return;
    }
    ShowWindow(g_hwnd, SW_HIDE);
    SaveSession();  // in case Windows shuts down or Jolt hangs
    if (g_dsAny) SaveDirSizes();
}

void BgQuit() {
    g_quitting = true;
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// Opens a folder: the tab that already has it, or a new one.
void BgOpenPath(const std::wstring& raw) {
    const std::wstring path = Normalize(raw);
    if (!FolderExists(path)) return;
    for (int i : ActiveTabs())
        if (Key(Normalize(g_tabs[i]->path)) == Key(path)) {
            SelectTab(i);
            return;
        }
    NewTab(path);
}

// The folder of the File Explorer window in front (of its active tab). Empty if it is not a real folder.
std::wstring ExplorerFolder(HWND fg) {
    wchar_t cls[64] = {};
    if (!fg || !GetClassNameW(fg, cls, 64) || (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)) return L"";
    ComPtr<IShellWindows> wins;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wins)))) return L"";
    long n = 0;
    wins->get_Count(&n);
    const HWND tab = FindWindowExW(fg, nullptr, L"ShellTabWindowClass", nullptr);
    std::wstring first;
    for (long i = 0; i < n; ++i) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        ComPtr<IDispatch> d;
        if (FAILED(wins->Item(v, &d)) || !d) continue;
        ComPtr<IWebBrowserApp> app;
        SHANDLE_PTR h = 0;
        if (FAILED(d.As(&app)) || FAILED(app->get_HWND(&h)) || reinterpret_cast<HWND>(h) != fg) continue;
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView> fv;
        ComPtr<IPersistFolder2> pf;
        if (FAILED(d.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv)) || FAILED(fv->GetFolder(IID_PPV_ARGS(&pf))))
            continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        wchar_t path[MAX_PATH] = {};
        const bool real = SUCCEEDED(pf->GetCurFolder(&pidl)) && SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        HWND self = nullptr;
        browser->GetWindow(&self);
        if (self == tab) return real ? path : L"";
        if (first.empty() && real) first = path;
    }
    return first;
}

// The global shortcut and the tray click: shows up, and if it was already in front, hides.
void BgToggle(bool fromHotkey) {
    if (BgIsUp()) {
        BgHide();
        return;
    }
    const std::wstring folder = fromHotkey ? ExplorerFolder(GetForegroundWindow()) : L"";
    if (!folder.empty()) BgOpenPath(folder);
    BgShow();
}

// ---- global shortcut and tray

bool ParseGlobalKey(const std::wstring& spec, UINT& fsMods, UINT& vk) {
    std::wstring s = Lower(Trim(spec));
    fsMods = MOD_NOREPEAT;
    for (const wchar_t* w : {L"windows+", L"win+"}) {
        const size_t at = s.find(w);
        if (at != std::wstring::npos) {
            s.erase(at, wcslen(w));
            fsMods |= MOD_WIN;
        }
    }
    UINT m = 0;
    if (!ParseKey(s, m, vk)) return false;
    if (m & ModCtrl) fsMods |= MOD_CONTROL;
    if (m & ModShift) fsMods |= MOD_SHIFT;
    if (m & ModAlt) fsMods |= MOD_ALT;
    return true;
}

void TrayAdd() {
    NOTIFYICONDATAW n{sizeof(n)};
    n.hWnd = g_hwnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wcscpy_s(n.szTip, L"Jolt");
    g_trayOn = Shell_NotifyIconW(NIM_ADD, &n) != FALSE;
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}

void TrayRemove() {
    if (!g_trayOn) return;
    NOTIFYICONDATAW n{sizeof(n)};
    n.hWnd = g_hwnd;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
    g_trayOn = false;
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kTrayShow, L"Open Jolt");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kTrayQuit, L"Quit");
    SetMenuDefaultItem(m, kTrayShow, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);  // otherwise the menu does not close when clicking outside
    const UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (id == kTrayShow) BgShow();
    if (id == kTrayQuit) BgQuit();
}

// Applies the shortcut and tray settings. Called at startup and when the config reloads.
void BgApplyConfig() {
    UINT mods = 0, vk = 0;
    const bool parsed = !g_cfg.globalKey.empty() && ParseGlobalKey(g_cfg.globalKey, mods, vk);
    if (g_test) {
        g_hotkeyOk = parsed;
        return;
    }
    UnregisterHotKey(g_hwnd, kHotkeyId);
    g_hotkeyOk = parsed && RegisterHotKey(g_hwnd, kHotkeyId, mods, vk);
    if (!g_cfg.globalKey.empty() && !g_hotkeyOk) S.message = L"The global shortcut " + g_cfg.globalKey + L" is not available";
    const bool wantTray = g_cfg.background && g_cfg.tray;
    if (wantTray && !g_trayOn) TrayAdd();
    if (!wantTray) TrayRemove();
}

// Windows announces new drives to all windows, even hidden ones.
void BgDeviceChange(WPARAM wp, LPARAM lp) {
    if (!g_cfg.openDrives || wp != DBT_DEVICEARRIVAL) return;
    const auto* h = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
    if (!h || h->dbch_devicetype != DBT_DEVTYP_VOLUME) return;
    const DWORD mask = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp)->dbcv_unitmask;
    bool any = false;
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            BgOpenPath(std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\");
            any = true;
        }
    if (any) BgShow();
}

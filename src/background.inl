// Jolt always ready: a single instance that hides instead of closing and comes back instantly with a global
// shortcut, the tray, or when another process asks it to open a folder.

constexpr UINT WM_APP_TRAY = WM_APP + 40, WM_APP_QUIT = WM_APP + 41;
constexpr int kHotkeyId = 1;
constexpr UINT kTrayShow = 1, kTrayQuit = 2;
constexpr wchar_t kWindowClass[] = L"JoltWindow";
constexpr wchar_t kAdminClass[] = L"JoltAdminWindow";  // the window that runs as administrator: see OpenAsAdmin

HANDLE g_instance = nullptr;
bool g_quitting = false;    // really close, not hide
bool g_showMaxOnce = false;  // started hidden and the window was maximized
bool g_testShown = true;     // in tests the window is never visible: it is tracked here
bool g_hotkeyOk = false, g_trayOn = false;
UINT g_taskbarCreated = 0;
std::wstring g_relaunch, g_relaunchArgs;  // on quit, open this (after installing)
constexpr ULONG_PTR kAdminHide = 4, kAdminBehind = 5, kAdminFront = 6;  // what the normal window tells the administrator one
bool g_adminWasFront = false;  // it was the one in front when the shortcut hid them
bool g_testAdminLink = false, g_testAdminFront = false;  // a test leaves the real one alone unless it asks

void TestLog(const std::wstring& s);
bool FolderExists(const std::wstring& p);
void SaveSession();
void InstPanelClose();
void RenameManyClose();
std::wstring ExePath();  // install.inl

// ---- single instance

// If a Jolt already exists, passes it the folder and returns true. The starting process may come to the front; it hands that over.
// With toggle ("--toggle", what Win+E runs) it does the same as the global shortcut: show or hide.
// With select ("--select"), path is an item to leave highlighted in the folder that holds it.
bool BgHandOff(const std::wstring& path, bool toggle, bool select) {
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
    COPYDATASTRUCT cd{static_cast<ULONG_PTR>(toggle ? 2 : select ? 3 : 1), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)),
                      const_cast<wchar_t*>(path.c_str())};
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

// ---- running as administrator

bool RunningElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

// Lists folders past their permissions, those of other users or of the system included, without changing anything in
// them: it's what backup programs do, and the listing already opens folders that way.
void EnableBackupRight() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
}

// The administrator window opens a little to the side of the normal one: exactly over it, it would look as if that
// one had been replaced. Same size, cut where it would leave the screen. False if there's no window to go by.
bool BgAdminPlace(int show) {
    HWND normal = FindWindowW(kWindowClass, nullptr);
    RECT r;
    if (!normal || !IsWindowVisible(normal) || IsIconic(normal) || !GetWindowRect(normal, &r)) return false;
    const int gap = MulDiv(50, g_dpi, 96);
    OffsetRect(&r, gap, gap);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    r.right = std::min(r.right, mi.rcWork.right);
    r.bottom = std::min(r.bottom, mi.rcWork.bottom);
    SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_hwnd, show);
    return true;
}

bool RunsAsAdmin(const std::wstring& full, bool followLink = true);

// Opens a folder in a separate Jolt that runs as administrator: a window of its own, passing through, that closes for
// real and leaves the session alone. A second folder goes to the same window, without asking again.
void OpenAsAdmin(const std::wstring& path) {
    if (g_elevated) {
        S.message = L"This window already runs as administrator";
        Invalidate();
        return;
    }
    if (g_test) {  // the prompt is for a person
        TestLog(L"as administrator: " + path);
        return;
    }
    if (HWND w = FindWindowW(kAdminClass, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cd{1, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(path.c_str())};
        if (SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 3000, nullptr)) return;
    }
    const std::wstring exe = ExePath();
    std::wstring args = L"--elevated";
    if (!path.empty()) args += L" \"" + path + (path.back() == L'\\' ? L"\\\"" : L"\"");  // \" would be a quote, not the end
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD e = GetLastError();
        S.message = e == ERROR_CANCELLED ? L"Cancelled: the folder stays closed" : L"Couldn't open it as administrator: " + ErrorText(e);
        Invalidate();
    }
}

// Does Windows know how to run this file as administrator? Programs and scripts; and a shortcut to one, which runs
// what it points to.
bool RunsAsAdmin(const std::wstring& full, bool followLink) {
    const wchar_t* ext = PathFindExtensionW(full.c_str());
    if (!*ext) return false;
    if (!_wcsicmp(ext, L".lnk")) {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        wchar_t to[4096] = L"";
        if (!followLink || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&file)) || FAILED(file->Load(full.c_str(), STGM_READ)) || FAILED(link->GetPath(to, 4096, nullptr, 0)))
            return false;
        if (!to[0]) return true;  // to an app with no file of its own: Windows knows
        const DWORD attrs = GetFileAttributesW(to);
        return (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) && RunsAsAdmin(to, false);
    }
    DWORD n = 0;  // only whether there is one: its length
    const HRESULT hr = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_COMMAND, ext, L"runas", nullptr, &n);
    return hr == S_FALSE && n > 1;
}

// Runs a file as administrator: Windows asks first. It starts in the file's own folder, like opening it does.
void RunAsAdmin(const std::wstring& full) {
    if (g_test) {  // the prompt is for a person
        TestLog(L"run as administrator: " + full);
        return;
    }
    const std::wstring dir = ParentOf(full);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = full.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        AddRecent(full);
        HeatUseFile(full);
        return;
    }
    const DWORD e = GetLastError();
    S.message = e == ERROR_CANCELLED ? L"Cancelled: " + LeafOf(full) + L" wasn't run"
              : e == ERROR_NO_ASSOCIATION ? L"Windows can't run " + LeafOf(full) + L" as administrator"
              : L"Couldn't run it as administrator: " + ErrorText(e);
    Invalidate();
}

// ---- show and hide
// The window as administrator goes along with the normal one: the shortcut and Win+E hide both and bring both back. If
// only the normal one hid, Windows would bring the other forward and Jolt could never get out of the way.

HWND BgAdminWindow() {
    if (g_adminWindow || (g_test && !g_testAdminLink)) return nullptr;
    return FindWindowW(kAdminClass, nullptr);
}

void BgAdminTell(HWND w, ULONG_PTR what) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cd{what, 0, nullptr};
    SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 1000, nullptr);
}

bool BgIsUp() {
    if (g_test) return g_testShown;
    return IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd) && GetForegroundWindow() == g_hwnd;
}

// asLeft: the shortcut brings them back as they were; anything else wants the normal window in front.
void BgShow(bool asLeft = false) {
    HWND admin = BgAdminWindow();
    if (admin && IsWindowVisible(admin)) admin = nullptr;  // only what the shortcut hid
    const bool adminOnTop = admin && asLeft && g_adminWasFront;
    if (admin && !adminOnTop) BgAdminTell(admin, kAdminBehind);
    if (T().has) Enqueue(T().path, JobKind::Revalidate);  // it may have changed while hidden
    Invalidate();
    if (g_test) {
        g_testShown = true;
        TestLog(L"window: shown");
    } else {
        if (g_showMaxOnce) ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
        else ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
        g_showMaxOnce = false;
        SetForegroundWindow(g_hwnd);
    }
    if (adminOnTop) BgAdminTell(admin, kAdminFront);
}

void BgHide() {
    PopupClose();
    if (g_pal.open) PaletteClose();
    InstPanelClose();
    RenameManyClose();
    MediaClose();  // nothing keeps playing behind a hidden window
    if (g_test) {
        g_testShown = false;
        TestLog(L"window: hidden");
        return;
    }
    ShowWindow(g_hwnd, SW_HIDE);
    SaveSession();  // in case Windows shuts down or Jolt hangs
    if (g_dsAny && !g_adminWindow) SaveDirSizes();
}

// In the window as administrator: what the normal one asks of it.
void BgAdminOrder(ULONG_PTR what) {
    if (!g_adminWindow) return;
    if (what == kAdminHide) {
        BgHide();
    } else if (what == kAdminFront) {
        BgShow();
    } else if (what == kAdminBehind) {
        Invalidate();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
}

void BgQuit() {
    if (HWND admin = BgAdminWindow(); admin && !IsWindowVisible(admin)) BgAdminTell(admin, kAdminFront);  // hidden, nothing could bring it back
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

// The folder to open so that item ends up highlighted in it ("--select"): its parent, remembered as the
// place to put the cursor. For a drive root or something that isn't there, the item itself.
std::wstring RevealFolderOf(const std::wstring& raw) {
    const std::wstring item = Normalize(raw), folder = ParentOf(item);
    if (folder.empty() || !FolderExists(folder)) return item;
    g_lastSel[Key(folder)] = LeafOf(item);
    return folder;
}

// "--select" with Jolt already running: shows the item's folder (the tab that has it, or a new one)
// with the item highlighted, even if it was just created.
void BgRevealPath(const std::wstring& raw) {
    const std::wstring item = Normalize(raw), folder = RevealFolderOf(item);
    if (folder == item) {
        BgOpenPath(item);
        return;
    }
    for (int i : ActiveTabs())
        if (Key(Normalize(g_tabs[i]->path)) == Key(folder)) {
            SelectTab(i);
            Tab& t = *g_tabs[i];
            StopRecursive(t, false);
            t.filter.clear();
            t.pendingSelect = LeafOf(item);
            if (t.list) Rebuild(t, nullptr, t.pendingSelect);
            Enqueue(folder, JobKind::Revalidate);  // it may be new
            Invalidate();
            return;
        }
    NewTab(folder);
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
    HWND admin = BgAdminWindow();
    const bool adminInFront = admin && (GetForegroundWindow() == admin || g_testAdminFront);
    if (BgIsUp() || adminInFront) {
        if (admin && IsWindowVisible(admin) && !IsIconic(admin)) {  // first, or Windows would bring it forward
            g_adminWasFront = adminInFront;
            BgAdminTell(admin, kAdminHide);
        }
        BgHide();
        return;
    }
    const std::wstring folder = fromHotkey ? ExplorerFolder(GetForegroundWindow()) : L"";
    if (!folder.empty()) BgOpenPath(folder);
    BgShow(true);
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
    if (g_adminWindow) return;  // the shortcut and the tray are the normal window's
    UnregisterHotKey(g_hwnd, kHotkeyId);
    g_hotkeyOk = parsed && RegisterHotKey(g_hwnd, kHotkeyId, mods, vk);
    if (!g_cfg.globalKey.empty() && !g_hotkeyOk) S.message = L"The global shortcut " + g_cfg.globalKey + L" is not available";
    const bool wantTray = g_cfg.background && g_cfg.tray;
    if (wantTray && !g_trayOn) TrayAdd();
    if (!wantTray) TrayRemove();
}

// Windows announces new drives to all windows, even hidden ones.
void BgDeviceChange(WPARAM wp, LPARAM lp) {
    if (!g_cfg.openDrives || g_adminWindow || wp != DBT_DEVICEARRIVAL) return;
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

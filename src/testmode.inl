// Test mode: "jolt.exe --test script.txt [--output folder]".
// The window is never shown (it steals focus from nobody): it draws into an in-memory bitmap,
// the script's keys and clicks go straight to the window (with Ctrl/Shift/Alt simulated) and
// "capture" saves the image, with popup menus on top. It uses a copy of the configuration in
// %TEMP%\jolt-test\data, so it does not touch the user's session, favorites or anything else.
// Included from app.cpp after the other .inl files.
//
// Script, one step per line (# comments):
//   go C:\folder             navigate there         key Ctrl+Shift+F      a key with modifiers
//   type text                characters             click 300 118         click (in window DIPs)
//   double x y / right x y   double / right click   move x y              move the mouse
//   wheel x y -3             mouse wheel            command show_folders  a command by name
//   wait 300                 milliseconds           capture name.png      save what is visible
//   size 1100 700            window size            echo text             writes to log.txt
//   middle x y               middle click           popup                 logs the open menu
//   crumbs                   logs where the path segments and chevrons are, to click them
//   elevated on / off        draws as the window that runs as administrator would
//   admin_window [front]     logs the real one on this desktop, and the shortcut takes it along from then on
//
// Windows windows (with --test-desktop they stay on an invisible desktop):
//   wait_window Properties [ms]          windows                  (logs the open windows)
//   capture_window Properties | p.png    button Properties | Cancel   (UI Automation)
//   close_window Properties
// Drag and drop (as if it came from another app, or from Jolt's selection):
//   drag_over 300 140 copy C:\a.txt | C:\b.txt     drag_over 300 140 move selection
//   exists C:\a.txt (logs whether it is there)      touch C:\a.txt (appends a byte, or creates it)      message (logs the bottom notice)
//   rows (logs the names the list shows, in order)     selection (logs the selected names and the cursor)
//   cells (logs each row's [columns] show texts)        config columns | show | modified, items (sets it and reloads)
//   drop / cancel_drag             drag_out C:\target copy   (the selection dropped on another folder)
// Activation:  activate no / activate yes   (the window loses or regains focus)
// Mouse in parts:  press x y / drag x y / release x y   (move with the button held)
// Held modifiers:  hold Alt / hold none   (for wheel or clicks with Alt, Ctrl, Shift)

struct TestStep {
    std::wstring cmd, arg;
};
std::vector<TestStep> g_testSteps;
size_t g_testPos = 0;
std::wstring g_testOut;  // folder for the captures and the log
constexpr UINT_PTR kTimerTest = 9, kTimerTestKill = 10;

bool g_testBlocking = false;  // a step is waiting (UI Automation): the script does not advance
ComPtr<IDataObject> g_testDrag;
POINTL g_testDragPt{};
DWORD g_testDragKeys = 0;
double g_testWaitUntil = 0;

void TestLog(const std::wstring& s);

// The picture the preview would copy, as files next to the captures (the real clipboard is left alone):
// clip.bmp (the DIB as it goes) and clip.png when there is one.
void TestClipImage(const ClipImage& c) {
    std::wstring what = L"clipboard (test): " + std::wstring(c.v5 ? L"DIBV5 32-bit" : L"DIB 24-bit") + L" " + std::to_wstring(c.w) + L"x" + std::to_wstring(c.h);
    if (const void* p = GlobalLock(c.dib)) {
        const SIZE_T n = GlobalSize(c.dib);
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfSize = static_cast<DWORD>(sizeof(fh) + n);
        fh.bfOffBits = static_cast<DWORD>(sizeof(fh) + (c.v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER)));
        std::ofstream f(g_testOut + L"\\clip.bmp", std::ios::binary);
        f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
        f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
        GlobalUnlock(c.dib);
    }
    if (c.png)
        if (const void* p = GlobalLock(c.png)) {
            std::ofstream f(g_testOut + L"\\clip.png", std::ios::binary);
            f.write(static_cast<const char*>(p), static_cast<std::streamsize>(GlobalSize(c.png)));
            GlobalUnlock(c.png);
            what += L" + PNG";
        }
    TestLog(what);
}

void TestLog(const std::wstring& s) {
    std::wstring f = g_testOut + L"\\log.txt";
    std::ofstream out(f, std::ios::binary | std::ios::app);
    out << Utf8(s) << "\n";
}

// Prepares the test data folder: a fresh copy of the user's configuration.
std::wstring SessionRoundTrip();  // app.cpp
void ReloadConfig();              // app.cpp

void TestPrepareData() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"jolt-test";
    CreateDirectoryW(base.c_str(), nullptr);
    std::wstring real;
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        real = std::wstring(appdata) + L"\\Jolt";
        CoTaskMemFree(appdata);
    }
    g_dataDir = base + L"\\data";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    DeleteFileW((g_dataDir + L"\\session.txt").c_str());
    DeleteFileW((g_dataDir + L"\\usage.txt").c_str());
    DeleteFileW((g_dataDir + L"\\picks.txt").c_str());
    for (const wchar_t* n : {L"config.txt", L"views.txt", L"favorites.txt", L"menu.txt"})
        CopyFileW((real + L"\\" + n).c_str(), (g_dataDir + L"\\" + n).c_str(), FALSE);
    if (g_testOut.empty()) g_testOut = base + L"\\captures";
    CreateDirectoryW(g_testOut.c_str(), nullptr);
    DeleteFileW((g_testOut + L"\\log.txt").c_str());
}

bool TestLoadScript(const std::wstring& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = Trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        if (line.empty() || line[0] == L'#') continue;
        size_t sp = line.find(L' ');
        g_testSteps.push_back({Lower(line.substr(0, sp)), sp == std::wstring::npos ? L"" : line.substr(sp + 1)});
    }
    return true;
}

// Window that receives the keyboard (the one that would have focus).
HWND TestKeyTarget() {
    if (g_renaming) return g_renameEdit;
    if (g_rn.open && g_rn.edit[g_rn.focus]) return g_rn.edit[g_rn.focus];
    if (g_editing) return g_edit;
    if (g_ved.editStep >= 0 && g_ved.edit) return g_ved.edit;
    if (!g_set.editId.empty() && g_set.edit) return g_set.edit;
    if (g_at.editing >= 0 && g_at.edit) return g_at.edit;
    return g_hwnd;
}

void TestKey(const std::wstring& spec) {
    UINT mods, vk;
    if (!ParseKey(spec, mods, vk)) {
        TestLog(L"unknown key: " + spec);
        return;
    }
    g_testMods = mods;
    HWND h = TestKeyTarget();
    const bool alt = (mods & ModAlt) != 0;
    SendMessageW(h, alt ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, 1);
    // the characters TranslateMessage would generate
    wchar_t ch = 0;
    if (!(mods & (ModCtrl | ModAlt))) {
        if (vk == VK_RETURN) ch = L'\r';
        else if (vk == VK_BACK) ch = 8;
        else if (vk == VK_ESCAPE) ch = 27;
        else if (vk == VK_TAB) ch = L'\t';
        else if (vk == VK_SPACE) ch = L' ';
    }
    if (ch && IsWindow(h)) SendMessageW(h, WM_CHAR, ch, 1);
    if (IsWindow(h)) SendMessageW(h, alt ? WM_SYSKEYUP : WM_KEYUP, vk, 0xC0000001);
    g_testMods = 0;
}

int TestPx(float dip) { return static_cast<int>(std::lround(dip * g_dpi / 96.f)); }

// If the point is on a popup menu (or one is open and the click is outside), the messages go to it.
bool TestPopupTarget(float x, float y, HWND& h, LPARAM& lp) {
    if (!g_popup.open()) return false;
    POINT pt{TestPx(x), TestPx(y)};
    ClientToScreen(g_hwnd, &pt);
    for (int i = static_cast<int>(g_popup.levels.size()) - 1; i >= 0; --i) {
        const PopupLevel& L = *g_popup.levels[i];
        if (PtInRect(&L.rc, pt)) {
            h = L.hwnd;
            lp = MAKELPARAM(pt.x - L.rc.left, pt.y - L.rc.top);
            return true;
        }
    }
    const PopupLevel& L = *g_popup.levels[0];  // outside: the menu gets it (it has the capture) and closes
    h = L.hwnd;
    lp = MAKELPARAM(static_cast<short>(pt.x - L.rc.left), static_cast<short>(pt.y - L.rc.top));
    return true;
}

void TestMouse(const std::wstring& kind, const std::wstring& arg) {
    float x = 0, y = 0;
    int delta = 0;
    swscanf_s(arg.c_str(), L"%f %f %d", &x, &y, &delta);
    Render();  // clickable areas up to date
    HWND h = g_hwnd;
    LPARAM lp = MAKELPARAM(TestPx(x), TestPx(y));
    TestPopupTarget(x, y, h, lp);
    SendMessageW(h, WM_MOUSEMOVE, 0, lp);
    if (kind == L"click") {
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
    } else if (kind == L"double") {
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
        SendMessageW(h, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
    } else if (kind == L"right") {
        SendMessageW(h, WM_RBUTTONDOWN, MK_RBUTTON, lp);
        SendMessageW(h, WM_RBUTTONUP, 0, lp);
    } else if (kind == L"middle") {
        SendMessageW(h, WM_MBUTTONDOWN, MK_MBUTTON, lp);
        SendMessageW(h, WM_MBUTTONUP, 0, lp);
    } else if (kind == L"wheel") {
        POINT pt{TestPx(x), TestPx(y)};
        ClientToScreen(g_hwnd, &pt);
        SendMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta * WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
    }
}

bool SaveBitmapPng(IWICBitmapSource* bmp, const std::wstring& file) {
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(g_wic->CreateStream(&st)) || FAILED(st->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
        FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)))
        return false;
    UINT w, h;
    bmp->GetSize(&w, &h);
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    return SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}

void TestCapture(const std::wstring& name) {
    Render();
    if (g_popup.open() && g_rt) {
        // each popup menu draws into its own bitmap; they are pasted on top in place
        for (auto& L : g_popup.levels) DrawLevel(*L);
        g_rt->BeginDraw();
        for (auto& L : g_popup.levels) {
            if (!L->wic) continue;
            ComPtr<ID2D1Bitmap> b;
            if (FAILED(g_rt->CreateBitmapFromWicBitmap(L->wic.Get(), nullptr, &b))) continue;
            POINT o{L->rc.left, L->rc.top};
            ScreenToClient(g_hwnd, &o);
            const float x = o.x * 96.f / g_dpi, y = o.y * 96.f / g_dpi;
            D2D1_SIZE_F s = b->GetSize();
            for (int i = 3; i >= 1; --i) Round(D2D1::RectF(x - i, y - i + 2, x + s.width + i, y + s.height + i + 2), 6.f + i, 0x000000, 0.10f);
            g_rt->DrawBitmap(b.Get(), D2D1::RectF(x, y, x + s.width, y + s.height));
        }
        g_rt->EndDraw();
    }
    std::wstring f = g_testOut + L"\\" + name;
    bool ok = g_testBmp && SaveBitmapPng(g_testBmp.Get(), f);
    TestLog((ok ? L"captured " : L"FAILED to capture ") + f);
}

// Launcher: runs "jolt.exe --test script" on a separate Windows desktop and waits for it to finish.
int TestOnHiddenDesktop(const std::wstring& script, const std::wstring& out) {
    HDESK d = CreateDesktopW(L"JoltTest", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!d) return 2;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --test \"" + script + L"\"";
    if (!out.empty()) cmd += L" --output \"" + out + L"\"";
    STARTUPINFOW si{sizeof(si)};
    wchar_t desk[] = L"JoltTest";
    si.lpDesktop = desk;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseDesktop(d);
        return 3;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 200000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 4);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseDesktop(d);
    return static_cast<int>(code);
}

// ---- Windows windows
struct TestWinFind {
    std::wstring part;
    HWND found = nullptr;
    std::vector<std::wstring>* list = nullptr;
};
BOOL CALLBACK TestEnumWin(HWND h, LPARAM lp) {
    auto& c = *reinterpret_cast<TestWinFind*>(lp);
    if (h == g_hwnd || !IsWindowVisible(h)) return TRUE;
    wchar_t title[256] = {}, cls[128] = {};
    GetWindowTextW(h, title, 256);
    GetClassNameW(h, cls, 128);
    if (c.list) c.list->push_back(std::wstring(title) + L"  [" + cls + L"]");
    if (!c.part.empty() && title[0] && Lower(title).find(c.part) != std::wstring::npos) {
        c.found = h;
        return c.list != nullptr;  // when listing, keep going
    }
    return TRUE;
}
HWND TestFindWindow(const std::wstring& part) {
    TestWinFind c{Lower(Trim(part))};
    EnumWindows(TestEnumWin, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

// "a | b" -> ("a", "b")
std::pair<std::wstring, std::wstring> TestSplitBar(const std::wstring& s) {
    size_t b = s.find(L'|');
    if (b == std::wstring::npos) return {Trim(s), L""};
    return {Trim(s.substr(0, b)), Trim(s.substr(b + 1))};
}

void TestCaptureWindow(HWND h, const std::wstring& name) {
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, w, hh);
    HGDIOBJ old = SelectObject(mdc, bm);
    PrintWindow(h, mdc, 0);
    SelectObject(mdc, old);
    ComPtr<IWICBitmap> wb;
    bool ok = SUCCEEDED(g_wic->CreateBitmapFromHBITMAP(bm, nullptr, WICBitmapIgnoreAlpha, &wb)) && SaveBitmapPng(wb.Get(), g_testOut + L"\\" + name);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    TestLog((ok ? L"captured window " : L"FAILED to capture window ") + name);
}

// Finds a control by name inside a window and "presses" it (UI Automation, on another thread;
// meanwhile this window keeps handling messages, in case the control is ours).
bool TestUiaInvoke(HWND top, const std::wstring& name) {
    bool ok = false;
    std::wstring found;
    std::thread th([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IUIAutomation> ua;
            ComPtr<IUIAutomationElement> root;
            ComPtr<IUIAutomationCondition> cond;
            ComPtr<IUIAutomationElementArray> all;
            if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua))) &&
                SUCCEEDED(ua->ElementFromHandle(top, &root)) && SUCCEEDED(ua->CreateTrueCondition(&cond)) &&
                SUCCEEDED(root->FindAll(TreeScope_Descendants, cond.Get(), &all)) && all) {
                const std::wstring want = Lower(name);
                int n = 0, best = -1, bestScore = 0;
                all->get_Length(&n);
                for (int i = 0; i < n; ++i) {
                    ComPtr<IUIAutomationElement> el;
                    if (FAILED(all->GetElement(i, &el))) continue;
                    BSTR nm = nullptr;
                    el->get_CurrentName(&nm);
                    std::wstring s = nm ? Lower(StripAmp(nm)) : L"";
                    if (nm) SysFreeString(nm);
                    CONTROLTYPEID ct = 0;
                    el->get_CurrentControlType(&ct);
                    int score = s == want ? 4 : (!want.empty() && s.find(want) != std::wstring::npos) ? 2 : 0;
                    if (score && ct == UIA_ButtonControlTypeId) score += 1;
                    if (score > bestScore) {
                        bestScore = score;
                        best = i;
                    }
                }
                ComPtr<IUIAutomationElement> el;
                if (best >= 0 && SUCCEEDED(all->GetElement(best, &el))) {
                    BSTR nm = nullptr;
                    el->get_CurrentName(&nm);
                    found = nm ? nm : L"";
                    if (nm) SysFreeString(nm);
                    ComPtr<IUIAutomationInvokePattern> ip;
                    ComPtr<IUIAutomationLegacyIAccessiblePattern> lp;
                    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&ip))) && ip)
                        ok = SUCCEEDED(ip->Invoke());
                    else if (SUCCEEDED(el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&lp))) && lp)
                        ok = SUCCEEDED(lp->DoDefaultAction());
                }
            }
        }
        CoUninitialize();
    });
    HANDLE h = th.native_handle();
    g_testBlocking = true;
    while (MsgWaitForMultipleObjects(1, &h, FALSE, 15000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    th.join();
    g_testBlocking = false;
    TestLog(ok ? L"pressed: " + found : L"NOT found / could not press: " + name);
    return ok;
}

// ---- drag and drop
ComPtr<IDataObject> TestDataObjectForPaths(const std::wstring& list) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    std::wstring rest = list;
    for (;;) {
        auto [first, more] = TestSplitBar(rest);
        PIDLIST_ABSOLUTE p = nullptr;
        if (!first.empty() && SUCCEEDED(SHParseDisplayName(first.c_str(), nullptr, &p, 0, nullptr)) && p) pidls.push_back(p);
        if (more.empty()) break;
        rest = more;
    }
    ComPtr<IDataObject> d;
    ComPtr<IShellItemArray> arr;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &arr)))
        arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&d));
    for (auto p : pidls) CoTaskMemFree(p);
    return d;
}

DWORD TestDropKeys(const std::wstring& mode) {
    if (mode == L"copy") return MK_LBUTTON | MK_CONTROL;
    if (mode == L"move") return MK_LBUTTON | MK_SHIFT;
    if (mode == L"link") return MK_LBUTTON | MK_CONTROL | MK_SHIFT;
    return MK_LBUTTON;
}
std::wstring EffectText(DWORD e) {
    std::wstring s;
    if (e & DROPEFFECT_COPY) s += L"copy ";
    if (e & DROPEFFECT_MOVE) s += L"move ";
    if (e & DROPEFFECT_LINK) s += L"link ";
    return s.empty() ? L"none" : Trim(s);
}

// drag_over x y mode source
void TestDragOver(const std::wstring& arg) {
    float x = 0, y = 0;
    wchar_t mode[32] = {};
    int consumed = 0;
    swscanf_s(arg.c_str(), L"%f %f %31s %n", &x, &y, mode, 32, &consumed);
    std::wstring src = Trim(arg.substr(std::min<size_t>(consumed, arg.size())));
    if (Lower(src) == L"keep" && g_testDrag) {  // the same drag, moved to another point
        Render();
        POINT pt{TestPx(x), TestPx(y)};
        ClientToScreen(g_hwnd, &pt);
        g_testDragPt = {pt.x, pt.y};
        DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        g_dropTarget->DragOver(g_testDragKeys, g_testDragPt, &eff);
        TestLog(L"dragging over " + g_dropTarget->innerPath + L": offers " + EffectText(eff));
        return;
    }
    g_dragFiles.clear();
    if (Lower(src) == L"selection" && InArchive(T().path)) {
        ArcDragOut(g_testDrag, g_dragFiles);
    } else {
        g_testDrag = Lower(src) == L"selection" ? SelectionDataObject() : TestDataObjectForPaths(src);
        if (Lower(src) == L"selection") g_dragFiles = SelectedFullPaths(T());
    }
    if (!g_testDrag) {
        TestLog(L"drag_over: nothing to drag (" + src + L")");
        return;
    }
    Render();
    POINT pt{TestPx(x), TestPx(y)};
    ClientToScreen(g_hwnd, &pt);
    g_testDragPt = {pt.x, pt.y};
    g_testDragKeys = TestDropKeys(mode);
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    g_dropTarget->DragEnter(g_testDrag.Get(), g_testDragKeys, g_testDragPt, &eff);
    eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    g_dropTarget->DragOver(g_testDragKeys, g_testDragPt, &eff);
    TestLog(L"dragging over " + g_dropTarget->innerPath + L": offers " + EffectText(eff));
}

void TestDrop() {
    if (!g_testDrag) return;
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    std::wstring where = g_dropTarget->innerPath;
    g_dropTarget->Drop(g_testDrag.Get(), g_testDragKeys, g_testDragPt, &eff);
    g_testDrag.Reset();
    TestLog(L"dropped on " + where + L": " + EffectText(eff));
    RunPendingDrop();
}

// drag_out folder mode: Jolt's selection dropped on a folder "from another app" (its Shell IDropTarget)
void TestDragOut(const std::wstring& arg) {
    std::wstring a = Trim(arg), mode = L"normal";
    size_t sp = a.find_last_of(L' ');
    if (sp != std::wstring::npos) {
        std::wstring last = Lower(a.substr(sp + 1));
        if (last == L"copy" || last == L"move" || last == L"link" || last == L"normal") {
            mode = last;
            a = Trim(a.substr(0, sp));
        }
    }
    if (TestRefuses(a)) return;
    for (const std::wstring& p : SelectedFullPaths(T()))
        if (TestRefuses(p)) return;
    ComPtr<IDataObject> d = SelectionDataObject();
    ComPtr<IShellItem> item;
    ComPtr<IDropTarget> dt;
    if (!d || FAILED(SHCreateItemFromParsingName(a.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
        FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt)))) {
        TestLog(L"drag_out: could not prepare the drag to " + a);
        return;
    }
    DWORD keys = TestDropKeys(mode), eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    POINTL pt{0, 0};
    dt->DragEnter(d.Get(), keys, pt, &eff);
    eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    dt->DragOver(keys, pt, &eff);
    dt->Drop(d.Get(), keys, pt, &eff);
    TestLog(L"dragged out to " + a + L": " + EffectText(eff));
    if (T().recursive) RecCheckSoon();  // as after a real drag (StartDrag)
}

void TestActivate(bool on) {
    if (!on) {
        if (g_renaming) SendMessageW(g_renameEdit, WM_KILLFOCUS, 0, 0);
        if (g_editing) SendMessageW(g_edit, WM_KILLFOCUS, 0, 0);
        if (g_ved.editStep >= 0 && g_ved.edit) SendMessageW(g_ved.edit, WM_KILLFOCUS, 0, 0);
        if (!g_set.editId.empty() && g_set.edit) SendMessageW(g_set.edit, WM_KILLFOCUS, 0, 0);
        if (g_at.editing >= 0 && g_at.edit) SendMessageW(g_at.edit, WM_KILLFOCUS, 0, 0);
    }
    g_testActive = on;
    SendMessageW(g_hwnd, WM_ACTIVATE, on ? WA_ACTIVE : WA_INACTIVE, 0);
    SendMessageW(g_hwnd, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
    TestLog(on ? L"window active" : L"window inactive");
}

// Runs steps up to the next "wait" (or just one, to let the message queue run).
// The next step is scheduled BEFORE running this one: if the step opens something modal (a copy with a conflict,
// for example), the script keeps running inside that wait and can press its buttons.
void TestRunStep() {
    while (g_testPos < g_testSteps.size()) {
        const TestStep s = g_testSteps[g_testPos++];
        UINT next = 40;
        if (s.cmd == L"wait") next = static_cast<UINT>(std::max(1, _wtoi(s.arg.c_str())));
        if (s.cmd == L"echo") {
            TestLog(s.arg);
            continue;
        }
        if (s.cmd == L"quit") {
            g_testPos = g_testSteps.size();
            continue;
        }
        if (s.cmd == L"wait_window") {
            // this same step repeats until the window appears (or time runs out)
            auto [name, msTxt] = TestSplitBar(s.arg);
            if (g_testWaitUntil == 0) g_testWaitUntil = NowMs() + (msTxt.empty() ? 5000 : _wtoi(msTxt.c_str()));
            if (TestFindWindow(name)) {
                TestLog(L"appeared: " + name);
                g_testWaitUntil = 0;
            } else if (NowMs() < g_testWaitUntil) {
                --g_testPos;
                next = 50;
            } else {
                TestLog(L"did NOT appear: " + name);
                g_testWaitUntil = 0;
            }
            SetTimer(g_hwnd, kTimerTest, next, nullptr);
            return;
        }
        SetTimer(g_hwnd, kTimerTest, next, nullptr);
        if (s.cmd == L"wait") {
        } else if (s.cmd == L"windows") {
            std::vector<std::wstring> list;
            TestWinFind c;
            c.list = &list;
            EnumWindows(TestEnumWin, reinterpret_cast<LPARAM>(&c));
            TestLog(L"open windows: " + std::to_wstring(list.size()));
            for (auto& w : list) TestLog(L"  " + w);
        } else if (s.cmd == L"capture_window") {
            auto [name, file] = TestSplitBar(s.arg);
            if (HWND h = TestFindWindow(name)) TestCaptureWindow(h, file.empty() ? L"window.png" : file);
            else TestLog(L"capture_window: not found: " + name);
        } else if (s.cmd == L"button") {
            auto [name, button] = TestSplitBar(s.arg);
            if (HWND h = TestFindWindow(name)) TestUiaInvoke(h, button);
            else TestLog(L"button: window not found: " + name);
        } else if (s.cmd == L"close_window") {
            if (HWND h = TestFindWindow(s.arg)) PostMessageW(h, WM_CLOSE, 0, 0);
            else TestLog(L"close_window: not found: " + s.arg);
        } else if (s.cmd == L"exists") {
            TestLog((PathExists(s.arg) ? L"exists: " : L"does NOT exist: ") + s.arg);
        } else if (s.cmd == L"touch") {
            HANDLE h = CreateFileW(s.arg.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
            DWORD w = 0;
            if (h != INVALID_HANDLE_VALUE) WriteFile(h, "x", 1, &w, nullptr), CloseHandle(h);
        } else if (s.cmd == L"message") {
            TestLog(L"message: " + S.message);
        } else if (s.cmd == L"measure") {
            const int n = std::max(1, _wtoi(s.arg.c_str()));
            std::vector<double> ms;
            for (int i = 0; i < n; ++i) {
                const double t0 = NowMs();
                Render();
                ms.push_back(NowMs() - t0);
            }
            std::sort(ms.begin(), ms.end());
            wchar_t b[160];
            swprintf(b, 160, L"measure: first frame %.0f ms | median frame %.2f ms, p95 %.2f ms, max %.2f ms", g_firstFrameMs,
                     ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
            TestLog(b);
            for (const auto& [what, at] : g_bootMarks) {
                swprintf(b, 160, L"  %.0f ms: %s", at, what);
                TestLog(b);
            }
        } else if (s.cmd == L"x_button") {  // the X of the window
            SendMessageW(g_hwnd, WM_CLOSE, 0, 0);
        } else if (s.cmd == L"hotkey") {
            SendMessageW(g_hwnd, WM_HOTKEY, kHotkeyId, 0);
        } else if (s.cmd == L"usage") {
            std::wstring all;
            for (const HotItem& h : g_hot) all += L" | " + h.name + L" " + std::to_wstring(static_cast<int>(h.heat * 100));
            TestLog(L"usage:" + all);
        } else if (s.cmd == L"usage_set") {  // path | points | how many days ago [| file]
            const size_t bar = s.arg.find(L'|'), bar2 = s.arg.find(L'|', bar + 1), bar3 = s.arg.find(L'|', bar2 + 1);
            HeatAdd(Trim(s.arg.substr(0, bar)), _wtof(s.arg.c_str() + bar + 1), time(nullptr) - _wtoi(s.arg.c_str() + bar2 + 1) * 86400LL,
                    bar3 != std::wstring::npos && Trim(s.arg.substr(bar3 + 1)) == L"file");
            HeatRefresh();
        } else if (s.cmd == L"index_only") {  // the drive index is only this folder (and the ones above it)
            IdxTestOnly(Normalize(Trim(s.arg)));
            TestLog(L"index_only: " + std::to_wstring(g_ixCount.load()) + L" items");
        } else if (s.cmd == L"palette_paths") {  // Go to's first rows: path, and what the row shows of it (after a capture)
            std::wstring out = L"palette_paths:";
            for (size_t i = 0; i < g_pal.hits.size() && i < 8; ++i) {
                const PalItem& it = g_pal.all[g_pal.hits[i].first];
                out += L"\n  " + it.path + (it.picked ? L"  [picked]" : L"");
                const int row = static_cast<int>(i) - g_pal.first;
                if (row >= 0 && row < static_cast<int>(g_pal.shownDetail.size()) && g_pal.shownDetail[row] != it.path) out += L"  shows: " + g_pal.shownDetail[row];
            }
            TestLog(out);
        } else if (s.cmd == L"palette_pick") {  // runs the first row whose title is this, as Enter would
            for (size_t i = 0; i < g_pal.hits.size(); ++i)
                if (g_pal.all[g_pal.hits[i].first].title == Trim(s.arg)) {
                    TestLog(L"palette_pick: row " + std::to_wstring(i));
                    PaletteRun(static_cast<int>(i));
                    break;
                }
        } else if (s.cmd == L"picks") {  // what Ctrl+P remembers: text -> path (times)
            std::wstring out = L"picks:";
            for (const Pick& p : g_picks) out += L" | " + p.query + L" -> " + LeafOf(p.path) + L" (" + std::to_wstring(p.times) + L")";
            TestLog(out);
        } else if (s.cmd == L"clip") {  // what the clipboard holds, as if copied from another app (the real one stays as it is)
            g_testClip = s.arg;
            HsReadClipboard();
        } else if (s.cmd == L"checksums") {  // the checksums panel: each file's hashes, and the copied hash it matches
            if (!g_hs.open) {
                TestLog(L"checksums: closed");
            } else {
                Render();
                bool pending = false;
                const auto [mf, ma] = HsMatch(&pending);
                std::wstring out = L"checksums: " + Thousands(static_cast<int>(g_hs.files.size())) + L" files, " + Thousands(g_hs.folders) + L" folders left out";
                for (const HsFile& f : g_hs.files) {
                    out += L"\n  " + LeafOf(f.path) + (f.done ? L"" : L" (not read yet)");
                    if (!f.error.empty()) out += L" error: " + f.error;
                    for (int a = 0; a < kHsAlgs && f.done && f.error.empty(); ++a) out += L"\n    " + std::wstring(kHsNames[a]) + L" " + f.hash[a];
                }
                out += L"\n  clipboard: " + (g_hs.clip.empty() ? L"no hash" : g_hs.clip.substr(0, 12) + L"… " +
                                                (mf >= 0 ? L"matches " + std::wstring(kHsNames[ma]) + L" of " + LeafOf(g_hs.files[mf].path)
                                                         : pending ? std::wstring(L"pending") : std::wstring(L"matches none")));
                for (const HsHit& h : g_hs.hits)
                    if (h.k == HsK::Hash)
                        out += L"\n  @" + std::to_wstring(static_cast<int>((h.r.left + h.r.right) / 2)) + L"," + std::to_wstring(static_cast<int>((h.r.top + h.r.bottom) / 2)) +
                               L" copies " + kHsNames[h.alg] + L" of " + LeafOf(g_hs.files[h.file].path);
                TestLog(out);
            }
        } else if (s.cmd == L"diff") {  // the comparison of two files: what it found, and the rows it shows
            if (!g_df.open || !g_df.r) {
                TestLog(L"diff: closed" + std::wstring(g_diffBusy ? L" (comparing)" : L""));
            } else {
                Render();
                const DiffResult& r = *g_df.r;
                std::wstring out = L"diff: " + LeafOf(r.path[0]) + L" | " + LeafOf(r.path[1]) + L", kind " + std::to_wstring(r.kind);
                if (r.kind == 3) {
                    out += L", " + std::to_wstring(g_df.hunkAt.size()) + L" changes, -" + std::to_wstring(r.removed) + L" +" + std::to_wstring(r.added) + L", current " +
                           std::to_wstring(g_df.cur) + L", top row " + std::to_wstring(static_cast<int>(g_df.scroll / kDfRow)) + (g_df.all ? L", all lines" : L"");
                    const wchar_t* k = L"=~-+";
                    for (size_t i = 0; i < g_df.show.size() && i < 60; ++i) {
                        const DfShow& sh = g_df.show[i];
                        if (sh.fold) {
                            out += L"\n  [" + std::to_wstring(sh.fold) + L" equal lines]";
                            continue;
                        }
                        const DiffOp& op = r.ops[sh.op];
                        out += L"\n  " + std::wstring(1, k[op.kind]) + L" " + (op.a >= 0 ? std::to_wstring(op.a + 1) + L":" + r.lines[0][op.a] : L"-") + L" | " +
                               (op.b >= 0 ? std::to_wstring(op.b + 1) + L":" + r.lines[1][op.b] : L"-");
                    }
                } else {
                    out += L", " + std::to_wstring(r.runs.size()) + (r.moreRuns ? L"+" : L"") + L" runs, " + std::to_wstring(r.diffBytes) + L" bytes differ, first at " +
                           (r.runs.empty() ? L"-" : std::to_wstring(r.runs[0])) + L", current " + std::to_wstring(g_df.cur) + L", top row " + std::to_wstring(g_df.hexRow);
                }
                for (const DfHit& h : g_df.hits)
                    if (h.k == DfK::Fold)
                        out += L"\n  fold @" + std::to_wstring(static_cast<int>((h.r.left + h.r.right) / 2)) + L"," + std::to_wstring(static_cast<int>((h.r.top + h.r.bottom) / 2));
                TestLog(out);
            }
        } else if (s.cmd == L"diskuse") {  // the disk usage panel: the folder, and its rows by size
            if (g_du.open) Render();
            TestLog(L"disk usage: " + DuDump());
        } else if (s.cmd == L"compared") {  // the comparison of two folders: what it found, and the lines it shows
            if (g_dd.open) Render();
            TestLog(L"compared: " + DdDump());
        } else if (s.cmd == L"hint") {  // the note under "No matches"
            TestLog(L"hint: filter \"" + g_hint.filter + L"\" started " + std::to_wstring(g_hint.started) + L", subfolders " +
                    (g_hint.sub ? (g_hint.subDone ? std::to_wstring(g_hint.inSub) + (g_hint.capped ? L"+" : L"") : L"…") : L"-") + L", drives " +
                    (g_hint.drives ? (g_hint.drivesDone ? std::to_wstring(g_hint.onDrives) : L"…") : L"-"));
        } else if (s.cmd == L"favorites") {
            std::wstring all;
            for (const Favorite& v : g_favs) all += L" | " + v.name;
            TestLog(L"favorites:" + all);
        } else if (s.cmd == L"key_down" || s.cmd == L"key_up") {  // only one half of a key: held, then let go
            UINT mods = 0, vk = 0;
            if (!ParseKey(s.arg, mods, vk)) TestLog(L"unknown key: " + s.arg);
            else SendMessageW(TestKeyTarget(), s.cmd == L"key_down" ? WM_KEYDOWN : WM_KEYUP, vk, s.cmd == L"key_down" ? 1 : 0xC0000001);
        } else if (s.cmd == L"preview") {  // what the preview shows: its kind and what it found
            std::wstring out = std::wstring(L"preview") + (g_pvFloat ? L" (floating)" : L"") + (g_preview ? L" (panel)" : L"") + L": " + LeafOf(g_pv.path) + L" | ";
            const PreviewResult* r = g_pv.res.get();
            if (!r) out += g_pv.loading ? L"loading" : L"none";
            else if (r->kind == 1) {
                out += std::wstring(r->thumb ? L"thumbnail " : L"image ") + std::to_wstring(r->imgW) + L"x" + std::to_wstring(r->imgH) +
                       (r->alpha ? L", see-through" : L"") + (r->full ? L", whole size" : L"");
                if (g_pv.d2d) out += L", bitmap " + std::to_wstring(g_pv.d2d->GetPixelSize().width) + L" wide";
            }
            else if (r->kind == 2) out += L"text, " + std::to_wstring(g_pv.lines.size()) + L" lines" + (r->indented ? L", indented" : L"");
            else if (r->kind == 3) out += L"online only";
            else if (r->kind == 4) out += L"hex, " + std::to_wstring(r->bytes.size()) + L" bytes of " + std::to_wstring(r->fileSize);
            else if (r->kind == 6 && r->md) {
                int count[7] = {};
                for (const MdBlock& b : r->md->blocks) ++count[static_cast<int>(b.kind)];
                out += L"markdown, " + std::to_wstring(r->md->blocks.size()) + L" blocks: " + std::to_wstring(count[0]) + L" paragraphs, " +
                       std::to_wstring(count[1]) + L" headings, " + std::to_wstring(count[2]) + L" items, " + std::to_wstring(count[3]) +
                       L" quotes, " + std::to_wstring(count[4]) + L" code, " + std::to_wstring(count[5]) + L" rules, " +
                       std::to_wstring(count[6]) + L" tables";
            } else if (r->kind == 7) {
                out += L"font " + r->fontName + L" / " + r->fontFace + L", " + std::to_wstring(r->fontFaces) + L" faces";
            } else if (r->kind == 8) {
                out += g_pv.svg ? L"svg " + ExifNum(g_pv.svgW, 1) + L"x" + ExifNum(g_pv.svgH, 1) : std::wstring(L"svg, not drawn yet");
            } else if (r->kind == 9) {
                out += std::wstring(r->media == 2 ? L"video" : L"song") + (r->bmp ? L" with a picture" : L"") + L", " + FormatDuration(r->mDur);
                if (!r->mTitle.empty()) out += L", " + r->mTitle;
                if (!r->mArtist.empty()) out += L" by " + r->mArtist;
                if (MediaFor(r->file)) {
                    out += MediaPlaying() ? L" | playing at " : L" | paused at ";
                    out += g_media.engine ? ExifNum(g_media.engine->GetCurrentTime(), 1) + L" s" : L"-";
                    if (g_media.video) out += L", video " + std::to_wstring(g_media.w) + L"x" + std::to_wstring(g_media.h) + (g_media.shown ? L", frames shown" : L", no frame yet");
                    if (!g_media.error.empty()) out += L", error: " + g_media.error;
                }
            } else if (r->kind == 10) {
                out += L"archive, " + std::to_wstring(r->arc.size()) + L" items:";
                for (size_t i = 0; i < r->arc.size() && i < 12; ++i)
                    out += L" " + std::wstring(r->arc[i].depth * 2, L'.') + r->arc[i].name + (r->arc[i].dir ? L"/" : L"") +
                           (r->arc[i].dir ? L"" : L"(" + std::to_wstring(r->arc[i].size) + L")");
            } else if (r->kind == 5) {
                out += L"table, " + std::to_wstring(r->rows.size()) + L" rows x " + std::to_wstring(r->cols) + L" columns, numbers in";
                for (size_t c = 0; c < g_pv.colNum.size(); ++c)
                    if (g_pv.colNum[c]) out += L" " + std::to_wstring(c + 1);
            } else out += L"nothing";
            if (r && PvZoomable()) {
                const PvView v = PvViewOf(g_pv.imgBox);
                out += L" | view " + ExifNum(v.k * 100, 0) + L"%" + (g_pv.zoom > 0 ? L"" : L" (fit)") + L", turned " + std::to_wstring(g_pv.rot) +
                       L", moved " + ExifNum(g_pv.panX, 0) + L"," + ExifNum(g_pv.panY, 0);
            }
            if (r)
                for (const auto& [k, v] : r->facts) out += L" | " + k + L": " + v;
            TestLog(out);
        } else if (s.cmd == L"toggle") {
            COPYDATASTRUCT cd{2, sizeof(wchar_t), const_cast<wchar_t*>(L"")};
            SendMessageW(g_hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd));
        } else if (s.cmd == L"open_path" || s.cmd == L"select_path") {
            COPYDATASTRUCT cd{static_cast<ULONG_PTR>(s.cmd == L"select_path" ? 3 : 1), static_cast<DWORD>((s.arg.size() + 1) * sizeof(wchar_t)),
                              const_cast<wchar_t*>(s.arg.c_str())};
            SendMessageW(g_hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd));
        } else if (s.cmd == L"tabs") {
            std::wstring all;
            for (auto& t : g_tabs) all += L" | " + t->path + (t->pinned ? L" (pinned)" : L"");
            TestLog(L"tabs:" + all + L" | active " + T().path);
        } else if (s.cmd == L"disk") {
            g_cfg.openDrives = s.arg.find(L"yes") != std::wstring::npos;
            DEV_BROADCAST_VOLUME v{sizeof(v), DBT_DEVTYP_VOLUME, 0, 1u << (towupper(s.arg[0]) - L'A'), 0};
            SendMessageW(g_hwnd, WM_DEVICECHANGE, DBT_DEVICEARRIVAL, reinterpret_cast<LPARAM>(&v));
        } else if (s.cmd == L"explorer") {
            int found = 0;
            for (HWND w = FindWindowW(L"CabinetWClass", nullptr); w; w = FindWindowExW(nullptr, w, L"CabinetWClass", nullptr)) {
                TestLog(L"explorer: " + ExplorerFolder(w));
                ++found;
            }
            TestLog(L"explorer windows: " + std::to_wstring(found));
        } else if (s.cmd == L"status") {
            TestLog(std::wstring(L"status: installed ") + (InstIsInstalled() ? L"yes" : L"no") + L", path " + (InstInPath() ? L"yes" : L"no") +
                    L", default " + (InstIsDefault() ? L"yes" : L"no") + L", startup " + (InstStartsWithWindows() ? L"yes" : L"no") +
                    L", hotkey " + (g_hotkeyOk ? L"yes" : L"no") + L", visible " + (g_testShown ? L"yes" : L"no"));
        } else if (s.cmd == L"reg") {
            const size_t bar = s.arg.find(L'|');
            const std::wstring sub = Trim(s.arg.substr(0, bar)), name = bar == std::wstring::npos ? L"" : Trim(s.arg.substr(bar + 1));
            std::wstring v;
            const bool ok = RegRead(sub, name.empty() ? nullptr : name.c_str(), v);
            TestLog(L"reg " + sub + L" [" + name + L"]: " + (ok ? v : L"(does not exist)"));
        } else if (s.cmd == L"regset") {
            const size_t bar = s.arg.find(L'|');
            const size_t bar2 = s.arg.find(L'|', bar + 1);
            const std::wstring sub = Trim(s.arg.substr(0, bar)), name = Trim(s.arg.substr(bar + 1, bar2 - bar - 1)), val = Trim(s.arg.substr(bar2 + 1));
            RegWrite(sub, name.empty() ? nullptr : name.c_str(), val);
        } else if (s.cmd == L"grep") {  // the content search: term, files still to read, files read, matches
            int matches = 0;
            for (const auto& [k, h] : g_grepHits) matches += h.match;
            TestLog(L"grep: \"" + g_grepTerm + L"\" pending " + std::to_wstring(g_grepPending.load()) + L" read " + std::to_wstring(g_grepHits.size()) +
                    L" matches " + std::to_wstring(matches) + L" gen " + std::to_wstring(g_grepGen.load()));
        } else if (s.cmd == L"thumb") {  // thumb C:\\a.jpg | 128: asks for it and logs its state
            size_t bar = s.arg.find(L'|');
            const std::wstring path = Trim(s.arg.substr(0, bar));
            const int size = bar == std::wstring::npos ? 128 : _wtoi(s.arg.c_str() + bar + 1);
            const int64_t mt = ThumbMtimeOf(path);
            Thumb* th = ThumbGet(path, size, mt);
            TestLog(th ? L"thumb: ready " + std::to_wstring(th->w) + L"x" + std::to_wstring(th->h) + (th->isIcon ? L" icon" : L" image") +
                             (PathExists(ThumbFile(path, size, mt)) ? L" cached" : L" not cached")
                       : L"thumb: loading");
        } else if (s.cmd == L"layout") {  // the active folder's view: list or grid, and the grid's shape
            const Geo g = GeoFor(T());
            TestLog(g.grid ? L"layout: grid " + std::to_wstring(g.size) + L" cols " + std::to_wstring(g.cols) + L" cell " +
                                 std::to_wstring(static_cast<int>(g.cellW)) + L"x" + std::to_wstring(static_cast<int>(g.cellH)) + L" content " +
                                 std::to_wstring(static_cast<int>(ContentHeight(T())))
                           : L"layout: list");
        } else if (s.cmd == L"tags") {  // the tags of the cursor item
            std::wstring out = L"tags:";
            if (T().sel >= 0 && T().sel < Count())
                if (const auto* tg = TagsOf(RowPath(T(), T().sel)))
                    for (const std::wstring& x : *tg) out += L" " + x;
            TestLog(out);
        } else if (s.cmd == L"rows") {  // what the list shows, in order
            std::wstring names;
            for (int i = 0; i < RowCount(T()) && i < 40; ++i) {
                const Row& r = T().rows[i];
                std::wstring n = IsHeader(T(), i) ? L"[" + r.l->groups[static_cast<size_t>(-1 - r.e)].second + L"]" : RowName(T(), i);
                if (T().recursive && Key(RowDir(T(), i)) != Key(T().path))  // results: where it is, below the root
                    n = RowPath(T(), i).substr(T().path.size() + (T().path.back() == L'\\' ? 0 : 1));
                names += L" " + n;
                if (T().rows[i].closeness) names += L"(" + std::to_wstring(T().rows[i].closeness) + L"%)";
            }
            TestLog(L"rows: " + std::to_wstring(RowCount(T())) + names);
        } else if (s.cmd == L"config") {  // section | key | value, in the test copy of config.txt
            const size_t bar = s.arg.find(L'|'), bar2 = s.arg.find(L'|', bar + 1);
            ConfigSetValue(Trim(s.arg.substr(0, bar)), Trim(s.arg.substr(bar + 1, bar2 - bar - 1)), Trim(s.arg.substr(bar2 + 1)));
            ReloadConfig();
            Invalidate();
        } else if (s.cmd == L"cells") {  // the [columns] show columns of each row (fitting or not): name[a|b]
            std::wstring out;
            for (int i = 0; i < RowCount(T()) && i < 40; ++i) {
                if (IsHeader(T(), i)) continue;
                const bool drives = T().rows[i].l->path.empty();
                out += L" " + RowName(T(), i) + L"[";
                for (size_t j = 0; j < g_cfg.cols.size(); ++j) {
                    bool faint = false;
                    out += (j ? L"|" : L"") + CellText(g_cfg.cols[j], EntryAt(T(), i), drives ? RowName(T(), i) + L"\\" : RowPath(T(), i), drives, faint);
                }
                out += L"]";
            }
            TestLog(L"cells:" + out);
        } else if (s.cmd == L"miller") {  // the Miller columns: each column's folder, * the tab's, > the next one
            const MillerGeo m = MillerLayout(T());
            std::wstring out = L"miller: " + std::wstring(GeoFor(T()).miller ? L"on, " : L"off, ") + std::to_wstring(m.n) + L" columns";
            for (int col = 0; col < m.n; ++col) {
                const int i = m.first + col;
                out += L" | " + (i <= m.depth()                               ? MillerTitle(m.chain[i]) + (i == m.depth() ? L"*" : L"")
                                 : i == m.depth() + 1 && !MillerNext(T()).empty() ? L">" + MillerTitle(MillerNext(T()))
                                                                                : std::wstring(L"-"));
                if (i < m.depth()) out += g_cache.count(Key(m.chain[i])) ? L" (in memory)" : g_millerPending.count(Key(m.chain[i])) ? L" (asked)" : L" (not asked)";
            }
            TestLog(out);
        } else if (s.cmd == L"rename_preview") {  // the rename panel's rows: old -> new, with problems
            std::wstring out = L"rename_preview: " + std::to_wstring(g_rn.changing) + L" change, " + std::to_wstring(g_rn.problems) + L" problems";
            for (const RnItem& it : g_rn.items)
                out += L" | " + it.oldName + L" -> " + it.newName + (it.problem ? L" !" + std::to_wstring(it.problem) : L"");
            TestLog(out);
        } else if (s.cmd == L"selection") {
            std::wstring names;
            for (const auto& n : SelectedNames(T())) names += L" " + n;
            TestLog(L"selection: " + std::to_wstring(SelCount(T())) + names + L" | cursor " + std::to_wstring(T().sel) +
                    L" scroll " + std::to_wstring(static_cast<int>(T().scroll)));
        } else if (s.cmd == L"drag_over") {
            TestDragOver(s.arg);
        } else if (s.cmd == L"drop") {
            TestDrop();
        } else if (s.cmd == L"cancel_drag") {
            if (g_testDrag) g_dropTarget->DragLeave();
            g_testDrag.Reset();
        } else if (s.cmd == L"drag_out") {
            TestDragOut(s.arg);
        } else if (s.cmd == L"activate") {
            TestActivate(Lower(Trim(s.arg)) != L"no");
        } else if (s.cmd == L"go") {
            Navigate(T(), Normalize(s.arg), true);
        } else if (s.cmd == L"key") {
            TestKey(s.arg);
        } else if (s.cmd == L"type") {
            for (wchar_t c : s.arg) SendMessageW(TestKeyTarget(), WM_CHAR, c, 1);
        } else if (s.cmd == L"session") {  // saves it and reads it back
            TestLog(SessionRoundTrip());
        } else if (s.cmd == L"palette") {  // the Ctrl+P or Ctrl+K list: how many, where, and its first rows
            std::wstring out = L"palette: " + std::to_wstring(g_pal.hits.size()) + L" rows, sel " + std::to_wstring(g_pal.sel) + L", first " + std::to_wstring(g_pal.first) + L", index " +
                               std::to_wstring(g_ixRes.hits.size()) + L" of " + std::to_wstring(g_ixRes.total) + L":";
            for (size_t i = 0; i < g_pal.hits.size() && i < 8; ++i) out += L" | " + g_pal.all[g_pal.hits[i].first].title;
            TestLog(out);
        } else if (s.cmd == L"admin_window") {  // works with the real one on this desktop ("front": as if it had the focus)
            g_testAdminLink = true;
            g_testAdminFront = s.arg == L"front";
            HWND w = FindWindowW(kAdminClass, nullptr);
            TestLog(std::wstring(L"admin window: ") + (!w ? L"none" : IsWindowVisible(w) ? L"visible" : L"hidden"));
        } else if (s.cmd == L"elevated") {  // draws as the administrator window would
            g_elevated = s.arg != L"off";
            Invalidate();
        } else if (s.cmd == L"popup") {  // the open menu: items, the marked one, the hovered one, the scroll
            if (!g_popup.open()) {
                TestLog(L"popup: closed");
            } else {
                const PopupLevel& L = *g_popup.levels.back();
                std::wstring out = L"popup: " + std::to_wstring(L.items.size()) + L" items, mark " +
                                   (L.mark >= 0 ? L.items[L.mark].text : L"-") + L", hover " + (L.hover >= 0 ? L.items[L.hover].text : L"-") +
                                   L", scroll " + std::to_wstring(static_cast<int>(L.scroll)) + L" of " + std::to_wstring(static_cast<int>(MaxScroll(L))) + L":";
                for (size_t i = 0; i < L.items.size() && i < 12; ++i) out += L" | " + L.items[i].text;
                TestLog(out);
            }
        } else if (s.cmd == L"inuse") {  // the In use panel: title, subtitle, each holder with its button (and where it is)
            if (!g_iu.open) {
                TestLog(L"inuse: closed");
            } else {
                Render();
                std::wstring out = L"inuse: " + g_iu.title + L" | " + g_iu.sub;
                for (size_t i = 0; i < g_iu.holders.size(); ++i) {
                    const Holder& h = g_iu.holders[i];
                    bool clickable = false;
                    const std::wstring label = IuLabel(h, clickable);
                    out += L" | " + LeafOf(h.exe) + L" " + label;
                    for (const IuHit& hit : g_iu.hits)
                        if (hit.k == IuK::Act && hit.idx == static_cast<int>(i))
                            out += L" @" + std::to_wstring(static_cast<int>((hit.r.left + hit.r.right) / 2)) + L"," + std::to_wstring(static_cast<int>((hit.r.top + hit.r.bottom) / 2));
                    if (!clickable) out += L" (no button)";
                }
                for (const IuHit& hit : g_iu.hits)
                    if (hit.k == IuK::Retry)
                        out += L" | retry @" + std::to_wstring(static_cast<int>((hit.r.left + hit.r.right) / 2)) + L"," + std::to_wstring(static_cast<int>((hit.r.top + hit.r.bottom) / 2));
                TestLog(out);
            }
        } else if (s.cmd == L"crumbs") {  // where the segments and chevrons are, to click them
            Render();
            std::wstring out = L"crumbs:";
            for (const Hit& h : g_hits)
                if (h.kind == HitKind::Crumb || h.kind == HitKind::CrumbChev)
                    out += (h.kind == HitKind::Crumb ? L" | " + g_crumbPaths[h.idx] + L" @" : L" > @") + std::to_wstring(static_cast<int>((h.r.left + h.r.right) / 2)) +
                           L"," + std::to_wstring(static_cast<int>((h.r.top + h.r.bottom) / 2));
            TestLog(out);
        } else if (s.cmd == L"click" || s.cmd == L"double" || s.cmd == L"right" || s.cmd == L"middle" || s.cmd == L"wheel") {
            TestMouse(s.cmd, s.arg);
        } else if (s.cmd == L"press" || s.cmd == L"drag" || s.cmd == L"release") {
            float x = 0, y = 0;
            swscanf_s(s.arg.c_str(), L"%f %f", &x, &y);
            Render();
            const LPARAM lp = MAKELPARAM(TestPx(x), TestPx(y));
            if (s.cmd == L"press") SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, lp);
            SendMessageW(g_hwnd, s.cmd == L"press" ? WM_LBUTTONDOWN : s.cmd == L"drag" ? WM_MOUSEMOVE : WM_LBUTTONUP,
                         s.cmd == L"release" ? 0 : MK_LBUTTON, lp);
        } else if (s.cmd == L"hold") {
            const std::wstring a = Lower(s.arg);
            g_testMods = (a.find(L"ctrl") != std::wstring::npos ? 1 : 0) | (a.find(L"shift") != std::wstring::npos ? 2 : 0) |
                         (a.find(L"alt") != std::wstring::npos ? 4 : 0);
        } else if (s.cmd == L"move") {
            float x = 0, y = 0;
            swscanf_s(s.arg.c_str(), L"%f %f", &x, &y);
            Render();
            HWND h = g_hwnd;
            LPARAM lp = MAKELPARAM(TestPx(x), TestPx(y));
            TestPopupTarget(x, y, h, lp);
            SendMessageW(h, WM_MOUSEMOVE, 0, lp);
        } else if (s.cmd == L"command") {
            bool found = false;
            for (const CmdDef& c : kCommands)
                if (s.arg == c.name) {
                    RunCommand(c.cmd);
                    found = true;
                }
            for (size_t i = 0; i < g_cfg.custom.size() && !found; ++i)
                if (Lower(s.arg) == Lower(g_cfg.custom[i].name)) {
                    RunCommand(CustomCmd(i));
                    found = true;
                }
            if (!found) TestLog(L"unknown command: " + s.arg);
        } else if (s.cmd == L"capture") {
            TestCapture(s.arg.empty() ? L"capture.png" : s.arg);
        } else if (s.cmd == L"size") {
            int w = 1100, h = 700;
            swscanf_s(s.arg.c_str(), L"%d %d", &w, &h);
            SetWindowPos(g_hwnd, nullptr, 0, 0, MulDiv(w, g_dpi, 96), MulDiv(h, g_dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            TestLog(L"unknown step: " + s.cmd);
        }
        return;
    }
    TestLog(L"end");
    ExitProcess(0);
}

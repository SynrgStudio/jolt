// Modo de prueba: "jolt.exe --prueba guion.txt [--salida carpeta]".
// La ventana no se muestra nunca (no le roba el foco a nadie): se dibuja en un bitmap en memoria,
// las teclas y los clicks del guion se mandan directo a la ventana (con Ctrl/Shift/Alt simulados) y
// "captura" guarda la imagen, con los menús emergentes encima. Usa una copia de la configuración en
// %TEMP%\jolt-prueba\datos, así que no toca la sesión, los favoritos ni nada del usuario.
// Se incluye desde app.cpp al final de los .inl.
//
// Guion, un paso por línea (# comenta):
//   ir C:\carpeta            navegar ahí            tecla Ctrl+Shift+F    una tecla con modificadores
//   escribir texto           caracteres             click 300 118         click (en DIP de la ventana)
//   doble x y / derecho x y  doble click / derecho  mover x y             mover el mouse
//   rueda x y -3             rueda del mouse        comando ver_carpetas  un comando por nombre
//   esperar 300              milisegundos           captura nombre.png    guardar lo que se ve
//   tamano 1100 700          tamaño de la ventana   eco texto             anota en log.txt
//
// Ventanas de Windows (con --prueba-escritorio quedan en un escritorio invisible):
//   esperar_ventana Propiedades [ms]     ventanas                 (anota las ventanas abiertas)
//   captura_ventana Propiedades | p.png  boton Propiedades | Cancelar   (UI Automation)
//   cerrar_ventana Propiedades
// Arrastrar y soltar (como si viniera de otra app, o de la selección de Jolt):
//   arrastrar_sobre 300 140 copiar C:\a.txt | C:\b.txt     arrastrar_sobre 300 140 mover seleccion
//   existe C:\a.txt (anota si está)      tocar C:\a.txt (le agrega un byte, o lo crea)      mensaje (anota el aviso de abajo)
//   soltar / cancelar_arrastre             sacar C:\destino copiar   (la selección soltada en otra carpeta)
// Activación:  activar no / activar si   (la ventana pierde o recupera el foco)
// Mouse por partes:  apretar x y / arrastrar x y / levantar x y   (mover con el botón apretado)
// Modificadores sostenidos:  mantener Alt / mantener nada   (para rueda o clicks con Alt, Ctrl, Shift)

struct TestStep {
    std::wstring cmd, arg;
};
std::vector<TestStep> g_testSteps;
size_t g_testPos = 0;
std::wstring g_testOut;  // carpeta de las capturas y el log
constexpr UINT_PTR kTimerTest = 9, kTimerTestKill = 10;

bool g_testBlocking = false;  // un paso está esperando (UI Automation): el guion no avanza
ComPtr<IDataObject> g_testDrag;
POINTL g_testDragPt{};
DWORD g_testDragKeys = 0;
double g_testWaitUntil = 0;

void TestLog(const std::wstring& s) {
    std::wstring f = g_testOut + L"\\log.txt";
    std::ofstream out(f, std::ios::binary | std::ios::app);
    out << Utf8(s) << "\n";
}

// Prepara la carpeta de datos de prueba: una copia fresca de la configuración del usuario.
void TestPrepareData() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"jolt-prueba";
    CreateDirectoryW(base.c_str(), nullptr);
    std::wstring real;
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        real = std::wstring(appdata) + L"\\Jolt";
        CoTaskMemFree(appdata);
    }
    g_dataDir = base + L"\\datos";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    DeleteFileW((g_dataDir + L"\\sesion.txt").c_str());
    for (const wchar_t* n : {L"config.txt", L"vistas.txt", L"favoritos.txt", L"menu.txt"})
        CopyFileW((real + L"\\" + n).c_str(), (g_dataDir + L"\\" + n).c_str(), FALSE);
    if (g_testOut.empty()) g_testOut = base + L"\\capturas";
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

// Ventana que recibe el teclado (la que tendría el foco).
HWND TestKeyTarget() {
    if (g_renaming) return g_renameEdit;
    if (g_editing) return g_edit;
    if (g_ved.editStep >= 0 && g_ved.edit) return g_ved.edit;
    return g_hwnd;
}

void TestKey(const std::wstring& spec) {
    UINT mods, vk;
    if (!ParseKey(spec, mods, vk)) {
        TestLog(L"tecla desconocida: " + spec);
        return;
    }
    g_testMods = mods;
    HWND h = TestKeyTarget();
    const bool alt = (mods & ModAlt) != 0;
    SendMessageW(h, alt ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, 1);
    // los caracteres que generaría TranslateMessage
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

// Si el punto cae en un menú emergente (o hay uno abierto y el click es afuera), los mensajes van a él.
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
    const PopupLevel& L = *g_popup.levels[0];  // afuera: lo recibe el menú (tiene la captura) y se cierra
    h = L.hwnd;
    lp = MAKELPARAM(static_cast<short>(pt.x - L.rc.left), static_cast<short>(pt.y - L.rc.top));
    return true;
}

void TestMouse(const std::wstring& kind, const std::wstring& arg) {
    float x = 0, y = 0;
    int delta = 0;
    swscanf_s(arg.c_str(), L"%f %f %d", &x, &y, &delta);
    Render();  // zonas clickeables al día
    HWND h = g_hwnd;
    LPARAM lp = MAKELPARAM(TestPx(x), TestPx(y));
    TestPopupTarget(x, y, h, lp);
    SendMessageW(h, WM_MOUSEMOVE, 0, lp);
    if (kind == L"click") {
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
    } else if (kind == L"doble") {
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
        SendMessageW(h, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
        SendMessageW(h, WM_LBUTTONUP, 0, lp);
    } else if (kind == L"derecho") {
        SendMessageW(h, WM_RBUTTONDOWN, MK_RBUTTON, lp);
        SendMessageW(h, WM_RBUTTONUP, 0, lp);
    } else if (kind == L"rueda") {
        POINT pt{TestPx(x), TestPx(y)};
        ClientToScreen(g_hwnd, &pt);
        SendMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta * WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
    }
}

bool SaveBitmapPng(IWICBitmap* bmp, const std::wstring& file) {
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
        // los menús emergentes se dibujan cada uno en su bitmap; se pegan encima en su lugar
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
    TestLog((ok ? L"captura " : L"NO se pudo capturar ") + f);
}

// Lanzador: corre "jolt.exe --prueba guion" en un escritorio de Windows aparte y espera que termine.
int TestOnHiddenDesktop(const std::wstring& script, const std::wstring& out) {
    HDESK d = CreateDesktopW(L"JoltPrueba", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!d) return 2;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --prueba \"" + script + L"\"";
    if (!out.empty()) cmd += L" --salida \"" + out + L"\"";
    STARTUPINFOW si{sizeof(si)};
    wchar_t desk[] = L"JoltPrueba";
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

// ---- ventanas de Windows
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
        return c.list != nullptr;  // al listar, seguir
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
    TestLog((ok ? L"captura de ventana " : L"NO se pudo capturar la ventana ") + name);
}

// Busca un control por nombre dentro de una ventana y lo "aprieta" (UI Automation, en otro hilo;
// mientras tanto esta ventana sigue atendiendo mensajes, por si el control es nuestro).
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
    TestLog(ok ? L"apretado: " + found : L"NO se encontró / no se pudo apretar: " + name);
    return ok;
}

// ---- arrastrar y soltar
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
    if (mode == L"copiar") return MK_LBUTTON | MK_CONTROL;
    if (mode == L"mover") return MK_LBUTTON | MK_SHIFT;
    if (mode == L"vincular") return MK_LBUTTON | MK_CONTROL | MK_SHIFT;
    return MK_LBUTTON;
}
std::wstring EffectText(DWORD e) {
    std::wstring s;
    if (e & DROPEFFECT_COPY) s += L"copiar ";
    if (e & DROPEFFECT_MOVE) s += L"mover ";
    if (e & DROPEFFECT_LINK) s += L"vincular ";
    return s.empty() ? L"nada" : Trim(s);
}

// arrastrar_sobre x y modo origen
void TestDragOver(const std::wstring& arg) {
    float x = 0, y = 0;
    wchar_t mode[32] = {};
    int consumed = 0;
    swscanf_s(arg.c_str(), L"%f %f %31s %n", &x, &y, mode, 32, &consumed);
    std::wstring src = Trim(arg.substr(std::min<size_t>(consumed, arg.size())));
    g_testDrag = Lower(src) == L"seleccion" ? SelectionDataObject() : TestDataObjectForPaths(src);
    g_dragFiles.clear();
    if (Lower(src) == L"seleccion") g_dragFiles = SelectedFullPaths(T());
    if (!g_testDrag) {
        TestLog(L"arrastrar: no hay nada para arrastrar (" + src + L")");
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
    TestLog(L"arrastrando sobre " + g_dropTarget->innerPath + L": ofrece " + EffectText(eff));
}

void TestDrop() {
    if (!g_testDrag) return;
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    std::wstring where = g_dropTarget->innerPath;
    g_dropTarget->Drop(g_testDrag.Get(), g_testDragKeys, g_testDragPt, &eff);
    g_testDrag.Reset();
    TestLog(L"soltado en " + where + L": " + EffectText(eff));
    RunPendingDrop();
}

// sacar carpeta modo: la selección de Jolt soltada en una carpeta "de otra app" (su IDropTarget del Shell)
void TestDragOut(const std::wstring& arg) {
    std::wstring a = Trim(arg), mode = L"normal";
    size_t sp = a.find_last_of(L' ');
    if (sp != std::wstring::npos) {
        std::wstring last = Lower(a.substr(sp + 1));
        if (last == L"copiar" || last == L"mover" || last == L"vincular" || last == L"normal") {
            mode = last;
            a = Trim(a.substr(0, sp));
        }
    }
    ComPtr<IDataObject> d = SelectionDataObject();
    ComPtr<IShellItem> item;
    ComPtr<IDropTarget> dt;
    if (!d || FAILED(SHCreateItemFromParsingName(a.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
        FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt)))) {
        TestLog(L"sacar: no se pudo preparar el arrastre a " + a);
        return;
    }
    DWORD keys = TestDropKeys(mode), eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    POINTL pt{0, 0};
    dt->DragEnter(d.Get(), keys, pt, &eff);
    eff = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    dt->DragOver(keys, pt, &eff);
    dt->Drop(d.Get(), keys, pt, &eff);
    TestLog(L"sacado a " + a + L": " + EffectText(eff));
}

void TestActivate(bool on) {
    if (!on) {
        if (g_renaming) SendMessageW(g_renameEdit, WM_KILLFOCUS, 0, 0);
        if (g_editing) SendMessageW(g_edit, WM_KILLFOCUS, 0, 0);
        if (g_ved.editStep >= 0 && g_ved.edit) SendMessageW(g_ved.edit, WM_KILLFOCUS, 0, 0);
    }
    g_testActive = on;
    SendMessageW(g_hwnd, WM_ACTIVATE, on ? WA_ACTIVE : WA_INACTIVE, 0);
    SendMessageW(g_hwnd, on ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
    TestLog(on ? L"ventana activa" : L"ventana inactiva");
}

// Ejecuta pasos hasta el próximo "esperar" (o uno solo, para dejar correr la cola de mensajes).
// El próximo paso se programa ANTES de ejecutar este: si el paso abre algo modal (copiar con conflicto,
// por ejemplo), el guion sigue corriendo adentro de esa espera y puede apretar sus botones.
void TestRunStep() {
    while (g_testPos < g_testSteps.size()) {
        const TestStep s = g_testSteps[g_testPos++];
        UINT next = 40;
        if (s.cmd == L"esperar") next = static_cast<UINT>(std::max(1, _wtoi(s.arg.c_str())));
        if (s.cmd == L"eco") {
            TestLog(s.arg);
            continue;
        }
        if (s.cmd == L"salir") {
            g_testPos = g_testSteps.size();
            continue;
        }
        if (s.cmd == L"esperar_ventana") {
            // se repite este mismo paso hasta que aparezca la ventana (o se acabe el tiempo)
            auto [name, msTxt] = TestSplitBar(s.arg);
            if (g_testWaitUntil == 0) g_testWaitUntil = NowMs() + (msTxt.empty() ? 5000 : _wtoi(msTxt.c_str()));
            if (TestFindWindow(name)) {
                TestLog(L"apareció: " + name);
                g_testWaitUntil = 0;
            } else if (NowMs() < g_testWaitUntil) {
                --g_testPos;
                next = 50;
            } else {
                TestLog(L"NO apareció: " + name);
                g_testWaitUntil = 0;
            }
            SetTimer(g_hwnd, kTimerTest, next, nullptr);
            return;
        }
        SetTimer(g_hwnd, kTimerTest, next, nullptr);
        if (s.cmd == L"esperar") {
        } else if (s.cmd == L"ventanas") {
            std::vector<std::wstring> list;
            TestWinFind c;
            c.list = &list;
            EnumWindows(TestEnumWin, reinterpret_cast<LPARAM>(&c));
            TestLog(L"ventanas abiertas: " + std::to_wstring(list.size()));
            for (auto& w : list) TestLog(L"  " + w);
        } else if (s.cmd == L"captura_ventana") {
            auto [name, file] = TestSplitBar(s.arg);
            if (HWND h = TestFindWindow(name)) TestCaptureWindow(h, file.empty() ? L"ventana.png" : file);
            else TestLog(L"captura_ventana: no está " + name);
        } else if (s.cmd == L"boton") {
            auto [name, button] = TestSplitBar(s.arg);
            if (HWND h = TestFindWindow(name)) TestUiaInvoke(h, button);
            else TestLog(L"boton: no está la ventana " + name);
        } else if (s.cmd == L"cerrar_ventana") {
            if (HWND h = TestFindWindow(s.arg)) PostMessageW(h, WM_CLOSE, 0, 0);
            else TestLog(L"cerrar_ventana: no está " + s.arg);
        } else if (s.cmd == L"existe") {
            TestLog((PathExists(s.arg) ? L"existe: " : L"NO existe: ") + s.arg);
        } else if (s.cmd == L"tocar") {
            HANDLE h = CreateFileW(s.arg.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
            DWORD w = 0;
            if (h != INVALID_HANDLE_VALUE) WriteFile(h, "x", 1, &w, nullptr), CloseHandle(h);
        } else if (s.cmd == L"mensaje") {
            TestLog(L"mensaje: " + S.message);
        } else if (s.cmd == L"medir") {
            const int n = std::max(1, _wtoi(s.arg.c_str()));
            std::vector<double> ms;
            for (int i = 0; i < n; ++i) {
                const double t0 = NowMs();
                Render();
                ms.push_back(NowMs() - t0);
            }
            std::sort(ms.begin(), ms.end());
            wchar_t b[160];
            swprintf(b, 160, L"medir: primer cuadro %.0f ms | cuadro mediana %.2f ms, p95 %.2f ms, max %.2f ms", g_firstFrameMs,
                     ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
            TestLog(b);
            for (const auto& [what, at] : g_bootMarks) {
                swprintf(b, 160, L"  %.0f ms: %s", at, what);
                TestLog(b);
            }
        } else if (s.cmd == L"atajo") {
            SendMessageW(g_hwnd, WM_HOTKEY, kHotkeyId, 0);
        } else if (s.cmd == L"abrir_ruta") {
            COPYDATASTRUCT cd{1, static_cast<DWORD>((s.arg.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(s.arg.c_str())};
            SendMessageW(g_hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd));
        } else if (s.cmd == L"pestanas") {
            std::wstring all;
            for (auto& t : g_tabs) all += L" | " + t->path;
            TestLog(L"pestanas:" + all + L" | activa " + T().path);
        } else if (s.cmd == L"disco") {
            g_cfg.openDrives = s.arg.find(L"si") != std::wstring::npos;
            DEV_BROADCAST_VOLUME v{sizeof(v), DBT_DEVTYP_VOLUME, 0, 1u << (towupper(s.arg[0]) - L'A'), 0};
            SendMessageW(g_hwnd, WM_DEVICECHANGE, DBT_DEVICEARRIVAL, reinterpret_cast<LPARAM>(&v));
        } else if (s.cmd == L"explorador") {
            int found = 0;
            for (HWND w = FindWindowW(L"CabinetWClass", nullptr); w; w = FindWindowExW(nullptr, w, L"CabinetWClass", nullptr)) {
                TestLog(L"explorador: " + ExplorerFolder(w));
                ++found;
            }
            TestLog(L"ventanas del explorador: " + std::to_wstring(found));
        } else if (s.cmd == L"estado") {
            TestLog(std::wstring(L"estado: instalado ") + (InstIsInstalled() ? L"si" : L"no") + L", path " + (InstInPath() ? L"si" : L"no") +
                    L", predeterminado " + (InstIsDefault() ? L"si" : L"no") + L", inicio " + (InstStartsWithWindows() ? L"si" : L"no") +
                    L", atajo " + (g_hotkeyOk ? L"si" : L"no"));
        } else if (s.cmd == L"reg") {
            const size_t bar = s.arg.find(L'|');
            const std::wstring sub = Trim(s.arg.substr(0, bar)), name = bar == std::wstring::npos ? L"" : Trim(s.arg.substr(bar + 1));
            std::wstring v;
            const bool ok = RegRead(sub, name.empty() ? nullptr : name.c_str(), v);
            TestLog(L"reg " + sub + L" [" + name + L"]: " + (ok ? v : L"(no existe)"));
        } else if (s.cmd == L"regponer") {
            const size_t bar = s.arg.find(L'|');
            const size_t bar2 = s.arg.find(L'|', bar + 1);
            const std::wstring sub = Trim(s.arg.substr(0, bar)), name = Trim(s.arg.substr(bar + 1, bar2 - bar - 1)), val = Trim(s.arg.substr(bar2 + 1));
            RegWrite(sub, name.empty() ? nullptr : name.c_str(), val);
        } else if (s.cmd == L"seleccion") {
            std::wstring names;
            for (const auto& n : SelectedNames(T())) names += L" " + n;
            TestLog(L"seleccion: " + std::to_wstring(SelCount(T())) + names + L" | cursor " + std::to_wstring(T().sel) +
                    L" scroll " + std::to_wstring(static_cast<int>(T().scroll)));
        } else if (s.cmd == L"arrastrar_sobre") {
            TestDragOver(s.arg);
        } else if (s.cmd == L"soltar") {
            TestDrop();
        } else if (s.cmd == L"cancelar_arrastre") {
            if (g_testDrag) g_dropTarget->DragLeave();
            g_testDrag.Reset();
        } else if (s.cmd == L"sacar") {
            TestDragOut(s.arg);
        } else if (s.cmd == L"activar") {
            TestActivate(Lower(Trim(s.arg)) != L"no");
        } else if (s.cmd == L"ir") {
            Navigate(T(), Normalize(s.arg), true);
        } else if (s.cmd == L"tecla") {
            TestKey(s.arg);
        } else if (s.cmd == L"escribir") {
            for (wchar_t c : s.arg) SendMessageW(TestKeyTarget(), WM_CHAR, c, 1);
        } else if (s.cmd == L"click" || s.cmd == L"doble" || s.cmd == L"derecho" || s.cmd == L"rueda") {
            TestMouse(s.cmd, s.arg);
        } else if (s.cmd == L"apretar" || s.cmd == L"arrastrar" || s.cmd == L"levantar") {
            float x = 0, y = 0;
            swscanf_s(s.arg.c_str(), L"%f %f", &x, &y);
            Render();
            const LPARAM lp = MAKELPARAM(TestPx(x), TestPx(y));
            if (s.cmd == L"apretar") SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, lp);
            SendMessageW(g_hwnd, s.cmd == L"apretar" ? WM_LBUTTONDOWN : s.cmd == L"arrastrar" ? WM_MOUSEMOVE : WM_LBUTTONUP,
                         s.cmd == L"levantar" ? 0 : MK_LBUTTON, lp);
        } else if (s.cmd == L"mantener") {
            const std::wstring a = Lower(s.arg);
            g_testMods = (a.find(L"ctrl") != std::wstring::npos ? 1 : 0) | (a.find(L"shift") != std::wstring::npos ? 2 : 0) |
                         (a.find(L"alt") != std::wstring::npos ? 4 : 0);
        } else if (s.cmd == L"mover") {
            float x = 0, y = 0;
            swscanf_s(s.arg.c_str(), L"%f %f", &x, &y);
            Render();
            HWND h = g_hwnd;
            LPARAM lp = MAKELPARAM(TestPx(x), TestPx(y));
            TestPopupTarget(x, y, h, lp);
            SendMessageW(h, WM_MOUSEMOVE, 0, lp);
        } else if (s.cmd == L"comando") {
            bool found = false;
            for (const CmdDef& c : kCommands)
                if (s.arg == c.name) {
                    RunCommand(c.cmd);
                    found = true;
                }
            if (!found) TestLog(L"comando desconocido: " + s.arg);
        } else if (s.cmd == L"captura") {
            TestCapture(s.arg.empty() ? L"captura.png" : s.arg);
        } else if (s.cmd == L"tamano") {
            int w = 1100, h = 700;
            swscanf_s(s.arg.c_str(), L"%d %d", &w, &h);
            SetWindowPos(g_hwnd, nullptr, 0, 0, MulDiv(w, g_dpi, 96), MulDiv(h, g_dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            TestLog(L"paso desconocido: " + s.cmd);
        }
        return;
    }
    TestLog(L"fin");
    ExitProcess(0);
}

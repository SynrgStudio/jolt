// Arrastrar y soltar. Se incluye desde app.cpp después de menu.inl.
//
// Destino: la carpeta bajo el cursor (fila, fondo, favorito, unidad o pestaña) delega en el
// IDropTarget que le da el Shell para esa carpeta: el mismo código que usa el Explorador, así que
// mover/copiar según el disco, Ctrl/Shift, conflictos, "optimized move" y deshacer se comportan igual.
// Origen: la selección se arrastra con el objeto de datos del Shell (SHDoDragDrop pone la imagen).

ComPtr<IDropTargetHelper> g_dropHelper;

// Lo que se arrastra desde Jolt y cae en Jolt no se le pasa al Shell, que no avisa qué hizo: se resuelve
// con FileOp al terminar el arrastre, así queda en el historial de Ctrl+Z.
std::vector<std::wstring> g_dragFiles;
struct PendingDrop {
    std::wstring dest;
    bool move = false, active = false;
} g_pendingDrop;

struct DropTarget : IDropTarget {
    LONG refs = 1;
    ComPtr<IDataObject> data;
    ComPtr<IDropTarget> inner;
    std::wstring innerPath;
    bool innerValid = false, rightDrag = false;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs);
        if (!r) delete this;
        return r;
    }

    // Carpeta destino bajo el punto de pantalla; "" = ninguna.
    std::wstring TargetAt(POINTL ptl, int& row, int& side, int& tab) {
        POINT pt{ptl.x, ptl.y};
        ScreenToClient(g_hwnd, &pt);
        float x = pt.x * 96.f / g_dpi, y = pt.y * 96.f / g_dpi;
        row = side = tab = -1;
        Hit h = HitAt(x, y);
        if (h.kind == HitKind::Side && h.idx < static_cast<int>(g_sidePaths.size())) {
            side = h.idx;
            return g_sidePaths[h.idx];
        }
        if (h.kind == HitKind::Tab && h.idx < static_cast<int>(g_tabs.size())) {
            tab = h.idx;
            return g_tabs[h.idx]->path;
        }
        int pane = PaneAt(x, y);
        if (pane < 0 || y < ListTop() || y >= ListBottom()) return L"";
        Tab& t = PT(pane);
        g_dropPane = pane;
        int r = HitRowIn(pane, x, y);
        if (r >= 0 && t.list && EntryAt(t, r).isDir()) {
            row = r;
            return RowPath(t, r);
        }
        return t.path;
    }

    void Bind(const std::wstring& path) {
        if (innerValid && Key(path) == Key(innerPath)) return;
        if (inner) inner->DragLeave();
        inner.Reset();
        innerPath = path;
        innerValid = true;
        if (path.empty()) return;
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
            item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&inner));
    }

    void Highlight(int row, int side, int tab) {
        if (row == g_dropRow && side == g_dropSide && tab == g_dropTab) return;
        g_dropRow = row;
        g_dropSide = side;
        g_dropTab = tab;
        Invalidate();
    }

    // Lleva el destino interno al punto actual y devuelve el efecto que ofrece.
    void Track(DWORD keys, POINTL pt, DWORD* effect, bool entering) {
        int row, side, tab;
        std::wstring path = TargetAt(pt, row, side, tab);
        bool changed = !innerValid || Key(path) != Key(innerPath);
        if (changed) {
            Bind(path);
            Log(L"drop target: \"%s\" inner=%d", path.c_str(), inner ? 1 : 0);
        }
        if (!inner)
            *effect = DROPEFFECT_NONE;
        else if (changed || entering)
            inner->DragEnter(data.Get(), keys, pt, effect);
        else
            inner->DragOver(keys, pt, effect);
        Highlight(row, side, tab);
    }

    STDMETHODIMP DragEnter(IDataObject* pdo, DWORD keys, POINTL pt, DWORD* effect) override {
        data = pdo;
        inner.Reset();
        innerValid = false;
        rightDrag = (keys & MK_RBUTTON) != 0;
        Track(keys, pt, effect, true);
        if (g_dropHelper) {
            POINT p{pt.x, pt.y};
            g_dropHelper->DragEnter(g_hwnd, pdo, &p, *effect);
        }
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        Track(keys, pt, effect, false);
        if (g_dropHelper) {
            POINT p{pt.x, pt.y};
            g_dropHelper->DragOver(&p, *effect);
        }
        return S_OK;
    }
    STDMETHODIMP DragLeave() override {
        if (inner) inner->DragLeave();
        inner.Reset();
        innerValid = false;
        data.Reset();
        Highlight(-1, -1, -1);
        if (g_dropHelper) g_dropHelper->DragLeave();
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject* pdo, DWORD keys, POINTL pt, DWORD* effect) override {
        data = pdo;
        Track(keys, pt, effect, false);
        if (g_dropHelper) {
            POINT p{pt.x, pt.y};
            g_dropHelper->Drop(pdo, &p, *effect);
        }
        if (inner && !g_dragFiles.empty() && !rightDrag && (*effect == DROPEFFECT_MOVE || *effect == DROPEFFECT_COPY) &&
            PathIsDir(innerPath)) {
            g_pendingDrop = {innerPath, *effect == DROPEFFECT_MOVE, true};
            inner->DragLeave();
            *effect = DROPEFFECT_NONE;
        } else if (inner) {
            HRESULT hr = inner->Drop(pdo, keys, pt, effect);
            Log(L"Drop hr=0x%08X effect=%u en \"%s\"", static_cast<unsigned>(hr), *effect, innerPath.c_str());
        } else {
            *effect = DROPEFFECT_NONE;
        }
        inner.Reset();
        innerValid = false;
        data.Reset();
        Highlight(-1, -1, -1);
        return S_OK;
    }
};
DropTarget* g_dropTarget = nullptr;

void InitDragDrop() {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_dropHelper));
    g_dropTarget = new DropTarget();
    RegisterDragDrop(g_hwnd, g_dropTarget);
}

// Objeto de datos del Shell con la selección (lo que se arrastra; el modo de prueba también lo usa).
ComPtr<IDataObject> SelectionDataObject() {
    Tab& t = T();
    ComPtr<IDataObject> pdo;
    if (t.path.empty() || !t.list) return pdo;
    std::vector<std::wstring> names = SelectedNames(t);
    if (names.empty()) return pdo;
    const std::wstring dir = SelDir(t);
    ComPtr<IShellFolder> psf;
    if (!BindFolder(dir, psf)) return pdo;
    PIDLIST_ABSOLUTE folderPidl = nullptr;
    SHParseDisplayName(dir.c_str(), nullptr, &folderPidl, 0, nullptr);
    std::vector<PIDLIST_RELATIVE> pidls;
    for (const std::wstring& n : names) {
        PIDLIST_RELATIVE c = nullptr;
        std::wstring nn = n;
        if (SUCCEEDED(psf->ParseDisplayName(g_hwnd, nullptr, nn.data(), nullptr, &c, nullptr)) && c) pidls.push_back(c);
    }
    if (folderPidl && !pidls.empty())
        SHCreateDataObject(folderPidl, static_cast<UINT>(pidls.size()), (PCUITEMID_CHILD_ARRAY)pidls.data(), nullptr,
                           IID_PPV_ARGS(&pdo));
    Log(L"SelectionDataObject: %zu nombres, %zu pidls, pdo=%d", names.size(), pidls.size(), pdo ? 1 : 0);
    for (auto p : pidls) CoTaskMemFree(p);
    if (folderPidl) CoTaskMemFree(folderPidl);
    return pdo;
}

// Hace lo que quedó pendiente de un arrastre dentro de Jolt.
void RunPendingDrop() {
    PendingDrop d = std::move(g_pendingDrop);
    std::vector<std::wstring> files = std::move(g_dragFiles);
    g_pendingDrop = {};
    g_dragFiles.clear();
    if (!d.active) return;
    std::vector<std::wstring> elsewhere;
    for (const auto& f : files)
        if (Key(ParentOf(f)) != Key(d.dest)) elsewhere.push_back(f);
    // copiar sobre su misma carpeta crea copias con otro nombre, como en el Explorador
    if (d.move) FileOp(FileOpKind::Move, elsewhere, d.dest);
    else FileOp(FileOpKind::Copy, files, d.dest, elsewhere.size() == files.size() ? 0 : FOF_RENAMEONCOLLISION);
    Enqueue(d.dest, JobKind::Revalidate);
    for (const auto& f : files) Enqueue(ParentOf(f), JobKind::Revalidate);
}

// Arrastra la selección actual.
void StartDrag() {
    ComPtr<IDataObject> pdo = SelectionDataObject();
    if (!pdo) return;
    ReleaseCapture();
    g_dragFiles = SelectedFullPaths(T());
    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = SHDoDragDrop(g_hwnd, pdo.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    Log(L"SHDoDragDrop hr=0x%08X effect=%u", static_cast<unsigned>(hr), effect);
    RunPendingDrop();
}

// Detección del arrastre con el mouse: un click sobre algo ya seleccionado no cambia la selección
// hasta soltar (para poder arrastrar varios); moverse más de SM_CXDRAG empieza el arrastre.
struct Press {
    bool active = false, deferred = false;
    int row = -1;
    POINT pt{};
    bool box = false;  // moverse dibuja un recuadro en vez de arrastrar archivos
} g_press;

void PressBegin(int row, POINT pt, bool deferred) {
    g_press = {true, deferred, row, pt};
    SetCapture(g_hwnd);
}

void PressEnd(bool applyDeferred) {
    if (!g_press.active) return;
    bool deferred = g_press.deferred;
    int row = g_press.row;
    g_press = {};
    if (GetCapture() == g_hwnd) ReleaseCapture();
    if (applyDeferred && deferred && row >= 0 && row < Count()) {
        SelectOnly(T(), row);
        Invalidate();
    }
}

bool PressMoved(POINT pt) {
    if (!g_press.active) return false;
    int dx = std::abs(pt.x - g_press.pt.x), dy = std::abs(pt.y - g_press.pt.y);
    return dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG);
}

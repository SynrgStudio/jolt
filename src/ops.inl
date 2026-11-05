// Operaciones de archivos y comandos rápidos. Se incluye desde app.cpp después de dragdrop.inl.

// A la papelera con IFileOperation, que dice dónde quedó cada cosa (Ctrl+Z la recupera) y respeta la
// confirmación elegida en Windows. El borrado definitivo no se puede deshacer: en una sola carpeta va
// con el verbo del Shell, que pregunta como el Explorador.
void DeleteSelection() {
    Tab& t = T();
    std::vector<std::wstring> all = SelectedFullPaths(t);
    std::vector<std::wstring> names = SelectedNames(t);
    const bool permanent = KeyDown(VK_SHIFT);
    if (all.empty() || (permanent && names.size() == all.size())) {
        ShellVerb(names, "delete");
        return;
    }
    DWORD flags = 0;
    if (!permanent) {
        flags = FOFX_RECYCLEONDELETE;
        SHELLSTATE ss{};
        SHGetSetSettings(&ss, SSF_NOCONFIRMRECYCLE, FALSE);
        if (ss.fNoConfirmRecycle) flags |= FOF_NOCONFIRMATION;
    }
    FileOp(FileOpKind::Delete, all, L"", flags);
    for (const auto& f : all) Enqueue(ParentOf(f), JobKind::Revalidate);
}

bool SetClipboardText(const std::wstring& s) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (!h) return false;
    memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    if (!OpenClipboard(g_hwnd)) {
        GlobalFree(h);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    return true;
}

// Ctrl+Shift+C: la ruta de lo seleccionado (una por línea), o la de la carpeta si no hay nada.
// Las comillas se agregan solo si hacen falta.
void CopyPathText() {
    Tab& t = T();
    std::vector<std::wstring> paths;
    for (int i = 0; i < Count(); ++i)
        if (IsSel(t, i)) paths.push_back(RowPath(t, i));
    if (paths.empty() && !t.path.empty()) paths.push_back(t.path);
    if (paths.empty()) return;
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::wstring& p = paths[i];
        bool quote = p.find_first_of(L" &()[]{}^=;!'+,`~") != std::wstring::npos;
        text += (i ? L"\r\n" : L"") + (quote ? L"\"" + p + L"\"" : p);
    }
    if (SetClipboardText(text))
        S.message = paths.size() == 1 ? L"Ruta copiada: " + paths[0] : Thousands(static_cast<int>(paths.size())) + L" rutas copiadas";
    Invalidate();
}

// Duplica lo seleccionado en su misma carpeta: "nombre - copia.ext", "nombre - copia (2).ext"...
void Duplicate() {
    Tab& t = T();
    std::vector<std::wstring> files = SelectedFullPaths(t), names;
    if (files.empty()) return;
    std::wstring firstCopy;
    // de a una carpeta destino por vez (en el árbol puede haber varias)
    std::unordered_map<std::wstring, std::pair<std::vector<std::wstring>, std::vector<std::wstring>>> byDir;
    std::vector<std::wstring> order;
    for (const std::wstring& f : files) {
        std::wstring dir = ParentOf(f), leaf = LeafOf(f), base = leaf, ext;
        DWORD a = GetFileAttributesW(ToExtendedPath(f).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            size_t dot = leaf.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) {
                base = leaf.substr(0, dot);
                ext = leaf.substr(dot);
            }
        }
        std::wstring nn = FreeName(dir, base + L" - copia", ext);
        // que dos copias del mismo lote no elijan el mismo nombre
        auto& slot = byDir[Key(dir)];
        for (int i = 2; std::find(slot.second.begin(), slot.second.end(), nn) != slot.second.end(); ++i)
            nn = FreeName(dir, base + L" - copia (" + std::to_wstring(i) + L")", ext);
        if (slot.first.empty()) order.push_back(dir);
        slot.first.push_back(f);
        slot.second.push_back(nn);
        if (firstCopy.empty()) firstCopy = Join(dir, nn.c_str());
    }
    for (const std::wstring& dir : order) {
        auto& slot = byDir[Key(dir)];
        FileOp(FileOpKind::Copy, slot.first, dir, 0, &slot.second);
        g_undoMerge = true;  // un solo Ctrl+Z para todo el duplicado
        Enqueue(dir, JobKind::Revalidate);
    }
    g_undoMerge = false;
    t.pendingSelect = firstCopy;
}

// Alt+Enter: propiedades de Windows de lo seleccionado (o de la carpeta, si no hay nada).
void Properties() {
    Tab& t = T();
    std::vector<std::wstring> names = SelectedNames(t);
    if (names.empty()) {
        if (!t.path.empty()) SHObjectProperties(g_hwnd, SHOP_FILEPATH, t.path.c_str(), nullptr);
        return;
    }
    ShellVerb(names, "properties");
}

// F5 / F6 con la vista dividida: copiar / mover la selección a la carpeta del otro panel.
void TransferToOther(bool move) {
    if (!g_split) {
        S.message = L"Abrí la vista dividida para copiar o mover al otro panel";
        Invalidate();
        return;
    }
    Tab& src = T();
    Tab& dst = PT(1 - g_act);
    if (dst.path.empty()) {
        S.message = L"El otro panel no está en una carpeta";
        Invalidate();
        return;
    }
    std::vector<std::wstring> files = SelectedFullPaths(src);
    if (files.empty()) return;
    bool same = true;
    for (const auto& f : files) same &= Key(ParentOf(f)) == Key(dst.path);
    if (same) {
        S.message = L"Ya está en esa carpeta";
        Invalidate();
        return;
    }
    if (FileOp(move ? FileOpKind::Move : FileOpKind::Copy, files, dst.path)) {
        if (files.size() == 1) dst.pendingSelect = LeafOf(files[0]);
        S.message = (move ? L"Movido a " : L"Copiado a ") + TabTitle(dst);
    }
    Enqueue(dst.path, JobKind::Revalidate);
    Enqueue(src.path, JobKind::Revalidate);
    Invalidate();
}

// Abre la carpeta que contiene lo resaltado (útil en la búsqueda y el árbol) y lo deja seleccionado.
void GoToLocation() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return;
    std::wstring dir = RowDir(t, t.sel), name = RowName(t, t.sel);
    if (Key(dir) == Key(t.path) && !FlatView(t)) return;
    g_lastSel[Key(dir)] = name;
    Navigate(t, dir, true);
}

// Desde un acceso directo (o un enlace simbólico o una junction): va a la carpeta donde está lo
// apuntado y lo deja resaltado.
void GoToRealLocation() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count() || IsHeader(t, t.sel) || RowDir(t, t.sel).empty()) return;
    const std::wstring path = RowPath(t, t.sel);
    std::wstring real;
    if (ExtOf(path.c_str(), path.size()) == L".lnk") {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        wchar_t buf[4096] = L"";
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(path.c_str(), STGM_READ)) &&
            SUCCEEDED(link->GetPath(buf, 4096, nullptr, 0)))
            real = buf;
        if (real.empty()) {
            S.message = L"Este acceso directo no apunta a un archivo";
            Invalidate();
            return;
        }
    } else {
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t buf[4096] = L"";
            if (GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED)) real = buf;
            CloseHandle(h);
        }
        if (real.rfind(L"\\\\?\\UNC\\", 0) == 0) real = L"\\\\" + real.substr(8);
        else if (real.rfind(L"\\\\?\\", 0) == 0) real.erase(0, 4);
        if (real.empty() || Key(Normalize(real)) == Key(path)) {
            S.message = L"Esto no es un acceso directo";
            Invalidate();
            return;
        }
    }
    if (GetFileAttributesW(real.c_str()) == INVALID_FILE_ATTRIBUTES) {
        S.message = L"El destino ya no existe: " + real;
        Invalidate();
        return;
    }
    real = Normalize(real);
    const std::wstring dir = ParentOf(real);
    if (dir.empty()) {
        Navigate(t, real, true);  // apunta a la raíz de un disco
        return;
    }
    const std::wstring name = real.substr(real.find_last_of(L'\\') + 1);
    if (Key(dir) == Key(t.path)) {
        int r = FindRow(t, name);
        if (r >= 0) SetSel(r);
        return;
    }
    g_lastSel[Key(dir)] = name;
    Navigate(t, dir, true);
}

// La terminal en la carpeta actual: Windows Terminal si está, y si no cmd. La carpeta va como
// directorio de trabajo ("-d ."), así no hay comillas que se rompan con rutas como C:\.
void OpenTerminal() {
    const std::wstring dir = T().path;
    if (dir.empty()) {
        S.message = L"Entrá a una carpeta para abrir la terminal";
        Invalidate();
        return;
    }
    if (g_test) {  // las pruebas no abren ventanas de verdad
        S.message = L"Terminal en " + dir;
        Invalidate();
        return;
    }
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.fMask = SEE_MASK_FLAG_NO_UI;
    si.hwnd = g_hwnd;
    si.lpFile = L"wt.exe";
    si.lpParameters = L"-d .";
    si.lpDirectory = dir.c_str();
    si.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&si)) return;
    si.lpFile = L"cmd.exe";
    si.lpParameters = nullptr;
    if (!ShellExecuteExW(&si)) {
        S.message = L"No se pudo abrir la terminal";
        Invalidate();
    }
}

// El Explorador en la carpeta actual, con lo resaltado ya seleccionado.
void OpenInExplorer() {
    Tab& t = T();
    std::wstring args;
    if (t.path.empty()) args = L"shell:MyComputerFolder";
    else if (t.sel >= 0 && t.sel < Count() && !IsHeader(t, t.sel)) args = L"/select,\"" + RowPath(t, t.sel) + L"\"";
    else args = L"\"" + t.path + L"\"";
    if (g_test) {
        S.message = L"Explorador: " + args;
        Invalidate();
        return;
    }
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// Vacía la papelera de todos los discos. Windows pide confirmación; va en otro hilo porque borrar
// muchos gigas tarda y Jolt no se tiene que trabar.
void EmptyRecycleBin() {
    SHQUERYRBINFO before{sizeof(before)};
    SHQueryRecycleBinW(nullptr, &before);
    if (before.i64NumItems == 0) {
        S.message = L"La papelera ya está vacía";
        Invalidate();
        return;
    }
    if (g_test) {  // las pruebas jamás tocan la papelera de verdad
        S.message = L"Papelera con elementos (en las pruebas no se vacía)";
        Invalidate();
        return;
    }
    const uint64_t bytes = static_cast<uint64_t>(before.i64Size);
    const HWND owner = g_hwnd;
    std::thread([owner, bytes] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SHEmptyRecycleBinW(owner, nullptr, 0);
        SHQUERYRBINFO after{sizeof(after)};
        SHQueryRecycleBinW(nullptr, &after);
        if (after.i64NumItems == 0) {  // si no, se canceló: no hay nada que avisar
            wchar_t buf[64];
            FormatSize(bytes, buf, 64);
            IdxStatus(std::wstring(L"Papelera vacía: ") + buf + L" liberados");
        }
        CoUninitialize();
    }).detach();
}

// Alt+R: calcula el tamaño de todas las carpetas de la lista; con varias seleccionadas, solo de esas.
void ComputeFolderSizes() {
    Tab& t = T();
    std::vector<std::wstring> dirs;
    if (SelCount(t) > 1)
        for (int i = 0; i < Count(); ++i)
            if (IsSel(t, i) && EntryAt(t, i).isDir() && !RowDir(t, i).empty()) dirs.push_back(RowPath(t, i));
    if (dirs.empty())
        for (int i = 0; i < Count(); ++i)
            if (EntryAt(t, i).isDir() && !RowDir(t, i).empty() && !(EntryAt(t, i).attrs & FILE_ATTRIBUTE_REPARSE_POINT))
                dirs.push_back(RowPath(t, i));
    if (dirs.empty()) return;
    DirSizeRequest(dirs);
    S.message = L"Calculando el tamaño de " + Thousands(static_cast<int>(dirs.size())) +
                (dirs.size() == 1 ? L" carpeta" : L" carpetas");
    Invalidate();
}

// --- vista dividida
void SetSplit(bool on) {
    if (on == g_split) return;
    CancelRename();
    if (on) {
        g_paneCur[0] = g_cur;
        g_split = true;
        for (auto& t : g_tabs) t->pane = 0;
        // el panel derecho arranca en la misma carpeta
        std::wstring path = T().path;
        g_tabs.push_back(std::make_unique<Tab>());
        Tab& nt = *g_tabs.back();
        nt.pane = 1;
        g_paneCur[1] = static_cast<int>(g_tabs.size()) - 1;
        g_act = 0;
        Navigate(nt, path, false);
        g_act = 1;
        g_cur = g_paneCur[1];
    } else {
        g_paneCur[g_act] = g_cur;
        g_split = false;
        for (auto& t : g_tabs) t->pane = 0;
        g_act = 0;
        g_paneCur[0] = g_paneCur[1] = g_cur;
    }
    for (auto& tp : g_tabs) ClampScroll(*tp);
    SetWatchAll();
    UpdateTitle();
    LayoutEdit();
    Invalidate();
    TouchSession();
}

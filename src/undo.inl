// Deshacer y rehacer (Ctrl+Z / Ctrl+Y). Se incluye desde app.cpp antes de menu.inl.
//
// Windows guarda su propio historial, pero no hay forma pública de pedirle que deshaga. Por eso cada
// operación avisa acá qué hizo de verdad: IFileOperationProgressSink da el nombre final de cada archivo
// (aunque un conflicto lo haya cambiado) y el elemento que quedó en la papelera.
//
// Reglas: deshacer nunca pisa nada, nunca toca lo que cambió después y lo que quita va a la papelera.

enum class FileOpKind { Copy, Move, Delete };
enum class UndoKind { Rename, Move, Copy, Create, Delete };

struct UndoItem {
    std::wstring from, to;  // Rename/Move: origen y destino. Copy/Create: to. Delete: from.
    std::wstring bin;       // dónde quedó en la papelera ($R…), si está ahí
    ULONGLONG stamp = 0, size = 0;  // Copy: cómo quedó, para no tocarlo si cambió
};
struct UndoEntry {
    UndoKind kind;
    std::vector<UndoItem> items;
};

constexpr size_t kUndoMax = 100;
std::deque<UndoEntry> g_undo, g_redo;
bool g_undoBusy = false;   // hay una operación de deshacer en curso
bool g_undoMerge = false;  // la próxima operación se suma a la última (duplicar en varias carpetas)

bool PathStamp(const std::wstring& p, ULONGLONG& stamp, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(ToExtendedPath(p).c_str(), GetFileExInfoStandard, &d)) return false;
    stamp = (ULONGLONG(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    size = (ULONGLONG(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}
bool PathExists(const std::wstring& p) {
    return !p.empty() && GetFileAttributesW(ToExtendedPath(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool PathIsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(ToExtendedPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring ItemPath(IShellItem* item) {
    std::wstring out;
    wchar_t* s = nullptr;
    if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &s)) && s) {
        out = s;
        CoTaskMemFree(s);
    }
    return out;
}

void UndoPush(UndoEntry e) {
    if (e.items.empty()) return;
    Log(L"undo: anotado tipo %d, %zu elementos, primero \"%s\" -> \"%s\"", static_cast<int>(e.kind), e.items.size(),
        e.items[0].from.c_str(), e.items[0].to.c_str());
    g_redo.clear();
    if (g_undoMerge && !g_undo.empty() && g_undo.back().kind == e.kind) {
        auto& items = g_undo.back().items;
        items.insert(items.end(), e.items.begin(), e.items.end());
        return;
    }
    g_undo.push_back(std::move(e));
    if (g_undo.size() > kUndoMax) g_undo.pop_front();
}

void UndoRecordRename(const std::wstring& from, const std::wstring& to) {
    // "archivo nuevo" entra renombrando: crear y ponerle nombre es una sola cosa
    if (!g_undo.empty() && g_undo.back().kind == UndoKind::Create && g_undo.back().items.size() == 1 &&
        Key(g_undo.back().items[0].to) == Key(from)) {
        g_undo.back().items[0].to = to;
        return;
    }
    UndoItem it;
    it.from = from;
    it.to = to;
    UndoPush({UndoKind::Rename, {it}});
}

void UndoRecordCreate(const std::wstring& path) {
    UndoItem it;
    it.to = path;
    UndoPush({UndoKind::Create, {it}});
}

// Anota lo que hace un IFileOperation. Solo cuenta lo pedido: de una carpeta copiada no interesa cada
// archivo de adentro. Lo que cayó sobre algo que ya existía (reemplazar, combinar carpetas) no se anota,
// porque deshacerlo se llevaría también lo que había antes.
struct OpRecorder : IFileOperationProgressSink {
    LONG refs = 1;
    DWORD cookie = 0;
    std::unordered_set<std::wstring> sources, existed;
    std::vector<UndoItem> moved, copied, deleted;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
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

    bool Asked(IShellItem* item) const { return sources.count(Key(ItemPath(item))) != 0; }
    void After(std::vector<UndoItem>& out, IShellItem* src, IShellItem* dest, LPCWSTR name, HRESULT hr, IShellItem* made) {
        if (FAILED(hr) || !Asked(src)) return;
        UndoItem it;
        it.from = ItemPath(src);
        it.to = ItemPath(made);
        if (it.to.empty()) {
            std::wstring leaf = name ? name : LeafOf(it.from);
            it.to = Join(ItemPath(dest), leaf.c_str());
        }
        if (!existed.count(Key(it.to))) out.push_back(std::move(it));
    }

    STDMETHODIMP StartOperations() override { return S_OK; }
    STDMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    STDMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostMoveItem(DWORD, IShellItem* s, IShellItem* d, LPCWSTR n, HRESULT hr, IShellItem* made) override {
        After(moved, s, d, n, hr, made);
        return S_OK;
    }
    STDMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostCopyItem(DWORD, IShellItem* s, IShellItem* d, LPCWSTR n, HRESULT hr, IShellItem* made) override {
        After(copied, s, d, n, hr, made);
        return S_OK;
    }
    STDMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    STDMETHODIMP PostDeleteItem(DWORD, IShellItem* s, HRESULT hr, IShellItem* made) override {
        if (SUCCEEDED(hr) && Asked(s)) {
            UndoItem it;
            it.from = ItemPath(s);
            it.bin = ItemPath(made);
            deleted.push_back(std::move(it));
        }
        return S_OK;
    }
    STDMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    STDMETHODIMP ResetTimer() override { return S_OK; }
    STDMETHODIMP PauseTimer() override { return S_OK; }
    STDMETHODIMP ResumeTimer() override { return S_OK; }

    // Se llama antes de empezar, con el disco todavía intacto: en un conflicto, Windows avisa de la copia
    // dos veces y durante la operación ya no se puede saber qué había.
    void AddSource(const std::wstring& path, const std::wstring& destDir = L"", const wchar_t* newName = nullptr) {
        sources.insert(Key(path));
        if (destDir.empty()) return;
        std::wstring leaf = newName ? newName : LeafOf(path);
        std::wstring full = Join(destDir, leaf.c_str());
        if (PathExists(full)) existed.insert(Key(full));
    }
    void Attach(IFileOperation* op) { op->Advise(this, &cookie); }

    // Después de PerformOperations. Un paso saltado en un conflicto también llega como éxito: por eso se
    // mira cómo quedó el disco.
    void Detach(IFileOperation* op) {
        op->Unadvise(cookie);
        auto drop = [](std::vector<UndoItem>& v, auto bad) { v.erase(std::remove_if(v.begin(), v.end(), bad), v.end()); };
        drop(moved, [](const UndoItem& i) { return !PathExists(i.to) || (Key(i.from) != Key(i.to) && PathExists(i.from)); });
        drop(copied, [](const UndoItem& i) { return !PathExists(i.to); });
        drop(deleted, [](const UndoItem& i) { return PathExists(i.from); });
        std::unordered_set<std::wstring> seen;  // el mismo elemento puede avisar más de una vez
        drop(moved, [&](const UndoItem& i) { return !seen.insert(Key(i.to)).second; });
        seen.clear();
        drop(copied, [&](const UndoItem& i) { return !seen.insert(Key(i.to)).second; });
        seen.clear();
        drop(deleted, [&](const UndoItem& i) { return !seen.insert(Key(i.from)).second; });
        for (UndoItem& i : copied) PathStamp(i.to, i.stamp, i.size);
    }
    void Commit() {
        std::vector<UndoItem> binned;  // sin papelera (borrado definitivo) no hay vuelta atrás
        for (UndoItem& i : deleted)
            if (!i.bin.empty()) binned.push_back(i);
        UndoPush({UndoKind::Move, moved});
        UndoPush({UndoKind::Copy, copied});
        UndoPush({UndoKind::Delete, binned});
    }
};

// Copia, mueve o borra con IFileOperation: progreso, conflictos y elevación como el Explorador.
// newNames (opcional, paralelo a files): nombre de cada copia. rec: quien la pide se queda con lo que
// pasó; si no, va al historial.
bool FileOp(FileOpKind kind, const std::vector<std::wstring>& files, const std::wstring& dest, DWORD extraFlags = 0,
            const std::vector<std::wstring>* newNames = nullptr, OpRecorder* rec = nullptr) {
    if (files.empty()) return false;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return false;
    op->SetOwnerWindow(g_hwnd);
    DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT | extraFlags;
    // FOF_ALLOWUNDO manda a la papelera: un borrado definitivo no puede llevarlo
    if (kind != FileOpKind::Delete || (extraFlags & FOFX_RECYCLEONDELETE)) flags |= FOF_ALLOWUNDO | FOFX_ADDUNDORECORD;
    if (kind == FileOpKind::Delete && (extraFlags & FOFX_RECYCLEONDELETE)) flags |= FOF_WANTNUKEWARNING;
    op->SetOperationFlags(flags);
    ComPtr<IShellItem> d;
    if (kind != FileOpKind::Delete && FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&d)))) return false;
    OpRecorder* own = rec ? nullptr : new OpRecorder();
    OpRecorder* r = rec ? rec : own;
    int added = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        ComPtr<IShellItem> src;
        if (FAILED(SHCreateItemFromParsingName(files[i].c_str(), nullptr, IID_PPV_ARGS(&src)))) continue;
        const wchar_t* nn = newNames && i < newNames->size() ? (*newNames)[i].c_str() : nullptr;
        r->AddSource(files[i], kind == FileOpKind::Delete ? L"" : dest, nn);
        if (kind == FileOpKind::Copy) op->CopyItem(src.Get(), d.Get(), nn, nullptr);
        else if (kind == FileOpKind::Move) op->MoveItem(src.Get(), d.Get(), nn, nullptr);
        else op->DeleteItem(src.Get(), nullptr);
        ++added;
    }
    bool ok = false;
    if (added) {
        r->Attach(op.Get());
        ok = SUCCEEDED(op->PerformOperations());
        r->Detach(op.Get());
        if (own) own->Commit();
    }
    if (own) own->Release();
    return ok;
}

// Recupera de la papelera con el verbo de Windows, que además borra su registro ($I…). Busca cada
// elemento en la carpeta virtual de la papelera: el que devuelve IFileOperation es el archivo $R, y ese
// no tiene el verbo. Devuelve las rutas originales que volvieron.
std::unordered_set<std::wstring> BinRestore(const std::vector<std::pair<std::wstring, std::wstring>>& items) {
    std::unordered_set<std::wstring> back;  // items: ruta en la papelera, ruta original
    ComPtr<IShellItem> bin;
    ComPtr<IEnumShellItems> en;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin))) ||
        FAILED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))))
        return back;
    std::unordered_map<std::wstring, std::wstring> wanted;
    for (const auto& [where, original] : items) wanted[Key(where)] = original;
    std::vector<std::pair<ComPtr<IShellItem>, std::wstring>> found;
    while (found.size() < wanted.size()) {
        ComPtr<IShellItem> item;
        if (en->Next(1, &item, nullptr) != S_OK) break;
        auto w = wanted.find(Key(ItemPath(item.Get())));
        if (w != wanted.end()) found.emplace_back(item, w->second);
    }
    for (auto& [it, original] : found) {
        ComPtr<IContextMenu> cm;
        if (FAILED(it->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm)))) continue;
        HMENU m = CreatePopupMenu();
        cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
        CMINVOKECOMMANDINFO ici{};
        ici.cbSize = sizeof(ici);
        ici.hwnd = g_hwnd;
        ici.lpVerb = "undelete";
        ici.nShow = SW_SHOWNORMAL;
        cm->InvokeCommand(&ici);
        DestroyMenu(m);
    }
    for (auto& [it, original] : found) {
        for (int i = 0; i < 150 && !PathExists(original); ++i) Sleep(20);  // el verbo puede terminar después
        if (PathExists(original)) back.insert(Key(original));
    }
    return back;
}

std::wstring UndoDesc(const UndoEntry& e) {
    const UndoItem& first = e.items[0];
    std::wstring what = e.items.size() == 1 ? L"«" + LeafOf(e.kind == UndoKind::Delete ? first.from : first.to) + L"»"
                                            : Thousands(static_cast<int>(e.items.size())) + L" elementos";
    switch (e.kind) {
        case UndoKind::Rename: return L"renombrar " + what;
        case UndoKind::Move: return L"mover " + what;
        case UndoKind::Copy: return L"copiar " + what;
        case UndoKind::Create: return L"crear " + what;
        case UndoKind::Delete: return L"mandar a la papelera " + what;
    }
    return what;
}

// Si una carpeta renombrada estaba desplegada, el árbol la sigue.
void TreeFollow(Tab& t, const std::wstring& from, const std::wstring& to) {
    std::wstring k = Key(from);
    if (!t.expanded.count(k)) return;
    t.expanded.erase(k);
    t.kids.erase(k);
    t.expanded[Key(to)] = to;
    LoadKid(t, Key(to), to);
}

// Aplica una entrada para atrás (undo) o para adelante. Deja en e.items lo que se pudo y en why por qué
// no se pudo lo demás; devuelve cuántos no se pudieron.
int UndoApply(UndoEntry& e, bool undo, std::wstring& why, std::vector<std::wstring>& touched, std::wstring& select,
              std::vector<std::pair<std::wstring, std::wstring>>& gone) {
    std::vector<UndoItem> done;
    int failed = 0;
    auto fail = [&](const wchar_t* reason) {
        why = reason;
        ++failed;
    };
    auto touch = [&](const std::wstring& p) { touched.push_back(ParentOf(p)); };

    if (e.kind == UndoKind::Rename || e.kind == UndoKind::Move) {
        auto srcOf = [&](const UndoItem& i) -> const std::wstring& { return undo ? i.to : i.from; };
        auto dstOf = [&](const UndoItem& i) -> const std::wstring& { return undo ? i.from : i.to; };
        std::vector<UndoItem> ready;
        for (UndoItem& i : e.items) {
            if (!PathExists(srcOf(i))) fail(L"ya no está donde quedó");
            else if (Key(srcOf(i)) != Key(dstOf(i)) && PathExists(dstOf(i))) fail(L"ya hay algo con ese nombre");
            else if (!PathExists(ParentOf(dstOf(i)))) fail(L"su carpeta ya no existe");
            else ready.push_back(i);
        }
        if (e.kind == UndoKind::Rename) {
            for (UndoItem& i : ready) {
                if (MoveFileExW(ToExtendedPath(srcOf(i)).c_str(), ToExtendedPath(dstOf(i)).c_str(), 0)) {
                    for (auto& tab : g_tabs) TreeFollow(*tab, srcOf(i), dstOf(i));
                    done.push_back(i);
                } else {
                    fail(L"está en uso");
                }
            }
        } else {
            std::unordered_map<std::wstring, std::vector<UndoItem>> byDir;  // el destino puede ser más de uno
            for (UndoItem& i : ready) byDir[ParentOf(dstOf(i))].push_back(i);
            for (auto& [dir, group] : byDir) {
                std::vector<std::wstring> srcs, names;
                for (UndoItem& i : group) {
                    srcs.push_back(srcOf(i));
                    names.push_back(LeafOf(dstOf(i)));
                }
                OpRecorder* rec = new OpRecorder();
                FileOp(FileOpKind::Move, srcs, dir, 0, &names, rec);
                rec->Release();
                for (UndoItem& i : group) {
                    if (PathExists(dstOf(i)) && !PathExists(srcOf(i))) done.push_back(i);
                    else fail(L"no se pudo mover");
                }
            }
        }
        for (UndoItem& i : done) {
            touch(i.from);
            touch(i.to);
            gone.emplace_back(srcOf(i), dstOf(i));
        }
        if (!done.empty()) select = dstOf(done[0]);
    } else {
        const bool toBin = (e.kind == UndoKind::Delete) != undo;  // deshacer crear = rehacer borrar
        auto place = [&](const UndoItem& i) -> const std::wstring& { return e.kind == UndoKind::Delete ? i.from : i.to; };
        if (toBin) {
            std::vector<UndoItem> ready;
            std::vector<std::wstring> paths;
            for (UndoItem& i : e.items) {
                const std::wstring& p = place(i);
                ULONGLONG stamp = 0, size = 0;
                if (!PathStamp(p, stamp, size)) fail(L"ya no está");
                else if (e.kind == UndoKind::Copy && (stamp != i.stamp || size != i.size)) fail(L"cambió desde entonces");
                else if (e.kind == UndoKind::Create && (PathIsDir(p) ? !PathIsDirectoryEmptyW(ToExtendedPath(p).c_str()) : size != 0))
                    fail(L"ya tiene contenido");
                else {
                    ready.push_back(i);
                    paths.push_back(p);
                }
            }
            OpRecorder* rec = new OpRecorder();
            FileOp(FileOpKind::Delete, paths, L"", FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION, nullptr, rec);
            for (UndoItem& i : ready) {
                auto it = std::find_if(rec->deleted.begin(), rec->deleted.end(),
                                       [&](const UndoItem& d) { return Key(d.from) == Key(place(i)); });
                if (it == rec->deleted.end()) {
                    fail(L"no se pudo mandar a la papelera");
                    continue;
                }
                i.bin = it->bin;  // vacío si no había papelera: Windows ya avisó que era definitivo
                touch(place(i));
                gone.emplace_back(place(i), L"");
                done.push_back(i);
            }
            rec->Release();
        } else {
            std::vector<std::pair<std::wstring, std::wstring>> wanted;
            std::vector<UndoItem> ready;
            for (UndoItem& i : e.items) {
                if (PathExists(place(i))) fail(L"ya hay algo con ese nombre");
                else if (!PathExists(ParentOf(place(i)))) fail(L"su carpeta ya no existe");
                else if (i.bin.empty() || !PathExists(i.bin)) fail(L"ya no está en la papelera");
                else {
                    wanted.emplace_back(i.bin, place(i));
                    ready.push_back(i);
                }
            }
            std::unordered_set<std::wstring> back;
            if (!wanted.empty()) back = BinRestore(wanted);
            for (UndoItem& i : ready) {
                if (!back.count(Key(place(i)))) {
                    fail(L"no se pudo recuperar de la papelera");
                    continue;
                }
                i.bin.clear();
                PathStamp(place(i), i.stamp, i.size);
                touch(place(i));
                done.push_back(i);
            }
            if (!done.empty()) select = place(done[0]);
        }
    }
    e.items = std::move(done);
    return failed;
}

// Una pestaña abierta adentro de lo que cambió de lugar lo sigue; si se fue a la papelera, sube a la
// carpeta que lo contenía.
void TabsFollow(const std::vector<std::pair<std::wstring, std::wstring>>& gone) {
    for (const auto& [src, dst] : gone) {
        const std::wstring k = Key(src) + L"\\";
        for (auto& tab : g_tabs) {
            if ((Key(tab->path) + L"\\").rfind(k, 0) != 0) continue;
            Navigate(*tab, dst.empty() ? ParentOf(src) : dst + tab->path.substr(src.size()), false);
        }
    }
}

// Ctrl+Z / Ctrl+Y.
void UndoStep(bool undo) {
    Log(L"undo: %s (historial %zu, rehacer %zu, ocupado %d)", undo ? L"deshacer" : L"rehacer", g_undo.size(), g_redo.size(), g_undoBusy);
    if (g_undoBusy) return;
    auto& from = undo ? g_undo : g_redo;
    auto& to = undo ? g_redo : g_undo;
    if (from.empty()) {
        S.message = undo ? L"No hay nada para deshacer" : L"No hay nada para rehacer";
        Invalidate();
        return;
    }
    g_undoBusy = true;
    UndoEntry e = std::move(from.back());
    from.pop_back();
    const std::wstring desc = UndoDesc(e);
    const int total = static_cast<int>(e.items.size());
    std::wstring why, select;
    std::vector<std::wstring> touched;
    std::vector<std::pair<std::wstring, std::wstring>> gone;
    const int failed = UndoApply(e, undo, why, touched, select, gone);
    const int done = total - failed;
    if (!e.items.empty()) to.push_back(std::move(e));
    TabsFollow(gone);

    const wchar_t* verb = undo ? L"Deshecho: " : L"Rehecho: ";
    if (!failed) {
        S.message = verb + desc;
    } else if (!done) {
        S.message = std::wstring(undo ? L"No se pudo deshacer " : L"No se pudo rehacer ") + desc + L": " + why;
        MessageBeep(MB_ICONWARNING);
    } else {
        S.message = verb + std::to_wstring(done) + L" de " + std::to_wstring(total) + L". El resto: " + why;
        MessageBeep(MB_ICONWARNING);
    }
    Tab& t = T();
    if (!select.empty() && Key(ParentOf(select)) == Key(t.path)) t.pendingSelect = LeafOf(select);
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const std::wstring& dir : touched) Enqueue(dir, JobKind::Revalidate);
    g_undoBusy = false;
    Invalidate();
}

// Undo and redo (Ctrl+Z / Ctrl+Y). Included from app.cpp before menu.inl.
//
// Windows keeps its own history, but there's no public way to ask it to undo. So every operation
// reports here what it really did: IFileOperationProgressSink gives the final name of each file
// (even if a conflict changed it) and the item that ended up in the Recycle Bin.
//
// Rules: undo never overwrites anything, never touches what changed later, and what it removes goes to the Recycle Bin.

enum class FileOpKind { Copy, Move, Delete };
enum class UndoKind { Rename, Move, Copy, Create, Delete };

struct UndoItem {
    std::wstring from, to;  // Rename/Move: source and destination. Copy/Create: to. Delete: from.
    std::wstring bin;       // where it ended up in the Recycle Bin ($R…), if it's there
    ULONGLONG stamp = 0, size = 0;  // Copy: its resulting state, so it's left alone if it changed
};
struct UndoEntry {
    UndoKind kind;
    std::vector<UndoItem> items;
    std::vector<std::wstring> dissolved;  // Move: folders it emptied and removed (flatten). Undo makes them again first
};

constexpr size_t kUndoMax = 100;
std::deque<UndoEntry> g_undo, g_redo;
bool g_undoBusy = false;   // an undo operation is in progress
bool g_undoMerge = false;  // the next operation joins the last one (duplicating across several folders)

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
    Log(L"undo: recorded kind %d, %zu items, first \"%s\" -> \"%s\"", static_cast<int>(e.kind), e.items.size(),
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
    // "new file" goes straight into rename: creating and naming it are one step
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

// Records what an IFileOperation does. Only what was requested counts: the individual files inside a
// copied folder don't matter. Whatever landed on something that already existed (replace, merge folders)
// isn't recorded, because undoing it would also take away what was there before.
struct OpRecorder : IFileOperationProgressSink {
    LONG refs = 1;
    DWORD cookie = 0;
    std::unordered_set<std::wstring> sources, existed;
    std::vector<UndoItem> moved, copied, deleted;
    std::vector<std::pair<std::wstring, HRESULT>> failed;  // what the engine couldn't do (nested items too), and why

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
        if (FAILED(hr)) failed.emplace_back(ItemPath(src), hr);
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
        if (FAILED(hr)) failed.emplace_back(ItemPath(s), hr);
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

    // Called before starting, while the disk is still untouched: on a conflict, Windows reports the copy
    // twice, and during the operation there's no longer any way to tell what was there.
    void AddSource(const std::wstring& path, const std::wstring& destDir = L"", const wchar_t* newName = nullptr) {
        sources.insert(Key(path));
        if (destDir.empty()) return;
        std::wstring leaf = newName ? newName : LeafOf(path);
        std::wstring full = Join(destDir, leaf.c_str());
        if (PathExists(full)) existed.insert(Key(full));
    }
    void Attach(IFileOperation* op) { op->Advise(this, &cookie); }

    // After PerformOperations. A step skipped on a conflict also arrives as a success: that's why the
    // resulting state of the disk is checked.
    void Detach(IFileOperation* op) {
        op->Unadvise(cookie);
        auto drop = [](std::vector<UndoItem>& v, auto bad) { v.erase(std::remove_if(v.begin(), v.end(), bad), v.end()); };
        drop(moved, [](const UndoItem& i) { return !PathExists(i.to) || (Key(i.from) != Key(i.to) && PathExists(i.from)); });
        drop(copied, [](const UndoItem& i) { return !PathExists(i.to); });
        drop(deleted, [](const UndoItem& i) { return PathExists(i.from); });
        std::unordered_set<std::wstring> seen;  // the same item can report more than once
        drop(moved, [&](const UndoItem& i) { return !seen.insert(Key(i.to)).second; });
        seen.clear();
        drop(copied, [&](const UndoItem& i) { return !seen.insert(Key(i.to)).second; });
        seen.clear();
        drop(deleted, [&](const UndoItem& i) { return !seen.insert(Key(i.from)).second; });
        for (UndoItem& i : copied) PathStamp(i.to, i.stamp, i.size);
    }
    void Commit() {
        std::vector<UndoItem> binned;  // without the Recycle Bin (permanent delete) there's no going back
        for (UndoItem& i : deleted)
            if (!i.bin.empty()) binned.push_back(i);
        UndoPush({UndoKind::Move, moved});
        UndoPush({UndoKind::Copy, copied});
        UndoPush({UndoKind::Delete, binned});
    }
};

void InUseAfterFailure(const std::wstring& what, const std::vector<std::wstring>& files, const std::wstring& reason, std::function<void()> retry);  // inuse.inl
std::wstring HrText(HRESULT hr);
bool FileOp(FileOpKind kind, const std::vector<std::wstring>& files, const std::wstring& dest, DWORD extraFlags, const std::vector<std::wstring>* newNames, OpRecorder* rec,
            const std::vector<std::wstring>* dests);

// What the engine couldn't do, without its dialogs (FOF_NOERRORUI): the reason on the status line, or the In use panel
// when a program holds the file, with the same operation to go again.
void OpFailed(FileOpKind kind, const std::vector<std::wstring>& files, const std::wstring& dest, DWORD extraFlags,
              const std::vector<std::wstring>* newNames, const std::vector<std::pair<std::wstring, HRESULT>>& failed, const std::vector<std::wstring>* dests) {
    std::vector<std::wstring> bad;
    HRESULT first = S_OK;
    for (const auto& [path, hr] : failed) {
        if (hr == COPYENGINE_E_USER_CANCELLED || hr == COPYENGINE_E_CANCELLED || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
            hr == COPYENGINE_E_REQUIRES_ELEVATION)
            continue;
        bad.push_back(path);
        if (first == S_OK) first = hr;
    }
    if (bad.empty()) return;
    const wchar_t* verb = kind == FileOpKind::Copy ? L"copy " : kind == FileOpKind::Move ? L"move " : L"delete ";
    const std::wstring what = L"Couldn't " + std::wstring(verb) + (bad.size() == 1 ? LeafOf(bad[0]) : Thousands(static_cast<int>(bad.size())) + L" items");
    const std::vector<std::wstring> again = files, names = newNames ? *newNames : std::vector<std::wstring>{}, where = dests ? *dests : std::vector<std::wstring>{};
    const bool hasNames = newNames != nullptr, hasDests = dests != nullptr;
    InUseAfterFailure(what, bad, HrText(first), [=] { FileOp(kind, again, dest, extraFlags, hasNames ? &names : nullptr, nullptr, hasDests ? &where : nullptr); });
}

// Copies, moves or deletes with IFileOperation: progress, conflicts and elevation like File Explorer. Errors don't get
// its dialogs: OpFailed says why, and offers to close whatever holds a file.
// newNames (optional, parallel to files): the name of each copy. rec: the caller keeps what
// happened; otherwise it goes to the history. dests (optional, parallel to files): the folder each one goes to, instead
// of dest.
bool FileOp(FileOpKind kind, const std::vector<std::wstring>& files, const std::wstring& dest, DWORD extraFlags = 0,
            const std::vector<std::wstring>* newNames = nullptr, OpRecorder* rec = nullptr, const std::vector<std::wstring>* dests = nullptr) {
    if (files.empty()) return false;
    for (const std::wstring& f : files)
        if (TestRefuses(f)) return false;
    if (kind != FileOpKind::Delete && TestRefuses(dest)) return false;
    if (dests)
        for (const std::wstring& dd : *dests)
            if (TestRefuses(dd)) return false;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return false;
    op->SetOwnerWindow(g_hwnd);
    DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT | FOF_NOERRORUI | extraFlags;
    // FOF_ALLOWUNDO sends to the Recycle Bin: a permanent delete can't use it
    if (kind != FileOpKind::Delete || (extraFlags & FOFX_RECYCLEONDELETE)) flags |= FOF_ALLOWUNDO | FOFX_ADDUNDORECORD;
    if (kind == FileOpKind::Delete && (extraFlags & FOFX_RECYCLEONDELETE)) flags |= FOF_WANTNUKEWARNING;
    op->SetOperationFlags(flags);
    ComPtr<IShellItem> d;
    if (kind != FileOpKind::Delete && FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(&d)))) return false;
    std::unordered_map<std::wstring, ComPtr<IShellItem>> dItems;  // with dests: each folder once
    auto destOf = [&](size_t i) -> std::pair<const std::wstring*, IShellItem*> {
        if (!dests || i >= dests->size()) return {&dest, d.Get()};
        const std::wstring& p = (*dests)[i];
        auto it = dItems.find(Key(p));
        if (it == dItems.end()) {
            ComPtr<IShellItem> x;
            if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&x)))) return {&p, nullptr};
            it = dItems.emplace(Key(p), x).first;
        }
        return {&p, it->second.Get()};
    };
    OpRecorder* own = rec ? nullptr : new OpRecorder();
    OpRecorder* r = rec ? rec : own;
    int added = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        ComPtr<IShellItem> src;
        if (FAILED(SHCreateItemFromParsingName(files[i].c_str(), nullptr, IID_PPV_ARGS(&src)))) continue;
        const wchar_t* nn = newNames && i < newNames->size() ? (*newNames)[i].c_str() : nullptr;
        const auto [where, to] = destOf(i);
        if (kind != FileOpKind::Delete && !to) continue;
        r->AddSource(files[i], kind == FileOpKind::Delete ? L"" : *where, nn);
        if (kind == FileOpKind::Copy) op->CopyItem(src.Get(), to, nn, nullptr);
        else if (kind == FileOpKind::Move) op->MoveItem(src.Get(), to, nn, nullptr);
        else op->DeleteItem(src.Get(), nullptr);
        ++added;
    }
    bool ok = false;
    if (added) {
        r->Attach(op.Get());
        ok = SUCCEEDED(op->PerformOperations());
        r->Detach(op.Get());
        if (own) own->Commit();
        if (!r->failed.empty()) OpFailed(kind, files, dest, extraFlags, newNames, r->failed, dests);
        // the folders it touched are read again: whatever shows them catches up, subfolder search results too
        std::unordered_set<std::wstring> again;
        for (const std::wstring& f : files)
            if (const std::wstring dir = ParentOf(f); !dir.empty() && again.insert(Key(dir)).second) Enqueue(dir, JobKind::Revalidate);
        if (!dest.empty() && again.insert(Key(dest)).second) Enqueue(dest, JobKind::Revalidate);
        if (dests)
            for (const std::wstring& dd : *dests)
                if (again.insert(Key(dd)).second) Enqueue(dd, JobKind::Revalidate);
    }
    if (own) own->Release();
    return ok;
}

// Restores from the Recycle Bin with the Windows verb. Looks up each item in the Recycle Bin's virtual folder: the one
// IFileOperation returns is the $R file, and that one doesn't have the verb. The verb leaves the record ($I…) behind,
// so that goes here. Returns the original paths that came back.
std::unordered_set<std::wstring> BinRestore(const std::vector<std::pair<std::wstring, std::wstring>>& items) {
    std::unordered_set<std::wstring> back;  // items: path in the Recycle Bin, original path
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
        for (int i = 0; i < 150 && !PathExists(original); ++i) Sleep(20);  // the verb may finish later
        if (PathExists(original)) back.insert(Key(original));
    }
    for (const auto& [where, original] : items)
        if (!PathExists(where)) DeleteFileW(ToExtendedPath(BinRecordOf(where)).c_str());
    return back;
}

std::wstring UndoDesc(const UndoEntry& e) {
    const UndoItem& first = e.items[0];
    std::wstring what = e.items.size() == 1 ? L"“" + LeafOf(e.kind == UndoKind::Delete ? first.from : first.to) + L"”"
                                            : Thousands(static_cast<int>(e.items.size())) + L" items";
    switch (e.kind) {
        case UndoKind::Rename: return L"rename " + what;
        case UndoKind::Move: return L"move " + what;
        case UndoKind::Copy: return L"copy " + what;
        case UndoKind::Create: return L"create " + what;
        case UndoKind::Delete: return L"send " + what + L" to the Recycle Bin";
    }
    return what;
}

// If a renamed folder was expanded, the tree follows it.
void TreeFollow(Tab& t, const std::wstring& from, const std::wstring& to) {
    std::wstring k = Key(from);
    if (!t.expanded.count(k)) return;
    t.expanded.erase(k);
    t.kids.erase(k);
    t.expanded[Key(to)] = to;
    LoadKid(t, Key(to), to);
}

// Applies an entry backward (undo) or forward. Leaves in e.items what succeeded and in why the reason
// the rest failed; returns how many failed.
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
        if (undo)  // flattened: the folders it removed come back, for their things to go back into
            for (const std::wstring& d : e.dissolved)
                if (!PathExists(d) && PathExists(ParentOf(d)) && CreateDirectoryW(ToExtendedPath(d).c_str(), nullptr)) touch(d);
        std::vector<UndoItem> ready;
        for (UndoItem& i : e.items) {
            if (!PathExists(srcOf(i))) fail(L"no longer where it was");
            else if (Key(srcOf(i)) != Key(dstOf(i)) && PathExists(dstOf(i))) fail(L"something with that name already exists");
            else if (!PathExists(ParentOf(dstOf(i)))) fail(L"its folder no longer exists");
            else ready.push_back(i);
        }
        if (e.kind == UndoKind::Rename) {
            for (UndoItem& i : ready) {
                if (MoveFileExW(ToExtendedPath(srcOf(i)).c_str(), ToExtendedPath(dstOf(i)).c_str(), 0)) {
                    for (auto& tab : g_tabs) TreeFollow(*tab, srcOf(i), dstOf(i));
                    done.push_back(i);
                } else {
                    fail(L"in use");
                }
            }
        } else {
            std::unordered_map<std::wstring, std::vector<UndoItem>> byDir;  // there can be more than one destination
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
                    else fail(L"couldn't be moved");
                }
            }
        }
        if (!undo)  // and flattening again removes them, if they ended up empty
            for (const std::wstring& d : e.dissolved)
                if (RemoveDirectoryW(ToExtendedPath(d).c_str())) touch(d);
        for (UndoItem& i : done) {
            touch(i.from);
            touch(i.to);
            gone.emplace_back(srcOf(i), dstOf(i));
        }
        if (!done.empty()) select = dstOf(done[0]);
    } else {
        const bool toBin = (e.kind == UndoKind::Delete) != undo;  // undoing create = redoing delete
        auto place = [&](const UndoItem& i) -> const std::wstring& { return e.kind == UndoKind::Delete ? i.from : i.to; };
        if (toBin) {
            std::vector<UndoItem> ready;
            std::vector<std::wstring> paths;
            for (UndoItem& i : e.items) {
                const std::wstring& p = place(i);
                ULONGLONG stamp = 0, size = 0;
                if (!PathStamp(p, stamp, size)) fail(L"no longer there");
                else if (e.kind == UndoKind::Copy && (stamp != i.stamp || size != i.size)) fail(L"changed since then");
                else if (e.kind == UndoKind::Create && (PathIsDir(p) ? !PathIsDirectoryEmptyW(ToExtendedPath(p).c_str()) : size != 0))
                    fail(L"now has content");
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
                    fail(L"couldn't be sent to the Recycle Bin");
                    continue;
                }
                i.bin = it->bin;  // empty if there was no Recycle Bin: Windows already warned it was permanent
                touch(place(i));
                gone.emplace_back(place(i), L"");
                done.push_back(i);
            }
            rec->Release();
        } else {
            std::vector<std::pair<std::wstring, std::wstring>> wanted;
            std::vector<UndoItem> ready;
            for (UndoItem& i : e.items) {
                if (PathExists(place(i))) fail(L"something with that name already exists");
                else if (!PathExists(ParentOf(place(i)))) fail(L"its folder no longer exists");
                else if (i.bin.empty() || !PathExists(i.bin)) fail(L"no longer in the Recycle Bin");
                else {
                    wanted.emplace_back(i.bin, place(i));
                    ready.push_back(i);
                }
            }
            std::unordered_set<std::wstring> back;
            if (!wanted.empty()) back = BinRestore(wanted);
            for (UndoItem& i : ready) {
                if (!back.count(Key(place(i)))) {
                    fail(L"couldn't be restored from the Recycle Bin");
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

// A tab open inside something that moved follows it; if it went to the Recycle Bin, the tab goes up to
// the folder that contained it.
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
    Log(L"undo: %s (history %zu, redo %zu, busy %d)", undo ? L"undo" : L"redo", g_undo.size(), g_redo.size(), g_undoBusy);
    if (g_undoBusy) return;
    auto& from = undo ? g_undo : g_redo;
    auto& to = undo ? g_redo : g_undo;
    if (from.empty()) {
        S.message = undo ? L"Nothing to undo" : L"Nothing to redo";
        Invalidate();
        return;
    }
    for (const UndoItem& i : from.back().items)
        if (TestRefuses(i.from) || TestRefuses(i.to)) return;
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

    const wchar_t* verb = undo ? L"Undone: " : L"Redone: ";
    if (!failed) {
        S.message = verb + desc;
    } else if (!done) {
        S.message = std::wstring(undo ? L"Couldn't undo " : L"Couldn't redo ") + desc + L": " + why;
        MessageBeep(MB_ICONWARNING);
    } else {
        S.message = verb + std::to_wstring(done) + L" of " + std::to_wstring(total) + L". The rest: " + why;
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

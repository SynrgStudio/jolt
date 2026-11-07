// File operations and quick commands. Included from app.cpp after dragdrop.inl.

// To the Recycle Bin with IFileOperation, which reports where each item ended up (Ctrl+Z brings it back)
// and honours the confirmation chosen in Windows. Permanent delete can't be undone: within a single folder
// it uses the Shell verb, which asks the way File Explorer does.
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

// Ctrl+Shift+C: the path of the selection (one per line), or of the folder if nothing is selected.
// Quotes are added only when needed. Name: just the name, with its extension. Unix: forward slashes, no quotes.
enum class CopyWhat { Path, Name, Unix };
void CopyPathText(CopyWhat what) {
    Tab& t = T();
    std::vector<std::wstring> paths;
    for (int i = 0; i < Count(); ++i)
        if (IsSel(t, i)) paths.push_back(RowPath(t, i));
    if (paths.empty() && !t.path.empty()) paths.push_back(t.path);
    if (paths.empty()) return;
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::wstring p = paths[i];
        if (what == CopyWhat::Name) p = IsDriveRoot(p) ? p : LeafOf(p);
        if (what == CopyWhat::Unix) std::replace(p.begin(), p.end(), L'\\', L'/');
        bool quote = what == CopyWhat::Path && p.find_first_of(L" &()[]{}^=;!'+,`~") != std::wstring::npos;
        text += (i ? L"\r\n" : L"") + (quote ? L"\"" + p + L"\"" : p);
    }
    if (SetClipboardText(text)) {
        const wchar_t* one = what == CopyWhat::Name ? L"Name copied: " : L"Path copied: ";
        const wchar_t* many = what == CopyWhat::Name ? L" names copied" : L" paths copied";
        S.message = paths.size() == 1 ? one + text : Thousands(static_cast<int>(paths.size())) + many;
    }
    Invalidate();
}

// Duplicates the selection in its own folder: "name - Copy.ext", "name - Copy (2).ext"...
void Duplicate() {
    Tab& t = T();
    std::vector<std::wstring> files = SelectedFullPaths(t), names;
    if (files.empty()) return;
    std::wstring firstCopy;
    // one destination folder at a time (the tree can have several)
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
        std::wstring nn = FreeName(dir, base + L" - Copy", ext);
        // so two copies in the same batch don't pick the same name
        auto& slot = byDir[Key(dir)];
        for (int i = 2; std::find(slot.second.begin(), slot.second.end(), nn) != slot.second.end(); ++i)
            nn = FreeName(dir, base + L" - Copy (" + std::to_wstring(i) + L")", ext);
        if (slot.first.empty()) order.push_back(dir);
        slot.first.push_back(f);
        slot.second.push_back(nn);
        if (firstCopy.empty()) firstCopy = Join(dir, nn.c_str());
    }
    for (const std::wstring& dir : order) {
        auto& slot = byDir[Key(dir)];
        FileOp(FileOpKind::Copy, slot.first, dir, 0, &slot.second);
        g_undoMerge = true;  // a single Ctrl+Z for the whole duplicate
        Enqueue(dir, JobKind::Revalidate);
    }
    g_undoMerge = false;
    t.pendingSelect = firstCopy;
}

// Alt+Enter: Windows properties of the selection (or of the folder, if nothing is selected).
void Properties() {
    Tab& t = T();
    std::vector<std::wstring> names = SelectedNames(t);
    if (names.empty()) {
        if (!t.path.empty()) SHObjectProperties(g_hwnd, SHOP_FILEPATH, t.path.c_str(), nullptr);
        return;
    }
    ShellVerb(names, "properties");
}

// Ctrl+Shift+G: moves the selection into a new folder next to it and starts renaming the folder.
// One item: the folder takes its name (without the extension). Several: "New folder".
void GroupInFolder() {
    Tab& t = T();
    std::vector<std::wstring> files = SelectedFullPaths(t);
    if (files.empty()) return;
    const std::wstring dir = ParentOf(files[0]);
    for (const std::wstring& f : files)
        if (Key(ParentOf(f)) != Key(dir)) {
            S.message = L"Select items from a single folder to group them";
            Invalidate();
            return;
        }
    std::wstring base = L"New folder";
    if (files.size() == 1) {
        base = LeafOf(files[0]);
        DWORD a = GetFileAttributesW(ToExtendedPath(files[0]).c_str());
        size_t dot = base.find_last_of(L'.');
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && dot != std::wstring::npos && dot > 0) base.resize(dot);
        while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) base.pop_back();
        if (base.empty()) base = L"New folder";
    }
    const std::wstring name = FreeName(dir, base, L""), folder = Join(dir, name.c_str());
    if (!CreateDirectoryW(ToExtendedPath(folder).c_str(), nullptr)) {
        S.message = L"Couldn't create the folder: " + ErrorText(GetLastError());
        Invalidate();
        return;
    }
    UndoRecordCreate(folder);
    if (!FileOp(FileOpKind::Move, files, folder)) {
        RemoveDirectoryW(ToExtendedPath(folder).c_str());  // nothing moved: leave no empty folder behind
        Enqueue(dir, JobKind::Revalidate);
        return;
    }
    if (Key(dir) == Key(t.path)) {
        if (!t.filter.empty()) {
            t.filter.clear();
            BuildRows(t);
        }
        t.pendingSelect = name;
        g_renameAfterSelect = name;
    }
    S.message = Thousands(static_cast<int>(files.size())) + (files.size() == 1 ? L" item moved into " : L" items moved into ") + name;
    Enqueue(dir, JobKind::Revalidate);
    Invalidate();
}

// F5 / F6 with split view: copy / move the selection to the other pane's folder.
void TransferToOther(bool move) {
    if (!g_split) {
        S.message = L"Open split view to copy or move to the other pane";
        Invalidate();
        return;
    }
    Tab& src = T();
    Tab& dst = PT(1 - g_act);
    if (dst.path.empty()) {
        S.message = L"The other pane isn't in a folder";
        Invalidate();
        return;
    }
    std::vector<std::wstring> files = SelectedFullPaths(src);
    if (files.empty()) return;
    bool same = true;
    for (const auto& f : files) same &= Key(ParentOf(f)) == Key(dst.path);
    if (same) {
        S.message = L"Already in that folder";
        Invalidate();
        return;
    }
    if (FileOp(move ? FileOpKind::Move : FileOpKind::Copy, files, dst.path)) {
        if (files.size() == 1) dst.pendingSelect = LeafOf(files[0]);
        S.message = (move ? L"Moved to " : L"Copied to ") + TabTitle(dst);
    }
    Enqueue(dst.path, JobKind::Revalidate);
    Enqueue(src.path, JobKind::Revalidate);
    Invalidate();
}

// Opens the folder containing the highlighted item (useful in search and the tree) and leaves it selected.
void GoToLocation() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return;
    std::wstring dir = RowDir(t, t.sel), name = RowName(t, t.sel);
    if (Key(dir) == Key(t.path) && !FlatView(t)) return;
    g_lastSel[Key(dir)] = name;
    Navigate(t, dir, true);
}

// From a shortcut (or a symbolic link or a junction): goes to the folder holding the target and
// leaves it highlighted.
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
            S.message = L"This shortcut doesn't point to a file";
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
            S.message = L"This isn't a shortcut";
            Invalidate();
            return;
        }
    }
    if (GetFileAttributesW(real.c_str()) == INVALID_FILE_ATTRIBUTES) {
        S.message = L"The target no longer exists: " + real;
        Invalidate();
        return;
    }
    real = Normalize(real);
    const std::wstring dir = ParentOf(real);
    if (dir.empty()) {
        Navigate(t, real, true);  // points to a drive root
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

// The terminal in the current folder: Windows Terminal if present, otherwise cmd. The folder is passed as
// the working directory ("-d ."), so there are no quotes to break on paths like C:\.
void OpenTerminal() {
    const std::wstring dir = T().path;
    if (dir.empty()) {
        S.message = L"Go into a folder to open the terminal";
        Invalidate();
        return;
    }
    if (g_test) {  // tests don't open real windows
        S.message = L"Terminal in " + dir;
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
        S.message = L"Couldn't open the terminal";
        Invalidate();
    }
}

// File Explorer in the current folder, with the highlighted item already selected.
void OpenInExplorer() {
    Tab& t = T();
    std::wstring args;
    if (t.path.empty()) args = L"shell:MyComputerFolder";
    else if (t.sel >= 0 && t.sel < Count() && !IsHeader(t, t.sel)) args = L"/select,\"" + RowPath(t, t.sel) + L"\"";
    else args = L"\"" + t.path + L"\"";
    if (g_test) {
        S.message = L"File Explorer: " + args;
        Invalidate();
        return;
    }
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// Empties the Recycle Bin on all drives. Windows asks for confirmation; it runs on another thread because
// deleting many gigabytes takes time and Jolt must not hang.
void EmptyRecycleBin() {
    SHQUERYRBINFO before{sizeof(before)};
    SHQueryRecycleBinW(nullptr, &before);
    if (before.i64NumItems == 0) {
        S.message = L"The Recycle Bin is already empty";
        Invalidate();
        return;
    }
    if (g_test) {  // tests never touch the real Recycle Bin
        S.message = L"Recycle Bin has items (not emptied in tests)";
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
        if (after.i64NumItems == 0) {  // otherwise it was cancelled: nothing to report
            wchar_t buf[64];
            FormatSize(bytes, buf, 64);
            IdxStatus(std::wstring(L"Recycle Bin emptied: ") + buf + L" freed");
        }
        CoUninitialize();
    }).detach();
}

// Alt+R: computes the size of every folder in the list; with several selected, only those.
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
    S.message = L"Calculating the size of " + Thousands(static_cast<int>(dirs.size())) +
                (dirs.size() == 1 ? L" carpeta" : L" carpetas");
    Invalidate();
}

// --- split view
void SetSplit(bool on) {
    if (on == g_split) return;
    CancelRename();
    if (on) {
        g_paneCur[0] = g_cur;
        g_split = true;
        for (auto& t : g_tabs) t->pane = 0;
        // the right pane starts in the same folder
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

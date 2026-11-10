// File operations and quick commands. Included from app.cpp after dragdrop.inl.

// To the Recycle Bin with IFileOperation, which reports where each item ended up (Ctrl+Z brings it back)
// and honours the confirmation chosen in Windows. Permanent delete can't be undone: within a single folder
// it uses the Shell verb, which asks the way File Explorer does.
void DeleteSelection() {
    Tab& t = T();
    std::vector<std::wstring> all = SelectedFullPaths(t);
    std::vector<std::wstring> names = SelectedNames(t);
    const bool permanent = KeyDown(VK_SHIFT);
    // in search results Jolt deletes it too (it also asks), so the results know what went
    if (all.empty() || (permanent && names.size() == all.size() && !t.recursive)) {
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
    const bool ok = FileOp(FileOpKind::Delete, all, L"", flags);
    if (ok && t.dups) {  // duplicates: say what went, so the copies that stay don't look gone too
        const int n = static_cast<int>(all.size());
        S.message = Thousands(n) + (n == 1 ? L" copy " : L" copies ") + (permanent ? L"deleted for good" : L"sent to the Recycle Bin") +
                    L"; the ones still listed stay";
        Invalidate();
    }
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

bool SaveBitmapPng(IWICBitmapSource* bmp, const std::wstring& file);  // testmode.inl

// Ctrl+V without files on the clipboard: an image becomes a PNG in the folder, text a .txt (UTF-8),
// named by the moment, and ready to rename.
void PasteClipboardAsFile() {
    Tab& t = T();
    if (t.path.empty() || !t.list || t.list->error || TestRefuses(t.path)) return;
    const bool image = IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB);
    const bool text = IsClipboardFormatAvailable(CF_UNICODETEXT);
    if (!image && !text) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"Clipboard %04d-%02d-%02d %02d.%02d.%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring name = FreeName(t.path, stamp, image ? L".png" : L".txt");
    const std::wstring full = Join(t.path, name.c_str());
    bool ok = false;
    if (!OpenClipboard(g_hwnd)) return;
    if (image) {
        // Preferred: the "PNG" format many apps add (keeps transparency, written as is). Then the DIB,
        // decoded by WIC as a BMP file. CF_BITMAP last: with OLE clipboards it is often announced but not delivered.
        auto bytes = [&](UINT fmt, const BYTE*& data, size_t& size) {
            HGLOBAL h = GetClipboardData(fmt);
            data = h ? static_cast<const BYTE*>(GlobalLock(h)) : nullptr;
            size = data ? GlobalSize(h) : 0;
            return h;
        };
        const BYTE* data = nullptr;
        size_t size = 0;
        if (HGLOBAL h = bytes(RegisterClipboardFormatW(L"PNG"), data, size); h && size > 8 && memcmp(data, "PNG", 4) == 0) {
            HANDLE f = CreateFileW(ToExtendedPath(full).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD n = 0;
                ok = WriteFile(f, data, static_cast<DWORD>(size), &n, nullptr) && n == size;
                CloseHandle(f);
                if (!ok) DeleteFileW(ToExtendedPath(full).c_str());
            }
            GlobalUnlock(h);
        } else if (HGLOBAL h2 = bytes(CF_DIB, data, size); h2 && size > sizeof(BITMAPINFOHEADER)) {
            const auto* bi = reinterpret_cast<const BITMAPINFOHEADER*>(data);
            DWORD colors = bi->biClrUsed ? bi->biClrUsed : bi->biBitCount <= 8 ? 1u << bi->biBitCount : 0;
            DWORD masks = bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
            std::vector<BYTE> bmp(sizeof(BITMAPFILEHEADER) + size);
            auto* fh = reinterpret_cast<BITMAPFILEHEADER*>(bmp.data());
            fh->bfType = 0x4D42;
            fh->bfSize = static_cast<DWORD>(bmp.size());
            fh->bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + bi->biSize + masks + colors * 4);
            memcpy(bmp.data() + sizeof(BITMAPFILEHEADER), data, size);
            GlobalUnlock(h2);
            ComPtr<IWICStream> ws;
            ComPtr<IWICBitmapDecoder> dec;
            ComPtr<IWICBitmapFrameDecode> fr;
            if (SUCCEEDED(g_wic->CreateStream(&ws)) && SUCCEEDED(ws->InitializeFromMemory(bmp.data(), static_cast<DWORD>(bmp.size()))) &&
                SUCCEEDED(g_wic->CreateDecoderFromStream(ws.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)))
                ok = SaveBitmapPng(fr.Get(), ToExtendedPath(full));
        } else if (HBITMAP hbm = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
            ComPtr<IWICBitmap> wb;
            if (SUCCEEDED(g_wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapIgnoreAlpha, &wb))) ok = SaveBitmapPng(wb.Get(), ToExtendedPath(full));
        }
    } else if (HGLOBAL h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            std::string utf8(WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr), '\0');
            if (!utf8.empty()) utf8.resize(WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr));
            while (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();  // the terminator
            HANDLE f = CreateFileW(ToExtendedPath(full).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD n = 0;
                ok = WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &n, nullptr) && n == utf8.size();
                CloseHandle(f);
                if (!ok) DeleteFileW(ToExtendedPath(full).c_str());
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (!ok) {
        S.message = image ? L"Couldn't save the clipboard image" : L"Couldn't save the clipboard text";
        Invalidate();
        return;
    }
    UndoRecordCreate(full);
    if (!t.filter.empty()) {
        t.filter.clear();
        BuildRows(t);
    }
    t.pendingSelect = name;
    g_renameAfterSelect = name;
    S.message = (image ? L"Image pasted as " : L"Text pasted as ") + name;
    Enqueue(t.path, JobKind::Revalidate);
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
    // items from several folders (search results): the menu verb can't open them together, the multi-file
    // properties sheet can, from the same data object a drag uses
    if (SelSpansFolders(t)) {
        if (ComPtr<IDataObject> d = SelectionDataObject()) SHMultiFileProperties(d.Get(), 0);
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
    if (TestRefuses(dir)) return;
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
    if (dst.path.empty() || IsVirtual(dst.path) || InArchive(dst.path)) {
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
        std::vector<std::wstring> names;
        for (const auto& f : files) names.push_back(LeafOf(f));
        SelectLanded(dst.path, names);
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

// One of the user's commands ([commands] in config.txt): the first word is the program, the rest its
// arguments, with {file} {files} {dir} {name} filled in from the cursor and the selection.
void RunCustomCommand(int i) {
    if (i < 0 || i >= static_cast<int>(g_cfg.custom.size())) return;
    const Config::Custom& c = g_cfg.custom[i];
    Tab& t = T();
    const bool onItem = t.sel >= 0 && t.sel < Count() && !IsHeader(t, t.sel) && !RowDir(t, t.sel).empty();
    const std::wstring dir = onItem ? RowDir(t, t.sel) : t.path;
    const std::wstring file = onItem ? RowPath(t, t.sel) : dir;
    std::wstring files;
    for (const std::wstring& f : SelectedFullPaths(t)) files += (files.empty() ? L"\"" : L" \"") + f + L"\"";
    if (files.empty()) files = L"\"" + file + L"\"";
    const std::wstring& line = c.line;
    // program: quoted, or up to the first space
    std::wstring program, args;
    if (!line.empty() && line[0] == L'"') {
        size_t q = line.find(L'"', 1);
        program = line.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
        if (q != std::wstring::npos) args = Trim(line.substr(q + 1));
    } else {
        size_t sp = line.find(L' ');
        program = line.substr(0, sp);
        if (sp != std::wstring::npos) args = Trim(line.substr(sp + 1));
    }
    if (dir.empty() && args.find(L'{') != std::wstring::npos) {
        S.message = L"Go into a folder to run " + c.name;
        Invalidate();
        return;
    }
    auto fill = [&](const wchar_t* tag, const std::wstring& value) {
        for (size_t p = args.find(tag); p != std::wstring::npos; p = args.find(tag, p + value.size())) args.replace(p, wcslen(tag), value);
    };
    fill(L"{files}", files);
    fill(L"{file}", file);
    fill(L"{dir}", dir);
    fill(L"{name}", onItem ? RowName(t, t.sel) : LeafOf(dir));
    if (g_test) {  // tests don't start programs
        S.message = L"Run: " + program + (args.empty() ? L"" : L" " + args) + L" | in " + dir;
        Invalidate();
        return;
    }
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.fMask = SEE_MASK_FLAG_NO_UI;
    si.hwnd = g_hwnd;
    si.lpFile = program.c_str();
    si.lpParameters = args.empty() ? nullptr : args.c_str();
    si.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) {
        S.message = L"Couldn't run " + c.name + L": " + ErrorText(GetLastError());
        Invalidate();
    }
}

// File Explorer in the current folder, with the highlighted item already selected.
void OpenInExplorer() {
    Tab& t = T();
    std::wstring args;
    if (t.path.empty()) args = L"shell:MyComputerFolder";
    if (IsBin(t.path)) args = L"shell:RecycleBinFolder";
    if (IsNet(t.path)) args = L"shell:NetworkPlacesFolder";
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
void EmptyRecycleBin();

// What makes no sense on something deleted: the real file has a made-up name, and it has to come back first.
bool BinRefuses(Cmd cmd) {
    switch (cmd) {
        case Cmd::Rename: case Cmd::RenameMany: case Cmd::NewFolder: case Cmd::NewFile: case Cmd::Copy: case Cmd::Cut: case Cmd::Paste:
        case Cmd::CopyToOther: case Cmd::MoveToOther: case Cmd::CopyTo: case Cmd::MoveTo: case Cmd::Attributes: case Cmd::InUse: case Cmd::Unblock: case Cmd::CopyPath: case Cmd::CopyName: case Cmd::CopyPathUnix: case Cmd::Duplicate:
        case Cmd::GroupInFolder: case Cmd::Properties: case Cmd::Terminal: case Cmd::RealLocation: case Cmd::OpenAsAdmin: case Cmd::Tag:
        case Cmd::SearchSubfolders: case Cmd::DeleteEmptyFolders: case Cmd::Flatten:
            return true;
        default:
            return IsCustom(cmd);
    }
}

// Back where they were, with Windows' own verb: it deals with conflicts and removes the record.
void BinRestoreSelection() {
    Tab& t = T();
    std::vector<std::pair<std::wstring, std::wstring>> items;
    for (int i = 0; i < Count(); ++i)
        if (IsSel(t, i)) {
            BinItem b = BinItemOf(RowName(t, i).c_str());
            if (!b.real.empty()) items.push_back({b.real, b.original});
        }
    if (items.empty()) return;
    S.message = L"Restoring…";
    std::thread([items] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const size_t back = BinRestore(items).size();
        CoUninitialize();
        Enqueue(kBinPath, JobKind::Revalidate);
        IdxStatus(back == items.size() ? (back == 1 ? L"Restored" : L"Restored " + Thousands(static_cast<int>(back)) + L" items")
                                       : L"Restored " + Thousands(static_cast<int>(back)) + L" of " + Thousands(static_cast<int>(items.size())));
    }).detach();
}

// Gone for good: the item and its record.
void BinDeleteSelection() {
    Tab& t = T();
    std::vector<std::wstring> real;
    for (int i = 0; i < Count(); ++i)
        if (IsSel(t, i)) {
            BinItem b = BinItemOf(RowName(t, i).c_str());
            if (!b.real.empty()) real.push_back(b.real);
        }
    if (real.empty()) return;
    const std::wstring what = real.size() == 1 ? L"“" + RowName(t, t.sel) + L"”" : Thousands(static_cast<int>(real.size())) + L" items";
    if (!g_test && MessageBoxW(g_hwnd, (L"Delete " + what + L" for good? This can't be undone.").c_str(), L"Recycle Bin", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    auto* rec = new OpRecorder();  // doesn't go to the history: it can't be undone
    FileOp(FileOpKind::Delete, real, L"", FOF_NOCONFIRMATION, nullptr, rec);
    rec->Release();
    int gone = 0;
    for (const std::wstring& p : real)
        if (!PathExists(p)) {
            DeleteFileW(ToExtendedPath(BinRecordOf(p)).c_str());
            ++gone;
        }
    S.message = gone == 1 ? L"Deleted for good" : L"Deleted " + Thousands(gone) + L" items for good";
    Enqueue(kBinPath, JobKind::Revalidate);
    Invalidate();
}

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
                (dirs.size() == 1 ? L" folder" : L" folders");
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
        KeepPinnedFirst();
    }
    for (auto& tp : g_tabs) ClampScroll(*tp);
    SetWatchAll();
    UpdateTitle();
    LayoutEdit();
    Invalidate();
    TouchSession();
}

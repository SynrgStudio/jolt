// Tidying folders (Ctrl+K). "delete empty folders": the folders inside this one (or inside the selected ones) that
// have nothing in them, not even in their subfolders, go to the Recycle Bin. "flatten": what is inside the selected
// folders moves up next to them, and the emptied folders go away. Each is one step of Ctrl+Z.
// Included from app.cpp after ops.inl.

constexpr UINT WM_APP_TIDY = WM_APP + 25;

struct TidyResult {
    uint64_t gen = 0;
    std::wstring where;             // what was looked in, for the message
    std::vector<std::wstring> top;  // the empty folders to delete: the outermost ones
    int total = 0;                  // every empty folder, the ones inside the others too
    int unread = 0;                 // folders that couldn't be read: they count as having something
};
std::atomic<uint64_t> g_tidyGen{0};
bool g_tidyBusy = false;

// Does dir have nothing in it but folders that have nothing in them? Then n is how many folders that is, itself
// included. When it has something, its empty subfolders go into r.
bool TidyWalk(const std::wstring& dir, TidyResult& r, int& n, uint64_t gen) {
    if (g_tidyGen != gen) return false;
    Listing l;
    if (!EnumerateDir(dir, EnumMethod::Nt, l) || l.error) {
        ++r.unread;
        return false;
    }
    bool empty = true;
    std::vector<std::pair<std::wstring, int>> emptyKids;
    for (const Entry& e : l.entries) {
        if (!e.isDir() || (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {  // a file, or a link: something
            empty = false;
            continue;
        }
        const std::wstring sub = Join(dir, l.name(e));
        int k = 0;
        if (TidyWalk(sub, r, k, gen)) emptyKids.emplace_back(sub, k);
        else empty = false;
    }
    if (empty) {
        n = 1;
        for (const auto& kid : emptyKids) n += kid.second;
        return true;
    }
    for (const auto& [p, k] : emptyKids) {
        r.top.push_back(p);
        r.total += k;
    }
    return false;
}

// Several selected: the empty folders in them, and those of them that are empty. Otherwise: inside this folder.
void DeleteEmptyFolders() {
    Tab& t = T();
    if (g_tidyBusy) return;
    if ((t.path.empty() && !t.recursive) || IsVirtual(t.path) || !t.list || t.list->error) {
        S.message = L"Open a folder to look for empty folders in it";
        Invalidate();
        return;
    }
    std::vector<std::wstring> roots;  // each one: empty, or looked into
    bool inside = true;               // roots are the subfolders of this folder (they can go too, it can't)
    std::wstring where = TabTitle(t);
    if (SelCount(t) > 1) {
        for (const std::wstring& d : SelectedFolders(t))
            if (!(GetFileAttributesW(ToExtendedPath(d).c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) roots.push_back(d);
        if (roots.empty()) {
            S.message = L"Select folders to look for empty folders in them, or nothing for this whole folder";
            Invalidate();
            return;
        }
        inside = false;
        where = roots.size() == 1 ? LeafOf(roots[0]) : Thousands(static_cast<int>(roots.size())) + L" folders";
    }
    if (TestRefuses(t.path)) return;
    g_tidyBusy = true;
    const uint64_t gen = ++g_tidyGen;
    const std::wstring path = t.path;
    S.message = L"Looking for empty folders in " + where + L"…";
    Invalidate();
    std::thread([roots, inside, where, path, gen] {
        auto* r = new TidyResult;
        r->gen = gen;
        r->where = where;
        std::vector<std::wstring> look = roots;
        if (inside) {
            Listing l;
            if (EnumerateDir(path, EnumMethod::Nt, l) && !l.error)
                for (const Entry& e : l.entries)
                    if (e.isDir() && !(e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) look.push_back(Join(path, l.name(e)));
        }
        for (const std::wstring& d : look) {
            int k = 0;
            if (TidyWalk(d, *r, k, gen)) {
                r->top.push_back(d);
                r->total += k;
            }
        }
        if (!PostMessageW(g_hwnd, WM_APP_TIDY, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void TidyApply(LPARAM lp) {
    std::unique_ptr<TidyResult> r(reinterpret_cast<TidyResult*>(lp));
    if (r->gen != g_tidyGen) return;
    g_tidyBusy = false;
    const std::wstring unread = r->unread ? L" (" + Thousands(r->unread) + (r->unread == 1 ? L" folder couldn't be read)" : L" folders couldn't be read)") : L"";
    if (r->top.empty()) {
        S.message = L"No empty folders in " + r->where + unread;
        Invalidate();
        return;
    }
    std::sort(r->top.begin(), r->top.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    DWORD flags = FOFX_RECYCLEONDELETE;
    SHELLSTATE ss{};
    SHGetSetSettings(&ss, SSF_NOCONFIRMRECYCLE, FALSE);
    if (ss.fNoConfirmRecycle) flags |= FOF_NOCONFIRMATION;
    if (g_test) flags = FOF_NOCONFIRMATION;  // tests stay out of the person's Recycle Bin: empty folders, deleted for good
    FileOp(FileOpKind::Delete, r->top, L"", flags);
    int gone = 0;  // what really went: Windows may have asked, and been told no
    for (const std::wstring& d : r->top) gone += !PathExists(d);
    if (!gone) S.message = L"No folders deleted";
    else if (gone == static_cast<int>(r->top.size()))
        S.message = Thousands(r->total) + (r->total == 1 ? L" empty folder" : L" empty folders") + L" sent to the Recycle Bin" + unread;
    else
        S.message = Thousands(gone) + L" of " + Thousands(static_cast<int>(r->top.size())) + L" empty folders sent to the Recycle Bin";
    Invalidate();
}

// A name that none of taken has (lowercase), like FreeName: "name (2).ext". Folders have no extension.
std::wstring TidyFreeName(const std::wstring& name, bool dir, const std::unordered_set<std::wstring>& taken) {
    if (!taken.count(Lower(name))) return name;
    const size_t dot = dir ? std::wstring::npos : name.find_last_of(L'.');
    const std::wstring base = dot && dot != std::wstring::npos ? name.substr(0, dot) : name;
    const std::wstring ext = dot && dot != std::wstring::npos ? name.substr(dot) : L"";
    for (int i = 2;; ++i) {
        std::wstring n = base + L" (" + std::to_wstring(i) + L")" + ext;
        if (!taken.count(Lower(n))) return n;
    }
}

// What is inside the selected folders moves up, into the folder that holds them, and the emptied folders go away. A name
// already there gets " (2)". One step of Ctrl+Z puts it all back, the folders too.
void FlattenFolders() {
    Tab& t = T();
    std::vector<std::wstring> dirs;
    for (const std::wstring& d : SelectedFolders(t))
        if (!(GetFileAttributesW(ToExtendedPath(d).c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(d);
    if (dirs.empty()) {
        S.message = L"Select the folders to flatten: what is inside them moves up next to them";
        Invalidate();
        return;
    }
    // a folder inside another selected one goes up with it
    std::vector<std::wstring> keep;
    for (const std::wstring& d : dirs) {
        bool under = false;
        for (const std::wstring& o : dirs) under |= &o != &d && Key(d).rfind(Key(o) + L"\\", 0) == 0;
        if (!under) keep.push_back(d);
    }
    dirs.swap(keep);
    for (const std::wstring& d : dirs)
        if (TestRefuses(d)) return;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> taken;  // by the folder they go to: its names
    std::vector<std::wstring> files, names, dests;
    int renamed = 0;
    for (const std::wstring& d : dirs) {
        const std::wstring up = ParentOf(d);
        auto it = taken.find(Key(up));
        if (it == taken.end()) {
            Listing l;
            EnumerateDir(up, EnumMethod::Nt, l);
            it = taken.emplace(Key(up), std::unordered_set<std::wstring>{}).first;
            for (const Entry& e : l.entries) it->second.insert(Lower(std::wstring(l.name(e), e.nameLen)));
        }
        Listing l;
        if (!EnumerateDir(d, EnumMethod::Nt, l) || l.error) {
            S.message = L"Couldn't read " + LeafOf(d) + L": " + ErrorText(l.error);
            Invalidate();
            return;
        }
        for (const Entry& e : l.entries) {
            const std::wstring n(l.name(e), e.nameLen), to = TidyFreeName(n, e.isDir(), it->second);
            renamed += to != n;
            it->second.insert(Lower(to));
            files.push_back(Join(d, n.c_str()));
            names.push_back(to);
            dests.push_back(up);
        }
    }
    if (files.empty()) {
        S.message = dirs.size() == 1 ? LeafOf(dirs[0]) + L" is empty: nothing to move up" : std::wstring(L"Those folders are empty: nothing to move up");
        Invalidate();
        return;
    }
    const UndoEntry* before = g_undo.empty() ? nullptr : &g_undo.back();
    FileOp(FileOpKind::Move, files, dests[0], 0, &names, nullptr, &dests);
    std::vector<std::wstring> removed;  // only what ended up empty: what couldn't be moved stays where it was
    for (const std::wstring& d : dirs)
        if (RemoveDirectoryW(ToExtendedPath(d).c_str())) removed.push_back(d);
    if (!g_undo.empty() && &g_undo.back() != before && g_undo.back().kind == UndoKind::Move) g_undo.back().dissolved = removed;
    int moved = 0;
    std::vector<std::wstring> landed;
    for (size_t i = 0; i < files.size(); ++i)
        if (!PathExists(files[i]) && PathExists(Join(dests[i], names[i].c_str()))) {
            ++moved;
            if (Key(dests[i]) == Key(dests[0])) landed.push_back(names[i]);
        }
    for (const std::wstring& d : removed) Enqueue(ParentOf(d), JobKind::Revalidate);
    SelectLanded(dests[0], landed);
    if (!moved) {
        S.message = L"Nothing was moved";
    } else {
        S.message = (dirs.size() == 1 ? L"Flattened " + LeafOf(dirs[0]) : L"Flattened " + Thousands(static_cast<int>(dirs.size())) + L" folders") + L": " +
                    Thousands(moved) + (moved == 1 ? L" item moved up" : L" items moved up");
        if (renamed) S.message += L", " + Thousands(renamed) + L" renamed (the name was taken)";
    }
    Invalidate();
}

// Subfolder search (Ctrl+Shift+F): a thread walks the folder and all its subfolders with
// NtQueryDirectoryFile and sends the listings in batches; the fuzzy filter runs over all of that.
// Included from app.cpp after ops.inl.

constexpr UINT WM_APP_REC = WM_APP + 7;
std::atomic<uint64_t> g_recActive{0};  // the current search; the others stop on their own

struct RecBatch {
    uint64_t gen = 0;
    std::vector<std::shared_ptr<const Listing>> lists;
    int dirs = 0;
    bool done = false;
};

void RecThread(std::wstring root, uint64_t gen, bool showHidden, SortBy by, bool asc, bool ff) {
    std::deque<std::wstring> q{root};
    auto batch = std::make_unique<RecBatch>();
    batch->gen = gen;
    double last = NowMs();
    int dirs = 0;
    auto post = [&](bool done) {
        batch->dirs = dirs;
        batch->done = done;
        RecBatch* b = batch.release();
        if (!PostMessageW(g_hwnd, WM_APP_REC, 0, reinterpret_cast<LPARAM>(b))) delete b;
        batch = std::make_unique<RecBatch>();
        batch->gen = gen;
        last = NowMs();
    };
    while (!q.empty() && g_recActive == gen) {
        std::wstring p = std::move(q.front());
        q.pop_front();
        auto l = std::make_shared<Listing>();
        EnumerateDir(p, EnumMethod::Nt, *l);
        ++dirs;
        if (l->error) continue;
        SortListing(*l, by, asc, ff);
        for (const Entry& e : l->entries) {
            if (!e.isDir() || (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (!showHidden && (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            q.push_back(Join(p, l->name(e)));
        }
        batch->lists.push_back(std::move(l));
        if (NowMs() - last > (dirs < 2000 ? 120 : 300)) post(false);
    }
    if (g_recActive == gen) post(true);
}

void DupStart(Tab& t);  // dups.inl
extern std::atomic<uint64_t> g_dupActive;

// dups: look for duplicate files instead (dups.inl)
void StartRecursive(Tab& t, bool dups = false) {
    if (t.path.empty() || IsVirtual(t.path) || !t.list || t.list->error) {
        S.message = L"Subfolder search needs a folder";
        Invalidate();
        return;
    }
    uint64_t gen = ++g_recActive;
    for (auto& tp : g_tabs)
        if (tp->recursive && tp->recGen != gen) tp->recDone = true;  // the previous one stays as it is
    Place p = SavePlace(t);
    t.recursive = true;
    t.recDone = false;
    t.recDirs = 0;
    t.recGen = gen;
    t.rec.clear();
    t.recKeys.clear();
    t.dups = dups;
    t.dupHashing = t.dupDone = false;
    t.dupSets.clear();
    t.dupHead.reset();
    Rebuild(t, &p, L"");
    SortBy by = static_cast<SortBy>(g_sortBy.load());
    std::thread(RecThread, t.path, gen, g_cfg.showHidden, by, g_sortAsc.load(), FoldersFirstFor(by)).detach();
    Invalidate();
}

void StopRecursive(Tab& t, bool rebuild) {
    if (!t.recursive) return;
    if (g_recActive == t.recGen) ++g_recActive;
    if (t.dups && g_dupActive == t.dupGen) ++g_dupActive;  // a comparison still running stops
    Place p;
    if (rebuild) p = SavePlace(t);
    t.recursive = false;
    t.dups = t.dupHashing = t.dupDone = false;
    t.filter.clear();
    if (rebuild) {
        // what was selected may be in a subfolder: it's lost, unless it's here
        Rebuild(t, &p, L"");
        EnsureVisible(t);
    } else {
        t.rows.clear();
        t.selMask.clear();
    }
    t.rec.clear();
    t.recKeys.clear();
    t.dupSets.clear();
    t.dupHead.reset();
    Invalidate();
}

void ApplyRecBatch(RecBatch* b) {
    std::unique_ptr<RecBatch> hold(b);
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.recursive || t.recGen != b->gen) continue;
        Place p = SavePlace(t);
        bool wasEmpty = t.rows.empty();
        for (auto& l : b->lists)
            if (t.recKeys.emplace(Key(l->path), l.get()).second) t.rec.push_back(l);  // already there if Jolt reloaded it meanwhile
        t.recDirs = b->dirs;
        t.recDone = b->done;
        if (t.dups && t.recDone && !t.dupHashing && !t.dupDone) DupStart(t);
        if (wasEmpty) {
            BuildRows(t);
            t.selMask.assign(t.rows.size(), 0);
            t.sel = t.anchor = 0;
            if (!t.rows.empty()) t.selMask[0] = 1;
            t.scroll = 0;
        } else {
            Rebuild(t, &p, L"");
        }
        Invalidate();
    }
}

// ---- live results: what is done to them shows up in them

// Is key k the root's or inside it?
bool RecUnder(const std::wstring& k, const std::wstring& root) {
    if (k == root) return true;
    const std::wstring pre = !root.empty() && root.back() == L'\\' ? root : root + L"\\";
    return k.size() > pre.size() && k.compare(0, pre.size(), pre) == 0;
}

// Would the search walk into this subfolder? The same rule as RecThread.
bool RecEnters(const Entry& e) {
    if (!e.isDir() || (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return g_cfg.showHidden || !(e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
}

std::vector<std::shared_ptr<const Listing>>::iterator RecFind(Tab& t, const std::wstring& path) {
    const std::wstring k = Key(path);
    return std::find_if(t.rec.begin(), t.rec.end(), [&](const auto& r) { return r->path.size() == path.size() && Key(r->path) == k; });
}

// A folder Jolt read again (after a delete, a move, a rename...) replaces its old listing in the results.
// Subfolders that appeared are searched too, and the ones that are gone take what they held with them.
bool RecTake(Tab& t, const std::shared_ptr<const Listing>& l) {
    const std::wstring k = Key(l->path), root = Key(t.path);
    if (!RecUnder(k, root) || IsVirtual(l->path)) return false;
    auto at = t.recKeys.count(k) ? RecFind(t, l->path) : t.rec.end();
    const bool known = at != t.rec.end();
    std::vector<std::wstring> gone, found;  // subfolders: keys that left, paths that came
    if (l->error) {
        // only a folder that no longer exists leaves; one that can't be read right now stays as it was
        if (!known || k == root || (l->error != ERROR_FILE_NOT_FOUND && l->error != ERROR_PATH_NOT_FOUND)) return false;
        gone.push_back(k);
    } else if (known) {
        if (SameContent(**at, *l)) return false;
        std::unordered_set<std::wstring> before, now;
        for (const Entry& e : (*at)->entries)
            if (RecEnters(e)) before.insert(Key(Join(l->path, (*at)->name(e))));
        for (const Entry& e : l->entries) {
            if (!RecEnters(e)) continue;
            const std::wstring p = Join(l->path, l->name(e));
            now.insert(Key(p));
            if (!before.count(Key(p))) found.push_back(p);
        }
        for (const std::wstring& b : before)
            if (!now.count(b)) gone.push_back(b);
    } else {
        // new: only below a searched folder, and only where the search would have walked
        if (k == root) return false;
        auto parent = RecFind(t, ParentOf(l->path));
        if (parent == t.rec.end()) return false;
        const int i = FindName(**parent, LeafOf(l->path));
        if (i < 0 || !RecEnters((*parent)->entries[i])) return false;
        for (const Entry& e : l->entries)
            if (RecEnters(e)) found.push_back(Join(l->path, l->name(e)));
    }
    Place p = SavePlace(t);  // before anything is released: the rows point to the listings
    if (!l->error) {
        if (!known) t.rec.push_back(l);
        else *at = l;
        t.recKeys[k] = l.get();
    }
    if (!gone.empty())
        std::erase_if(t.rec, [&](const std::shared_ptr<const Listing>& r) {
            const std::wstring rk = Key(r->path);
            for (const std::wstring& g : gone)
                if (RecUnder(rk, g)) {
                    t.recKeys.erase(rk);
                    return true;
                }
            return false;
        });
    for (const std::wstring& f : found)
        if (!t.recKeys.count(Key(f))) Enqueue(f, JobKind::Revalidate);  // arrives here as a new folder
    Rebuild(t, &p, L"");
    return true;
}

// Only the root folder is watched. The folders of the results on screen and of the selection are read again when
// Jolt comes back to the front, and after a Windows command or a drag that may have changed them.
void RecRevalidateShown() {
    if (g_tabs.empty()) return;  // the window can be activated before the first tab exists
    for (int pn = 0; pn < PaneCount(); ++pn) {
        Tab& t = PT(pn);
        if (!t.recursive) continue;
        std::unordered_set<std::wstring> seen;
        auto add = [&](int i) {
            if (i < 0 || i >= RowCount(t) || IsHeader(t, i) || seen.size() >= 200) return;
            const std::wstring& d = RowDir(t, i);
            if (seen.insert(Key(d)).second) Enqueue(d, JobKind::Revalidate);
        };
        const int first = std::max(0, static_cast<int>(t.scroll / kRow) - 1);
        const int last = first + static_cast<int>(ListHeight() / kRow) + 2;
        for (int i = first; i <= last; ++i) add(i);
        for (int i = 0; i < RowCount(t) && seen.size() < 200; ++i)
            if (IsSel(t, i)) add(i);
    }
}

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

void StartRecursive(Tab& t) {
    if (t.path.empty() || !t.list || t.list->error) {
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
    Rebuild(t, &p, L"");
    SortBy by = static_cast<SortBy>(g_sortBy.load());
    std::thread(RecThread, t.path, gen, g_cfg.showHidden, by, g_sortAsc.load(), FoldersFirstFor(by)).detach();
    Invalidate();
}

void StopRecursive(Tab& t, bool rebuild) {
    if (!t.recursive) return;
    if (g_recActive == t.recGen) ++g_recActive;
    Place p;
    if (rebuild) p = SavePlace(t);
    t.recursive = false;
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
    Invalidate();
}

void ApplyRecBatch(RecBatch* b) {
    std::unique_ptr<RecBatch> hold(b);
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.recursive || t.recGen != b->gen) continue;
        Place p = SavePlace(t);
        bool wasEmpty = t.rows.empty();
        for (auto& l : b->lists) t.rec.push_back(l);
        t.recDirs = b->dirs;
        t.recDone = b->done;
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

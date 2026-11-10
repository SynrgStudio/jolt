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

// ---- When the filter finds nothing in a folder, a dim note under "No matches" says where there is something: how many in
// the subfolders (Ctrl+Shift+F shows them) and how many on the drives (Ctrl+P). Looked for in the background a moment
// after typing stops: the drives' index answers at once; the subfolders are walked up to a limit, on local drives only.
constexpr UINT WM_APP_HINT = WM_APP + 21;
constexpr UINT_PTR kTimerHint = 21;
constexpr int kHintDirs = 4000;       // folders walked at most for the subfolder count
constexpr double kHintWalkMs = 1500;  // or for this long

struct FilterHint {
    uint64_t gen = 0;
    std::wstring folder, filter;  // what it's about
    bool started = false;
    bool sub = false, drives = false;  // whether each can be told
    bool subDone = false, drivesDone = false;
    bool capped = false;  // too many subfolders to walk them all: "at least"
    uint64_t inSub = 0, onDrives = 0;
};
FilterHint g_hint;
std::atomic<uint64_t> g_hintGen{0};
struct HintMsg {
    uint64_t gen;
    bool drives;  // or the subfolders
    uint64_t count;
    bool capped;
};

// The filter is only words, which is what Ctrl+P searches for (no .ext, size, date…).
bool HintPlainWords(const FilterSpec& s) {
    return !s.fuzzy.empty() && s.content.empty() && s.exts.empty() && s.notExts.empty() && s.notNames.empty() && s.globs.empty() &&
           s.notGlobs.empty() && s.tags.empty() && s.notTags.empty() && !s.sized && !s.newerThan && !s.olderThan && s.kind == 0;
}

// Is the tab showing "No matches" for its filter (in the folder, or after a subfolder search that has ended)?
bool HintApplies(const Tab& t) {
    return t.list && !t.list->error && !t.loading && !t.filter.empty() && RowCount(t) == 0 && !t.dups && (!t.recursive || t.recDone);
}

void HintPost(uint64_t gen, bool drives, uint64_t count, bool capped) {
    auto* m = new HintMsg{gen, drives, count, capped};
    if (!PostMessageW(g_hwnd, WM_APP_HINT, 0, reinterpret_cast<LPARAM>(m))) delete m;
}

// Counts what passes the filter in the folder's subfolders, as the subfolder search would show it.
void HintWalk(std::wstring root, FilterSpec spec, bool showHidden, uint64_t gen, int typoMin) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const NearPattern close(spec.fuzzy, typoMin);
    const double t0 = NowMs();
    std::deque<std::wstring> q{root};
    uint64_t count = 0;
    int dirs = 0;
    Listing l;
    while (!q.empty()) {
        if (g_hintGen != gen) return;
        if (dirs >= kHintDirs || NowMs() - t0 > kHintWalkMs) {
            HintPost(gen, false, count, true);
            return;
        }
        const std::wstring p = std::move(q.front());
        q.pop_front();
        l = Listing{};
        EnumerateDir(p, EnumMethod::Nt, l);
        const bool top = dirs++ == 0;  // the folder's own items are the ones the filter already went through
        if (l.error) continue;
        for (const Entry& e : l.entries) {
            if (!showHidden && (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            int sc, percent = 0;
            if (!top && FilterPass(spec, e, l.name(e), e.nameLen, sc, nullptr, close, percent)) ++count;
            if (e.isDir() && !(e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) q.push_back(Join(p, l.name(e)));
        }
    }
    HintPost(gen, false, count, false);
}

// Called while "No matches" is drawn: a new filter (or folder) is looked for once typing stops for a moment.
void HintWant(const Tab& t) {
    if (g_hint.gen && t.filter == g_hint.filter && Key(t.path) == Key(g_hint.folder)) return;
    g_hint = FilterHint{};
    g_hint.gen = ++g_hintGen;  // whatever was being counted for the last one stops
    g_hint.folder = t.path;
    g_hint.filter = t.filter;
    SetTimer(g_hwnd, kTimerHint, 250, nullptr);
}

void HintStart() {
    KillTimer(g_hwnd, kTimerHint);
    const Tab& t = T();
    if (g_hint.started || !HintApplies(t) || t.filter != g_hint.filter || Key(t.path) != Key(g_hint.folder)) return;
    const FilterSpec spec = ParseFilter(t.filter);
    g_hint.started = true;
    g_hint.drives = g_ixCount > 0 && HintPlainWords(spec);
    g_hint.sub = !t.recursive && IsLocalFixed(t.path) && !InArchive(t.path) && spec.content.empty() && spec.tags.empty() && spec.notTags.empty();
    const uint64_t gen = g_hint.gen;
    if (g_hint.drives)
        std::thread([pat = spec.fuzzy, gen] {
            const uint64_t n = IdxCountMatches(pat, g_hintGen, gen);
            if (g_hintGen == gen) HintPost(gen, true, n, false);
        }).detach();
    if (g_hint.sub) std::thread(HintWalk, t.path, spec, g_cfg.showHidden, gen, g_cfg.typoMin).detach();
    Invalidate();
}

void HintDone(const HintMsg& m) {
    if (m.gen != g_hint.gen) return;
    if (m.drives) {
        g_hint.drivesDone = true;
        g_hint.onDrives = m.count;
    } else {
        g_hint.subDone = true;
        g_hint.inSub = m.count;
        g_hint.capped = m.capped;
    }
    Invalidate();
}

// The words Ctrl+P takes with it: the note says there are matches on the drives for this tab's filter. "" otherwise.
std::wstring HintWords() {
    const Tab& t = T();
    if (!HintApplies(t) || t.filter != g_hint.filter || Key(t.path) != Key(g_hint.folder) || !g_hint.drivesDone || !g_hint.onDrives) return L"";
    return ParseFilter(t.filter).fuzzy;
}

void HintGo(int what) { RunCommand(what == 0 ? Cmd::SearchSubfolders : Cmd::GoTo); }

// Under "No matches", the note: each count is a link to what shows them.
void DrawFilterHint(const Tab& t, float L, float R, float y) {
    if (&t != &T() || !HintApplies(t)) return;
    HintWant(t);
    const FilterHint& h = g_hint;
    if (!h.started) return;
    auto key = [](Cmd c) {
        const std::wstring k = ShortcutFor(c);
        return k.empty() ? std::wstring() : L"   ·   " + k;
    };
    auto count = [](uint64_t n) { return Thousands(static_cast<int>(std::min<uint64_t>(n, INT_MAX))); };
    std::vector<std::pair<std::wstring, int>> lines;  // text, and what a click does: 0 subfolders, 1 drives, -1 nothing
    if (h.subDone && h.inSub > 0) lines.push_back({count(h.inSub) + (h.capped ? L"+" : L"") + L" in the subfolders" + key(Cmd::SearchSubfolders), 0});
    if (h.drivesDone && h.onDrives > 0) lines.push_back({count(h.onDrives) + L" on the drives" + key(Cmd::GoTo), 1});
    const bool subNone = h.sub && h.subDone && !h.capped, drivesNone = h.drives && h.drivesDone;
    if (lines.empty() && (h.sub || h.drives) && (!h.sub || subNone) && (!h.drives || drivesNone))
        lines.push_back({h.sub && h.drives ? L"Not in the subfolders or on the drives either" : h.sub ? L"Not in the subfolders either" : L"Not on the drives either", -1});
    for (const auto& [text, act] : lines) {
        const float w = std::min(MeasureW(text, g_fmtCenter.Get()), R - L - 20), x0 = (L + R - w) / 2;
        const bool hot = act >= 0 && IsHover(HitKind::FilterHint, act);
        Color(hot ? cAccent : cFaint);
        Text(text, g_fmtCenter.Get(), L, y, R, y + 22);
        if (act >= 0) {
            if (hot) HLine(x0, x0 + w, y + 17, cAccent);
            g_hits.push_back({D2D1::RectF(x0, y, x0 + w, y + 22), HitKind::FilterHint, act});
        }
        y += 22;
    }
}

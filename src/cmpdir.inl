// Comparing two folders (Ctrl+K → "compare" with two folders selected, or in split view the two panes' folders; also in
// the right-click menu). Both are walked with all their subfolders at once, and a view over the window shows them as
// two lists side by side that scroll together, each thing on the same line on both sides: what is only on one side
// has a dark gap on the other, and what differs has the date and the size that differ lit up, with an arrow from the
// newer one to the older one between the two (≠ when neither is newer). Long runs of things that are the same are
// folded away. Files with the same size and date count as the same without being read; with the same size and another
// date they're read, and if the bytes are the same they count as the same too (files only in the cloud aren't read:
// that would download them). A folder only on one side shows as that folder alone.
// The cursor is on a line and on one of the sides: F5 and F6 copy and move what is selected there to the same place
// on the other side, and Enter compares the two files. After that, and when Jolt comes back to the front, it compares
// again. Included from app.cpp after diff.inl.

constexpr UINT WM_APP_CMP = WM_APP + 24;
constexpr UINT_PTR kTimerCmp = 23;
constexpr int64_t kCmpSameTime = 20000000;  // two seconds: FAT drives and some network drives keep dates to two seconds

enum DdState { DdSame, DdOnlyA, DdOnlyB, DdNewerA, DdNewerB, DdDiffer, DdStates };

struct DdRow {
    const Listing* l[2] = {nullptr, nullptr};  // the folder it is in, on each side
    int e[2] = {-1, -1};                       // its entry there; -1: it isn't on that side
    int depth = 0;
    int parent = -1;  // the row of the folder it is in
    int state = DdSame;
    int inside = 0;  // a folder on both sides: how many differences there are below it
};

struct DdResult {
    uint64_t gen = 0;
    std::vector<std::shared_ptr<Listing>> lists;
    std::vector<DdRow> rows;  // as a tree: each folder's things right below it
    int count[DdStates]{};    // how many of each (the same: files only)
    int failed = 0;           // folders that couldn't be read
    std::wstring error;       // one of the two couldn't be read at all
};

std::atomic<uint64_t> g_ddGen{0};
std::atomic<int> g_ddDirs{0}, g_ddReading{0};
std::atomic<bool> g_ddNotify{false};
std::mutex g_cmpCacheM;
std::unordered_map<std::wstring, bool> g_cmpCache;  // two files with their size and dates -> the same bytes

const Entry* DdEntry(const DdRow& r, int s) { return r.e[s] >= 0 ? &r.l[s]->entries[r.e[s]] : nullptr; }
std::wstring DdPath(const DdRow& r, int s) {
    const Entry* e = DdEntry(r, s);
    return e ? Join(r.l[s]->path, r.l[s]->name(*e)) : std::wstring();
}

bool CmpSameBytes(const std::wstring& a, const std::wstring& b, uint64_t gen) {
    HANDLE f[2];
    const std::wstring p[2] = {a, b};
    for (int s = 0; s < 2; ++s)
        f[s] = CreateFileW(ToExtendedPath(p[s]).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool same = f[0] != INVALID_HANDLE_VALUE && f[1] != INVALID_HANDLE_VALUE;
    static thread_local std::vector<uint8_t> ba(1 << 20), bb(1 << 20);
    while (same && g_ddGen == gen) {
        DWORD ga = 0, gb = 0;
        if (!ReadFile(f[0], ba.data(), static_cast<DWORD>(ba.size()), &ga, nullptr) || !ReadFile(f[1], bb.data(), static_cast<DWORD>(bb.size()), &gb, nullptr) ||
            ga != gb) {
            same = false;
            break;
        }
        if (ga == 0) break;
        same = memcmp(ba.data(), bb.data(), ga) == 0;
    }
    for (HANDLE h : f)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return same && g_ddGen == gen;
}

void DdThread(std::wstring rootA, std::wstring rootB, uint64_t gen, bool showHidden) {
    auto out = std::make_unique<DdResult>();
    out->gen = gen;
    const std::wstring root[2] = {rootA, rootB};
    std::vector<int> checks;  // rows whose bytes tell, later
    double last = NowMs();
    auto progress = [&] {
        if (NowMs() - last < 150) return;
        last = NowMs();
        if (!g_ddNotify.exchange(true)) PostMessageW(g_hwnd, WM_APP_CMP, 0, 0);
    };
    auto shown = [&](const Entry& e) { return showHidden || !(e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)); };
    // a folder that is on both sides: its things, folders first, and each folder's own right below it
    std::function<void(const std::wstring&, int, int)> walk = [&](const std::wstring& rel, int depth, int parent) {
        if (g_ddGen != gen) return;
        std::shared_ptr<Listing> l[2];
        for (int s = 0; s < 2; ++s) {
            l[s] = std::make_shared<Listing>();
            EnumerateDir(rel.empty() ? root[s] : Join(root[s], rel.c_str()), EnumMethod::Nt, *l[s]);
        }
        g_ddDirs += 2;
        if (l[0]->error || l[1]->error) {
            if (rel.empty()) {
                const int s = l[0]->error ? 0 : 1;
                out->error = LeafOf(root[s]) + L": " + ErrorText(l[s]->error);
            } else {
                ++out->failed;  // what is in it can't be told apart
            }
            return;
        }
        out->lists.push_back(l[0]);
        out->lists.push_back(l[1]);
        std::unordered_map<std::wstring, int> inB;
        for (size_t j = 0; j < l[1]->entries.size(); ++j) {
            const Entry& e = l[1]->entries[j];
            if (shown(e)) inB.emplace(Key(std::wstring(l[1]->name(e), e.nameLen)), static_cast<int>(j));
        }
        std::vector<std::pair<int, int>> pairs;
        std::vector<uint8_t> matched(l[1]->entries.size(), 0);
        for (size_t i = 0; i < l[0]->entries.size(); ++i) {
            const Entry& ea = l[0]->entries[i];
            if (!shown(ea)) continue;
            const auto it = inB.find(Key(std::wstring(l[0]->name(ea), ea.nameLen)));
            if (it != inB.end()) matched[it->second] = 1;
            pairs.push_back({static_cast<int>(i), it == inB.end() ? -1 : it->second});
        }
        for (size_t j = 0; j < l[1]->entries.size(); ++j)
            if (!matched[j] && shown(l[1]->entries[j])) pairs.push_back({-1, static_cast<int>(j)});
        auto one = [&](const std::pair<int, int>& p) -> const Entry& { return p.first >= 0 ? l[0]->entries[p.first] : l[1]->entries[p.second]; };
        auto name = [&](const std::pair<int, int>& p) { return p.first >= 0 ? l[0]->name(l[0]->entries[p.first]) : l[1]->name(l[1]->entries[p.second]); };
        std::stable_sort(pairs.begin(), pairs.end(), [&](const std::pair<int, int>& x, const std::pair<int, int>& y) {
            if (one(x).isDir() != one(y).isDir()) return one(x).isDir();
            return StrCmpLogicalW(name(x), name(y)) < 0;
        });
        for (const auto& p : pairs) {
            DdRow r;
            r.l[0] = l[0].get();
            r.l[1] = l[1].get();
            r.e[0] = p.first;
            r.e[1] = p.second;
            r.depth = depth;
            r.parent = parent;
            const Entry *ea = DdEntry(r, 0), *eb = DdEntry(r, 1);
            bool into = false, check = false;
            if (!eb) {
                r.state = DdOnlyA;
            } else if (!ea) {
                r.state = DdOnlyB;
            } else if (ea->isDir() && eb->isDir()) {  // links to folders: being on both sides is enough
                into = !(ea->attrs & FILE_ATTRIBUTE_REPARSE_POINT) && !(eb->attrs & FILE_ATTRIBUTE_REPARSE_POINT);
            } else if (ea->isDir() != eb->isDir()) {  // a folder on one side, a file on the other
                r.state = DdDiffer;
            } else {
                const int64_t dt = ea->mtime - eb->mtime;
                const bool sameTime = dt <= kCmpSameTime && dt >= -kCmpSameTime;
                if (ea->size == eb->size && sameTime) {
                } else if (ea->size == eb->size && !OnlineOnly(ea->attrs) && !OnlineOnly(eb->attrs)) {
                    check = true;
                } else {
                    r.state = sameTime ? DdDiffer : dt > 0 ? DdNewerA : DdNewerB;
                }
            }
            const int at = static_cast<int>(out->rows.size());
            out->rows.push_back(r);
            if (check) checks.push_back(at);
            if (into) walk(rel.empty() ? std::wstring(name(p)) : rel + L"\\" + name(p), depth + 1, at);
        }
        progress();
    };
    walk(L"", 0, -1);
    g_ddReading = static_cast<int>(checks.size());
    for (int at : checks) {
        if (g_ddGen != gen) break;
        DdRow& r = out->rows[at];
        const Entry &ea = *DdEntry(r, 0), &eb = *DdEntry(r, 1);
        const std::wstring pa = DdPath(r, 0), pb = DdPath(r, 1);
        const std::wstring key = Key(pa) + L"|" + Key(pb) + L"|" + std::to_wstring(ea.size) + L"|" + std::to_wstring(ea.mtime) + L"|" + std::to_wstring(eb.mtime);
        int known = -1;
        {
            std::lock_guard<std::mutex> lock(g_cmpCacheM);
            if (auto it = g_cmpCache.find(key); it != g_cmpCache.end()) known = it->second;
        }
        const bool same = known >= 0 ? known == 1 : CmpSameBytes(pa, pb, gen);
        if (g_ddGen != gen) break;
        if (known < 0) {
            std::lock_guard<std::mutex> lock(g_cmpCacheM);
            if (g_cmpCache.size() > 20000) g_cmpCache.clear();
            g_cmpCache[key] = same;
        }
        --g_ddReading;
        progress();
        if (!same) r.state = ea.mtime > eb.mtime ? DdNewerA : DdNewerB;
    }
    if (g_ddGen != gen) return;
    for (const DdRow& r : out->rows) {
        if (r.state == DdSame) {
            if (!DdEntry(r, 0)->isDir()) ++out->count[DdSame];
            continue;
        }
        ++out->count[r.state];
        for (int p = r.parent; p >= 0; p = out->rows[p].parent) ++out->rows[p].inside;
    }
    DdResult* raw = out.release();
    if (!PostMessageW(g_hwnd, WM_APP_CMP, 1, reinterpret_cast<LPARAM>(raw))) delete raw;
}

// ---- the view
struct DdShow {
    int row;
    int fold;  // > 0: instead, this many things that are the same folded away, from row on
};
enum class DdK { None, Close, Prev, Next, All, Fold, Overview };
struct DdHit {
    D2D1_RECT_F r;
    DdK k;
    int idx = 0;
};

struct DirDiffView {
    bool open = false, busy = false, again = false;
    std::wstring root[2], label[2];
    std::unique_ptr<DdResult> r;
    std::vector<DdShow> show;
    std::vector<int> rowAt;                   // where each row is in show; -1: folded away
    std::vector<int> diffs;                   // the rows that differ, in order
    std::unordered_set<std::wstring> opened;  // folds opened by a click (by where their first row is)
    bool all = false;                         // everything, nothing folded
    float scroll = 0, viewH = 0;
    int cur = -1, side = 0, anchor = -1;  // the cursor's row and side; where a Shift selection starts
    std::vector<uint8_t> sel;             // by row
    bool goFirst = false;                 // to the first difference once the view's height is known
    D2D1_RECT_F box{}, body{}, overview{};
    float mid = 0;  // where the two sides meet
    std::vector<DdHit> hits;
    DdHit hover{};
    bool hovering = false;
} g_dd;
constexpr int kDdContext = 2;
// The two sides' colors. Not the file comparison's red and green: nothing here was removed or added, it is only in one
// place or the other.
constexpr UINT32 kDdSide[2] = {0x4CC2D6, 0xB392F0};
constexpr float kDdRow = 24.f, kDdGutter = 30.f;

// Where a row is below the two folders, the same on both sides: what it is known by from one comparison to the next.
std::wstring DdRel(const DdRow& r) {
    const int s = r.e[0] >= 0 ? 0 : 1;
    std::wstring p = DdPath(r, s).substr(g_dd.root[s].size());
    while (!p.empty() && p[0] == L'\\') p.erase(0, 1);
    return Key(p);
}

void DdBuild() {
    g_dd.show.clear();
    g_dd.diffs.clear();
    g_dd.rowAt.clear();
    if (!g_dd.r) return;
    const std::vector<DdRow>& rows = g_dd.r->rows;
    const int n = static_cast<int>(rows.size());
    g_dd.rowAt.assign(n, -1);
    auto put = [&](int k) {
        g_dd.rowAt[k] = static_cast<int>(g_dd.show.size());
        g_dd.show.push_back({k, 0});
    };
    for (int i = 0; i < n;) {
        if (rows[i].state != DdSame || rows[i].inside > 0) {  // a difference, or a folder with some below
            if (rows[i].state != DdSame) g_dd.diffs.push_back(i);
            put(i++);
            continue;
        }
        int j = i;
        while (j < n && rows[j].state == DdSame && rows[j].inside == 0) ++j;
        const int top = i == 0 ? 0 : kDdContext, bot = j == n ? 0 : kDdContext;
        if (g_dd.all || j - i <= top + bot + 2 || g_dd.opened.count(DdRel(rows[i + top]))) {
            for (int k = i; k < j; ++k) put(k);
        } else {
            for (int k = i; k < i + top; ++k) put(k);
            g_dd.show.push_back({i + top, j - i - top - bot});
            for (int k = j - bot; k < j; ++k) put(k);
        }
        i = j;
    }
}

void DdClamp() {
    const float total = g_dd.show.size() * kDdRow;
    g_dd.scroll = std::clamp(g_dd.scroll, 0.f, std::max(0.f, total - g_dd.viewH + kDdRow));
}

void DdSee(int row) {  // the row in sight
    if (row < 0 || row >= static_cast<int>(g_dd.rowAt.size()) || g_dd.rowAt[row] < 0) return;
    const float y = g_dd.rowAt[row] * kDdRow;
    if (y < g_dd.scroll) g_dd.scroll = y;
    else if (y + kDdRow > g_dd.scroll + g_dd.viewH) g_dd.scroll = y + kDdRow - g_dd.viewH;
    DdClamp();
}

// The cursor to a row: alone, or selecting from where Shift was first held.
void DdCursor(int row, bool extend, bool toggle = false) {
    if (!g_dd.r || row < 0 || row >= static_cast<int>(g_dd.rowAt.size()) || g_dd.rowAt[row] < 0) return;
    if (toggle) {
        g_dd.sel[row] = !g_dd.sel[row];
        g_dd.anchor = row;
    } else {
        std::fill(g_dd.sel.begin(), g_dd.sel.end(), uint8_t(0));
        if (!extend || g_dd.anchor < 0 || g_dd.rowAt[g_dd.anchor] < 0) g_dd.anchor = row;
        const int a = std::min(g_dd.rowAt[g_dd.anchor], g_dd.rowAt[row]), b = std::max(g_dd.rowAt[g_dd.anchor], g_dd.rowAt[row]);
        for (int i = a; i <= b; ++i)
            if (!g_dd.show[i].fold) g_dd.sel[g_dd.show[i].row] = 1;
    }
    g_dd.cur = row;
    DdSee(row);
    Invalidate();
}

// The side a difference is told from: the one that has it, or has it newer.
int DdFrom(const DdRow& r) { return r.state == DdOnlyB || r.state == DdNewerB ? 1 : 0; }

// Which difference the cursor is on, or the one before it; -1 before them all.
int DdDiffAt() {
    return static_cast<int>(std::upper_bound(g_dd.diffs.begin(), g_dd.diffs.end(), g_dd.cur) - g_dd.diffs.begin()) - 1;
}

// To difference k: the cursor on it, on its side, a third of the way down.
void DdGo(int k) {
    const int n = static_cast<int>(g_dd.diffs.size());
    if (n == 0) return;
    const int row = g_dd.diffs[std::clamp(k, 0, n - 1)];
    g_dd.side = DdFrom(g_dd.r->rows[row]);
    DdCursor(row, false);
    const float y = g_dd.rowAt[row] * kDdRow;
    if (y < g_dd.scroll + kDdRow || y > g_dd.scroll + g_dd.viewH - 2 * kDdRow) g_dd.scroll = y - g_dd.viewH / 3;
    DdClamp();
}

void DdStep(int dir) {
    if (g_dd.diffs.empty()) return;
    const int at = DdDiffAt();
    const bool on = at >= 0 && g_dd.diffs[at] == g_dd.cur;
    DdGo(dir > 0 ? at + 1 : on ? at - 1 : at);
}

void DdRun() {
    g_dd.busy = true;
    g_ddDirs = 0;
    g_ddReading = 0;
    std::thread(DdThread, g_dd.root[0], g_dd.root[1], ++g_ddGen, g_cfg.showHidden).detach();
}

void DirDiffClose() {
    if (!g_dd.open) return;
    ++g_ddGen;
    KillTimer(g_hwnd, kTimerCmp);
    g_dd = DirDiffView{};
    Invalidate();
}

// Compares again: now, or as soon as the comparison going on is done.
void DdAgain() {
    if (!g_dd.open) return;
    if (g_dd.busy) g_dd.again = true;
    else DdRun();
    Invalidate();
}
void DdSoon() { SetTimer(g_hwnd, kTimerCmp, 300, nullptr); }
void DdTimer() {
    KillTimer(g_hwnd, kTimerCmp);
    DdAgain();
}
// Jolt comes back to the front: what changed meanwhile shows.
void DdActivated() {
    if (g_dd.open && g_dd.r && !g_dd.busy) DdRun();
}

// How each side is called: the folder's name, or more of its path when both have the same name.
void DdLabels() {
    std::wstring a = g_dd.root[0], b = g_dd.root[1];
    std::wstring la = LeafOf(a), lb = LeafOf(b);
    for (int up = 0; up < 3 && Key(la) == Key(lb); ++up) {
        a = ParentOf(a);
        b = ParentOf(b);
        if (a.empty() || b.empty()) break;
        la = LeafOf(a) + L"\\" + la;
        lb = LeafOf(b) + L"\\" + lb;
    }
    if (Key(la) == Key(lb)) {
        la = g_dd.root[0];
        lb = g_dd.root[1];
    }
    g_dd.label[0] = la;
    g_dd.label[1] = lb;
}

void CompareFolders(const std::wstring& a, const std::wstring& b) {
    const std::wstring ka = Key(a), kb = Key(b);
    if (ka == kb) {
        S.message = L"That's the same folder twice";
    } else if (RecUnder(ka, kb) || RecUnder(kb, ka)) {
        S.message = L"One of them is inside the other: there's nothing to compare";
    } else if (a.empty() || b.empty() || IsVirtual(a) || IsVirtual(b) || InArchive(a) || InArchive(b)) {
        S.message = L"Only folders on a drive can be compared";
    } else {
        PopupClose();
        PaletteClose();
        ViewEditorClose();
        MenuEditorClose();
        SettingsClose();
        CancelRename();
        ++g_ddGen;
        g_dd = DirDiffView{};
        g_dd.open = true;
        g_dd.root[0] = a;
        g_dd.root[1] = b;
        DdLabels();
        DdRun();
        S.message.clear();
    }
    Invalidate();
}

void DdApply(WPARAM wp, LPARAM lp) {
    if (wp == 0) {  // progress
        g_ddNotify = false;
        Invalidate();
        return;
    }
    std::unique_ptr<DdResult> r(reinterpret_cast<DdResult*>(lp));
    if (!g_dd.open || r->gen != g_ddGen) return;
    g_dd.busy = false;
    if (!r->error.empty()) {
        S.message = L"Couldn't compare: " + r->error;
        DirDiffClose();
        return;
    }
    // The cursor and the selection stay on what they were on. A difference that is gone or the same now (it was just
    // copied over) hands the cursor to the difference that follows.
    const bool first = !g_dd.r;
    const int was = g_dd.cur;
    std::wstring on;
    std::unordered_set<std::wstring> selected;
    bool wasDiff = false;
    if (!first) {
        const std::vector<DdRow>& old = g_dd.r->rows;
        if (was >= 0 && was < static_cast<int>(old.size())) {
            on = DdRel(old[was]);
            wasDiff = old[was].state != DdSame;
        }
        for (size_t i = 0; i < old.size(); ++i)
            if (g_dd.sel[i]) selected.insert(DdRel(old[i]));
    }
    g_dd.r = std::move(r);
    const std::vector<DdRow>& rows = g_dd.r->rows;
    const int n = static_cast<int>(rows.size());
    g_dd.sel.assign(n, 0);
    g_dd.anchor = g_dd.cur = -1;
    DdBuild();
    if (first) {
        g_dd.goFirst = true;
    } else {
        int found = -1;
        for (int i = 0; i < n && (!on.empty() || !selected.empty()); ++i) {
            if (g_dd.rowAt[i] < 0) continue;
            const std::wstring k = DdRel(rows[i]);
            if (k == on) found = i;
            if (selected.count(k)) g_dd.sel[i] = 1;
        }
        if (wasDiff && !g_dd.diffs.empty() && (found < 0 || rows[found].state == DdSame)) {
            const auto it = std::lower_bound(g_dd.diffs.begin(), g_dd.diffs.end(), found >= 0 ? found : was);
            g_dd.cur = it == g_dd.diffs.end() ? g_dd.diffs.back() : *it;
            g_dd.side = DdFrom(rows[g_dd.cur]);
            std::fill(g_dd.sel.begin(), g_dd.sel.end(), uint8_t(0));
            g_dd.sel[g_dd.cur] = 1;
            DdSee(g_dd.cur);
        } else {
            g_dd.cur = found;
        }
        g_dd.anchor = g_dd.cur;
        DdClamp();
    }
    if (g_dd.again) {
        g_dd.again = false;
        DdRun();
    }
    Invalidate();
}

// What it found, in a line.
std::wstring DdStatus() {
    if (!g_dd.r) {
        const int reading = g_ddReading.load();
        return L"Comparing   ·   " + Thousands(g_ddDirs.load()) + L" folders read" + (reading ? L"   ·   " + Thousands(reading) + L" files to read" : L"") + L"…";
    }
    const DdResult& r = *g_dd.r;
    const int differ = static_cast<int>(g_dd.diffs.size());
    const std::wstring same = Thousands(r.count[DdSame]) + (r.count[DdSame] == 1 ? L" file" : L" files") + L" the same";
    const std::wstring failed = r.failed ? L"   ·   " + Thousands(r.failed) + (r.failed == 1 ? L" folder couldn't be read" : L" folders couldn't be read") : L"";
    const std::wstring busy = g_dd.busy ? L"   ·   comparing again…" : L"";
    if (!differ) return L"No differences: " + same + failed + busy;
    return Thousands(differ) + (differ == 1 ? L" difference" : L" differences") + L"   ·   " + same + failed + busy;
}

// ---- drawing
void DdHeader(float x0, float y0, float R) {
    const float mid = std::floor((x0 + R) / 2);
    for (int s = 0; s < 2; ++s) {
        const float l = s ? mid + 14 : x0 + 18, rr = s ? R - 48 : mid - 14;
        Fill(D2D1::RectF(l, y0 + 12, l + 3, y0 + 46), kDdSide[s], 0.8f);
        Color(cText);
        Text(LeafOf(g_dd.root[s]), g_fmtNameBold.Get(), l + 12, y0 + 8, rr, y0 + 28);
        std::wstring info;
        if (g_dd.r) {
            const int only = g_dd.r->count[s ? DdOnlyB : DdOnlyA], newer = g_dd.r->count[s ? DdNewerB : DdNewerA];
            if (only) info = Thousands(only) + L" only here";
            if (newer) info += (info.empty() ? L"" : L"  ·  ") + Thousands(newer) + L" newer";
        }
        const float iw = info.empty() ? 0 : MeasureW(info, g_fmtStatus.Get());
        Color(cFaint);
        Text(ParentOf(g_dd.root[s]), g_fmtStatus.Get(), l + 12, y0 + 28, std::max(l + 40, rr - iw - 14), y0 + 48);
        if (!info.empty()) {
            Color(cDim);
            Text(info, g_fmtStatusR.Get(), rr - iw - 4, y0 + 28, rr, y0 + 48);
        }
    }
    Fill(D2D1::RectF(mid, y0 + 10, mid + 1, y0 + 48), cBorder);
}

void DrawDirDiff(float W, float H) {
    if (!g_dd.open) return;
    g_dd.hits.clear();
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    const float x0 = 12, y0 = kTabH + 6, R = W - 12, B = H - 12;
    g_dd.box = D2D1::RectF(x0, y0, R, B);
    Round(g_dd.box, 10, cBg);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_dd.box, 10, 10), g_brush.Get(), 1.f);
    auto isHover = [&](DdK k, int idx = 0) { return g_dd.hovering && g_dd.hover.k == k && g_dd.hover.idx == idx; };
    DdHeader(x0, y0, R);
    const D2D1_RECT_F cb = D2D1::RectF(R - 40, y0 + 13, R - 12, y0 + 41);
    if (isHover(DdK::Close)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(DdK::Close) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_dd.hits.push_back({cb, DdK::Close});
    HLine(x0, R, y0 + 56);

    const float by = y0 + 57, bb = by + 36;
    g_dd.body = D2D1::RectF(x0 + 1, bb + 1, R - 1, B - 26);
    g_dd.viewH = g_dd.body.bottom - g_dd.body.top;
    if (g_dd.goFirst && g_dd.viewH > 0) {
        g_dd.goFirst = false;
        DdGo(0);
    }
    DdClamp();

    // the bar: what it found, and the buttons
    Color(cText);
    Text(DdStatus(), g_fmtStatus.Get(), x0 + 18, by, R - 380, bb);
    const int n = static_cast<int>(g_dd.diffs.size());
    float bx = R - 14;
    auto button = [&](DdK k, const std::wstring& label, bool on = true) {
        const float w = MeasureW(label, g_fmtStatus.Get()) + 22;
        const D2D1_RECT_F b = D2D1::RectF(bx - w, by + 5, bx, bb - 5);
        Round(b, 6, isHover(k) && on ? 0x3A3A44 : cTop);
        Color(on ? cText : cFaint);
        Text(label, g_fmtCenter.Get(), b.left, b.top, b.right, b.bottom);
        if (on) g_dd.hits.push_back({b, k});
        bx = b.left - 6;
    };
    button(DdK::Next, L"Next  ↓", n > 0);
    button(DdK::Prev, L"↑  Previous", n > 0);
    if (n) {
        const int at = DdDiffAt();
        const bool on = at >= 0 && g_dd.diffs[at] == g_dd.cur;
        const std::wstring where = (on ? Thousands(at + 1) + L" of " : L"") + Thousands(n);
        const float w = MeasureW(where, g_fmtStatus.Get());
        Color(cDim);
        Text(where, g_fmtStatusR.Get(), bx - w - 8, by, bx - 4, bb);
        bx -= w + 18;
    }
    if (g_dd.r) button(DdK::All, g_dd.all ? L"Fold what is the same" : L"Show everything");
    HLine(x0, R, bb);

    const D2D1_RECT_F& body = g_dd.body;
    const float ovW = 12;
    g_dd.overview = D2D1::RectF(body.right - ovW, body.top, body.right, body.bottom);
    const float L = body.left, Rt = body.right - ovW - 2, colW = std::floor((Rt - L - kDdGutter) / 2);
    g_dd.mid = L + colW + kDdGutter / 2;
    if (!g_dd.r) {
        Color(cDim);
        Text(DdStatus(), g_fmtCenter.Get(), L, body.top + 30, Rt, body.top + 60);
    } else {
        const DdResult& res = *g_dd.r;
        const int detail = colW >= 430 ? 2 : colW >= 300 ? 1 : 0;  // the date and the size, the date, or only the name
        g_rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
        const int first = std::max(0, static_cast<int>(g_dd.scroll / kDdRow));
        for (int i = first; i < static_cast<int>(g_dd.show.size()); ++i) {
            const float y = body.top + i * kDdRow - g_dd.scroll;
            if (y >= body.bottom) break;
            const DdShow& sh = g_dd.show[i];
            if (sh.fold) {
                const D2D1_RECT_F fr = D2D1::RectF(L, y, Rt, y + kDdRow);
                Fill(fr, 0xFFFFFF, isHover(DdK::Fold, i) ? 0.07f : 0.025f);
                Color(isHover(DdK::Fold, i) ? cDim : cFaint);
                Text(L"⋯   " + Thousands(sh.fold) + L" the same   ⋯", g_fmtCenter.Get(), L, y, Rt, y + kDdRow);
                g_dd.hits.push_back({fr, DdK::Fold, i});
                continue;
            }
            const DdRow& r = res.rows[sh.row];
            const Entry* e[2] = {DdEntry(r, 0), DdEntry(r, 1)};
            const bool differ = r.state != DdSame, both = e[0] && e[1], files = both && !e[0]->isDir() && !e[1]->isDir();
            const int64_t dt = both ? e[0]->mtime - e[1]->mtime : 0;
            const bool sizeDiffers = files && e[0]->size != e[1]->size, dateDiffers = files && differ && (dt > kCmpSameTime || dt < -kCmpSameTime);
            const bool selected = g_dd.sel[sh.row] != 0;
            for (int s = 0; s < 2; ++s) {
                const float cx = s ? L + colW + kDdGutter : L, cr = cx + colW;
                const UINT32 tint = kDdSide[s];
                const D2D1_RECT_F cell = D2D1::RectF(cx, y, cr, y + kDdRow);
                if (differ && !e[s]) Fill(cell, 0x000000, 0.22f);  // it isn't on this side
                else if (differ) Fill(cell, tint, both ? 0.09f : 0.16f);
                if (selected) Fill(cell, 0xFFFFFF, s == g_dd.side ? 0.09f : 0.04f);
                if (selected && s == g_dd.side) Fill(D2D1::RectF(cx, y, cx + 3, y + kDdRow), cAccent);
                if (sh.row == g_dd.cur && s == g_dd.side) {
                    Color(cAccent, 0.9f);
                    g_rt->DrawRectangle(D2D1::RectF(cx + 0.5f, y + 0.5f, cr - 0.5f, y + kDdRow - 0.5f), g_brush.Get(), 1.f);
                }
                if (!e[s]) continue;
                const wchar_t* name = r.l[s]->name(*e[s]);
                const float ix = cx + 12 + r.depth * 16.f, a = differ ? 1.f : 0.75f;
                DrawIcon(Join(r.l[s]->path, name), e[s], name, e[s]->nameLen, false, ix, y + (kDdRow - kIcon) / 2, a);
                const float sizeR = cr - 12, sizeL = sizeR - 62, dateR = detail == 2 ? sizeL - 14 : sizeR, dateL = dateR - 102;
                const float nameR = (detail == 0 ? cr : dateL) - 12;
                Color(differ ? cText : cDim);
                Text(name, e[s]->nameLen, g_fmtName.Get(), ix + 24, y, nameR, y + kDdRow);
                wchar_t buf[64];
                if (detail >= 1 && e[s]->mtime) {
                    if (dateDiffers) Round(D2D1::RectF(dateL - 5, y + 3, dateR + 5, y + kDdRow - 3), 3, tint, 0.30f);
                    FormatDate(e[s]->mtime, buf, 64);
                    Color(dateDiffers ? cText : differ ? cDim : cFaint);
                    Text(buf, g_fmtRight.Get(), dateL, y, dateR, y + kDdRow);
                }
                if (detail == 2 && !e[s]->isDir()) {
                    if (sizeDiffers) Round(D2D1::RectF(sizeL - 5, y + 3, sizeR + 5, y + kDdRow - 3), 3, tint, 0.30f);
                    FormatSize(e[s]->size, buf, 64);
                    Color(sizeDiffers ? cText : differ ? cDim : cFaint);
                    Text(buf, g_fmtRight.Get(), sizeL, y, sizeR, y + kDdRow);
                }
            }
            // between the two: from the newer one to the older one
            if (r.state == DdNewerA || r.state == DdNewerB) {
                Color(cAccent);
                Glyph(r.state == DdNewerA ? gFwd : gBack, g_fmtCap.Get(), g_dd.mid - 12, y, g_dd.mid + 12, y + kDdRow);
            } else if (r.state == DdDiffer) {
                Color(kDfAmber);
                Text(L"≠", g_fmtCenter.Get(), g_dd.mid - 12, y, g_dd.mid + 12, y + kDdRow);
            }
        }
        g_rt->PopAxisAlignedClip();
        Fill(D2D1::RectF(L + colW, body.top, L + colW + 1, body.bottom), cBorder);
        Fill(D2D1::RectF(L + colW + kDdGutter - 1, body.top, L + colW + kDdGutter, body.bottom), cBorder);
        // the overview: where the differences are in the whole, and what is on screen
        const D2D1_RECT_F& ov = g_dd.overview;
        const float total = std::max({1.f, static_cast<float>(g_dd.show.size()), std::floor(g_dd.viewH / kDdRow)});  // short ones: as on screen
        const float oh = ov.bottom - ov.top;
        Fill(ov, 0xFFFFFF, 0.03f);
        auto shade = [&](size_t i) -> UINT32 {  // 0: nothing to mark
            const int st = g_dd.show[i].fold ? DdSame : res.rows[g_dd.show[i].row].state;
            return st == DdSame ? 0 : st == DdOnlyA ? kDdSide[0] : st == DdOnlyB ? kDdSide[1] : kDfAmber;
        };
        for (size_t i = 0; i < g_dd.show.size();) {
            const UINT32 c = shade(i);
            if (!c) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < g_dd.show.size() && shade(j) == c) ++j;
            const float t0 = ov.top + oh * i / total, t1 = std::max(t0 + 2, ov.top + oh * j / total);
            Fill(D2D1::RectF(ov.left + 2, t0, ov.right - 2, t1), c, 0.85f);
            i = j;
        }
        const float vt = ov.top + oh * (g_dd.scroll / kDdRow) / total, vb = std::min(ov.bottom, vt + oh * (g_dd.viewH / kDdRow) / total);
        Fill(D2D1::RectF(ov.left, vt, ov.right, std::max(vt + 4, vb)), 0xFFFFFF, 0.10f);
        g_dd.hits.push_back({ov, DdK::Overview});
    }
    HLine(x0, R, B - 26);
    Color(S.message.empty() ? cFaint : cText);
    Text(!S.message.empty() ? S.message
                            : ShortcutFor(Cmd::CopyToOther) + L" copies what is selected to the other side, " + ShortcutFor(Cmd::MoveToOther) +
                                  L" moves it   ·   ← → pick the side   ·   Enter compares the two files   ·   F7 next difference   ·   A shows everything   ·   Esc closes",
         g_fmtStatus.Get(), x0 + 18, B - 25, R - 18, B - 2);
}

// ---- what it does
// F5 / F6: what is selected, from the cursor's side to the same place on the other.
void DdTransfer(bool move) {
    if (!g_dd.r) return;
    const int s = g_dd.side;
    std::vector<std::wstring> files, dests;
    int missing = 0;
    for (size_t i = 0; i < g_dd.r->rows.size(); ++i) {
        const DdRow& r = g_dd.r->rows[i];
        if (!g_dd.sel[i] || r.state == DdSame) continue;
        if (!DdEntry(r, s)) {
            ++missing;
            continue;
        }
        files.push_back(DdPath(r, s));
        dests.push_back(r.l[1 - s]->path);
    }
    if (files.empty()) {
        S.message = missing ? std::wstring(missing == 1 ? L"It isn't" : L"They aren't") + L" on this side: ← → pick the side to copy from" : L"Select a difference to copy it to the other side";
        Invalidate();
        return;
    }
    if (FileOp(move ? FileOpKind::Move : FileOpKind::Copy, files, dests[0], 0, nullptr, nullptr, &dests)) {
        const int n = static_cast<int>(files.size());
        S.message = (move ? L"Moved " : L"Copied ") + (n == 1 ? LeafOf(files[0]) : Thousands(n) + L" items") + L" to " + g_dd.label[1 - s];
        if (missing) S.message += L"; " + Thousands(missing) + (missing == 1 ? L" isn't" : L" aren't") + L" on this side";
    }
    DdSoon();
    Invalidate();
}

// Enter: the cursor's file with its other copy.
void DdEnter() {
    if (!g_dd.r || g_dd.cur < 0) return;
    const DdRow& r = g_dd.r->rows[g_dd.cur];
    const Entry *a = DdEntry(r, 0), *b = DdEntry(r, 1);
    const std::wstring name = LeafOf(DdPath(r, a ? 0 : 1));
    if (!a || !b) S.message = name + L" is only on one side: " + ShortcutFor(Cmd::CopyToOther) + L" copies it to the other";
    else if (a->isDir() && b->isDir()) S.message = name + L" is a folder on both sides: what differs in it is below";
    else if (a->isDir() || b->isDir()) S.message = name + L" is a folder on one side and a file on the other";
    else CompareFiles(DdPath(r, 0), DdPath(r, 1));
    Invalidate();
}

void DdToggleAll() {
    if (!g_dd.r) return;
    // the row at the top of the view stays there
    const int top = std::clamp(static_cast<int>(g_dd.scroll / kDdRow), 0, std::max(0, static_cast<int>(g_dd.show.size()) - 1));
    const int row = g_dd.show.empty() ? 0 : g_dd.show[top].row;
    g_dd.all = !g_dd.all;
    DdBuild();
    for (size_t i = 0; i < g_dd.show.size(); ++i)
        if (g_dd.show[i].row >= row || (g_dd.show[i].fold && g_dd.show[i].row + g_dd.show[i].fold > row)) {
            g_dd.scroll = i * kDdRow;
            break;
        }
    if (g_dd.cur >= 0 && g_dd.rowAt[g_dd.cur] < 0) g_dd.cur = g_dd.anchor = -1;
    for (size_t i = 0; i < g_dd.sel.size(); ++i)
        if (g_dd.rowAt[i] < 0) g_dd.sel[i] = 0;
    DdClamp();
    Invalidate();
}

// The cursor up or down by so many lines, over the folds.
void DdMove(int by, bool extend) {
    if (!g_dd.r || g_dd.show.empty()) return;
    const int n = static_cast<int>(g_dd.show.size()), dir = by < 0 ? -1 : 1;
    const int at = g_dd.cur >= 0 && g_dd.rowAt[g_dd.cur] >= 0 ? g_dd.rowAt[g_dd.cur] : std::clamp(static_cast<int>(g_dd.scroll / kDdRow), 0, n - 1) - dir;
    int to = -1;
    for (int left = std::abs(by), i = at + dir; i >= 0 && i < n; i += dir) {
        if (g_dd.show[i].fold) continue;
        to = i;
        if (--left <= 0) break;
    }
    if (to >= 0) DdCursor(g_dd.show[to].row, extend);
}

// ---- mouse and keyboard
int DdHitAt(float x, float y) {
    for (int i = static_cast<int>(g_dd.hits.size()) - 1; i >= 0; --i)
        if (InRect(g_dd.hits[i].r, x, y)) return i;
    return -1;
}

void DirDiffMouseMove(float x, float y) {
    const int h = DdHitAt(x, y);
    const bool was = g_dd.hovering;
    const DdHit old = g_dd.hover;
    g_dd.hovering = h >= 0;
    g_dd.hover = h >= 0 ? g_dd.hits[h] : DdHit{};
    if (was != g_dd.hovering || old.k != g_dd.hover.k || old.idx != g_dd.hover.idx) Invalidate();
}

void DirDiffClick(float x, float y, bool twice) {
    if (!InRect(g_dd.box, x, y)) return;  // the view fills the window: a click outside it is on the tabs
    S.message.clear();
    Invalidate();
    const int hi = DdHitAt(x, y);
    if (hi >= 0 && g_dd.hits[hi].k != DdK::None) {
        const DdHit h = g_dd.hits[hi];
        switch (h.k) {
            case DdK::Close: DirDiffClose(); return;
            case DdK::Next: DdStep(1); return;
            case DdK::Prev: DdStep(-1); return;
            case DdK::All: DdToggleAll(); return;
            case DdK::Fold:
                if (g_dd.r && h.idx >= 0 && h.idx < static_cast<int>(g_dd.show.size()) && g_dd.show[h.idx].fold) {
                    g_dd.opened.insert(DdRel(g_dd.r->rows[g_dd.show[h.idx].row]));
                    DdBuild();
                    DdClamp();
                }
                return;
            case DdK::Overview: {
                const float f = (y - g_dd.overview.top) / std::max(1.f, g_dd.overview.bottom - g_dd.overview.top);
                const float total = std::max(static_cast<float>(g_dd.show.size()), std::floor(g_dd.viewH / kDdRow));
                g_dd.scroll = f * total * kDdRow - g_dd.viewH / 2;
                DdClamp();
                return;
            }
            default: return;
        }
    }
    if (!g_dd.r || !InRect(g_dd.body, x, y)) return;
    const int i = static_cast<int>(std::floor((y - g_dd.body.top + g_dd.scroll) / kDdRow));
    if (i < 0 || i >= static_cast<int>(g_dd.show.size()) || g_dd.show[i].fold) return;
    g_dd.side = x >= g_dd.mid ? 1 : 0;
    DdCursor(g_dd.show[i].row, KeyDown(VK_SHIFT), KeyDown(VK_CONTROL));
    if (twice) DdEnter();
}

void DirDiffWheel(int delta) {
    g_dd.scroll -= delta / 120.f * 3 * kDdRow;
    DdClamp();
    Invalidate();
}

void DirDiffKey(WPARAM vk) {
    const bool alt = KeyDown(VK_MENU), shift = KeyDown(VK_SHIFT), ctrl = KeyDown(VK_CONTROL);
    const int page = std::max(1, static_cast<int>(g_dd.viewH / kDdRow) - 2);
    S.message.clear();
    Invalidate();
    const Cmd cmd = FindCommand((ctrl ? ModCtrl : 0) | (shift ? ModShift : 0) | (alt ? ModAlt : 0), static_cast<UINT>(vk));
    if (cmd == Cmd::CopyToOther || cmd == Cmd::MoveToOther) {
        DdTransfer(cmd == Cmd::MoveToOther);
        return;
    }
    if (cmd == Cmd::Reload) {
        DdAgain();
        return;
    }
    switch (vk) {
        case VK_ESCAPE: DirDiffClose(); break;
        case VK_F7: DdStep(shift ? -1 : 1); break;
        case 'N': DdStep(1); break;
        case 'P': DdStep(-1); break;
        case 'A':
            if (!ctrl) {
                DdToggleAll();
            } else if (g_dd.r) {  // every difference
                std::fill(g_dd.sel.begin(), g_dd.sel.end(), uint8_t(0));
                for (int row : g_dd.diffs) g_dd.sel[row] = 1;
            }
            break;
        case VK_DOWN: alt ? DdStep(1) : DdMove(1, shift); break;
        case VK_UP: alt ? DdStep(-1) : DdMove(-1, shift); break;
        case VK_NEXT: DdMove(page, shift); break;
        case VK_PRIOR: DdMove(-page, shift); break;
        case VK_HOME: DdMove(-INT_MAX / 2, shift); break;
        case VK_END: DdMove(INT_MAX / 2, shift); break;
        case VK_LEFT: g_dd.side = 0; break;
        case VK_RIGHT: g_dd.side = 1; break;
        case VK_TAB: g_dd.side = 1 - g_dd.side; break;
        case VK_RETURN: DdEnter(); break;
        default: break;
    }
}

// For the tests: what the view shows, line by line. > the cursor (and its side), * selected.
std::wstring DdDump() {
    if (!g_dd.open) return L"closed";
    std::wstring out = DdStatus();
    if (!g_dd.r) return out;
    const wchar_t* states[DdStates] = {L"=", L"only A", L"only B", L"A newer", L"B newer", L"differ"};
    auto side = [&](const DdRow& r, int s) -> std::wstring {
        const Entry* e = DdEntry(r, s);
        if (!e) return L"—";
        if (e->isDir()) return L"folder";
        wchar_t buf[64];
        FormatSize(e->size, buf, 64);
        return buf;
    };
    for (size_t i = 0; i < g_dd.show.size() && i < 60; ++i) {
        const DdShow& sh = g_dd.show[i];
        if (sh.fold) {
            out += L"\n     [" + std::to_wstring(sh.fold) + L" the same]";
            continue;
        }
        const DdRow& r = g_dd.r->rows[sh.row];
        out += L"\n " + std::wstring(sh.row == g_dd.cur ? (g_dd.side ? L">B" : L">A") : L"  ") + (g_dd.sel[sh.row] ? L"* " : L"  ") + std::wstring(r.depth * 2, L' ') +
               LeafOf(DdPath(r, r.e[0] >= 0 ? 0 : 1)) + L" [" + states[r.state] + L"]: " + side(r, 0) + L" | " + side(r, 1);
    }
    return out;
}

// Would "compare" do something with what is selected? (For the right-click menu.)
bool CompareFits(const Tab& t) {
    if (t.path.empty() || IsBin(t.path) || InArchive(t.path) || IsVirtual(t.path)) return false;
    int n = 0, dirs = 0;
    for (int i = 0; i < RowCount(t); ++i)
        if (IsSel(t, i) && !IsHeader(t, i)) {
            ++n;
            dirs += EntryAt(t, i).isDir();
        }
    if (n == 2) return dirs != 1;
    return n == 1 && g_split;
}

// ---- Ctrl+K → "compare": what it works on
// Two selected here: those two (files, or folders). In split view: a file here with the one with its name in the other
// pane's folder (or else the one selected there), and otherwise the two panes' folders, the left one first.
void CompareCmd() {
    Tab& t = T();
    if (IsBin(t.path) || InArchive(t.path)) {
        S.message = InArchive(t.path) ? L"Inside an archive: extract it first to compare" : L"Things in the Recycle Bin can't be compared";
        Invalidate();
        return;
    }
    std::vector<int> sel;
    for (int i = 0; i < RowCount(t); ++i)
        if (IsSel(t, i) && !IsHeader(t, i)) sel.push_back(i);
    if (sel.size() == 2 && !t.path.empty()) {
        const bool da = EntryAt(t, sel[0]).isDir(), db = EntryAt(t, sel[1]).isDir();
        const std::wstring a = RowPath(t, sel[0]), b = RowPath(t, sel[1]);
        if (da != db) S.message = L"One is a folder and the other a file: select two files, or two folders";
        else if (da) CompareFolders(a, b);
        else if (EntryAt(t, sel[1]).mtime < EntryAt(t, sel[0]).mtime) CompareFiles(b, a);  // the older one on the left
        else CompareFiles(a, b);
        Invalidate();
        return;
    }
    if (sel.size() > 2) {
        S.message = L"Compare works on two: " + Thousands(static_cast<int>(sel.size())) + L" are selected";
        Invalidate();
        return;
    }
    if (g_split) {
        Tab& o = PT(1 - g_act);
        if (o.path.empty() || IsVirtual(o.path) || InArchive(o.path) || t.path.empty() || IsVirtual(t.path)) {
            S.message = L"Comparing needs a folder on a drive in each pane";
            Invalidate();
            return;
        }
        if (sel.size() == 1 && !EntryAt(t, sel[0]).isDir()) {
            const std::wstring here = RowPath(t, sel[0]);
            std::wstring there = Join(o.path, LeafOf(here).c_str());
            const DWORD a = GetFileAttributesW(ToExtendedPath(there).c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
                there.clear();
                if (o.sel >= 0 && o.sel < RowCount(o) && !IsHeader(o, o.sel) && !EntryAt(o, o.sel).isDir()) there = RowPath(o, o.sel);
            }
            if (there.empty()) {
                S.message = L"No " + LeafOf(here) + L" in the other pane: select there the file to compare it with";
                Invalidate();
                return;
            }
            if (g_act == 0) CompareFiles(here, there);
            else CompareFiles(there, here);
            return;
        }
        CompareFolders(PT(0).path, PT(1).path);
        return;
    }
    S.message = L"Select two files or two folders to compare them, or open the other one in the other pane (split view, " + ShortcutFor(Cmd::Split) + L")";
    Invalidate();
}

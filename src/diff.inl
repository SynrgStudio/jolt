// Comparing two files (Ctrl+K → "compare"; the right-click menu with two files selected, or with one file in each
// pane of the split view). They are read in the background. Identical files only say so on the status line, and so do
// files with the same text written another way (line endings, encoding). Text files up to 16 MB show side by side in
// a view over the window: lines removed in red on the left, added in green on the right, changed ones on both sides
// with the part that changed brighter, and long runs of equal lines folded away (a click opens one; A shows them all).
// Other files show where they differ: the bytes around each difference in hex, and how many bytes differ in all.
// Lines are matched like "patience diff": the lines found once on each side anchor the match, and what is left between
// anchors is matched with Myers' algorithm, unless it is too different (then it all counts as changed).
// Included from app.cpp after hash.inl.

constexpr UINT WM_APP_DIFF = WM_APP + 23;
constexpr uint64_t kDiffTextMax = 16ull << 20;  // bigger: compared as bytes
constexpr int kDiffRuns = 5000;                  // runs of differing bytes remembered
constexpr int kDiffMyersMax = 1000;              // differences Myers looks through before giving up on a stretch

struct DiffOp {
    int a, b;      // the line on each side, -1: none
    uint8_t kind;  // 0 the same, 1 changed, 2 only on the left (removed), 3 only on the right (added)
};

struct DiffResult {
    uint64_t gen = 0;
    std::wstring path[2];
    uint64_t size[2]{};
    int64_t time[2]{};
    int kind = 0;       // 0 couldn't, 1 identical, 2 the same text written differently, 3 text that differs, 4 bytes
    std::wstring note;  // 0: why; 2: how they differ
    std::vector<std::wstring> lines[2];
    std::vector<DiffOp> ops;
    int removed = 0, added = 0;   // lines
    std::vector<uint64_t> runs;   // 4: where each run of differing bytes starts
    uint64_t diffBytes = 0;
    bool moreRuns = false;
};

std::atomic<uint64_t> g_diffGen{0};
std::atomic<uint64_t> g_diffDone{0}, g_diffTotal{0};
std::atomic<bool> g_diffNotify{false};
bool g_diffBusy = false;
std::wstring g_diffWhat;  // "a.txt and b.txt", for the status line

// ---- reading
bool DiffReadAll(const std::wstring& p, std::vector<uint8_t>& out, std::wstring& err) {
    HANDLE h = CreateFileW(ToExtendedPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = LeafOf(p) + L": " + ErrorText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t at = 0;
    while (at < out.size()) {
        DWORD got = 0;
        if (!ReadFile(h, out.data() + at, static_cast<DWORD>(std::min<size_t>(out.size() - at, 1 << 24)), &got, nullptr)) {
            err = LeafOf(p) + L": " + ErrorText(GetLastError());
            CloseHandle(h);
            return false;
        }
        if (got == 0) break;
        at += got;
        g_diffDone += got;
    }
    out.resize(at);
    CloseHandle(h);
    return true;
}

struct DiffText {
    std::vector<std::wstring> lines;
    std::wstring enc;
    int crlf = 0, lf = 0;
    bool endNl = false;  // a line break after the last line
};

// Text in UTF-8, UTF-16 (with its BOM) or the ANSI code page, in lines. false: it isn't text (null bytes near the start).
bool DiffDecode(const std::vector<uint8_t>& d, DiffText& t) {
    std::wstring w;
    const size_t n = d.size();
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        w.assign(reinterpret_cast<const wchar_t*>(d.data() + 2), (n - 2) / 2);
        t.enc = L"UTF-16";
    } else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
        w.resize((n - 2) / 2);
        for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<wchar_t>((d[2 + 2 * i] << 8) | d[3 + 2 * i]);
        t.enc = L"UTF-16 BE";
    } else {
        const size_t s = n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF ? 3 : 0;
        for (size_t i = s; i < std::min<size_t>(n, 8192); ++i)
            if (d[i] == 0) return false;
        const char* p = reinterpret_cast<const char*>(d.data()) + s;
        const int len = static_cast<int>(n - s);
        UINT cp = CP_UTF8;
        int m = len ? MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, p, len, nullptr, 0) : 0;
        if (len && m <= 0) {
            cp = CP_ACP;
            m = MultiByteToWideChar(cp, 0, p, len, nullptr, 0);
        }
        w.resize(static_cast<size_t>(std::max(0, m)));
        if (m > 0) MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, p, len, w.data(), m);
        t.enc = s ? L"UTF-8 with BOM" : cp == CP_UTF8 ? L"UTF-8" : L"ANSI";
    }
    size_t start = 0;
    for (size_t i = 0; i <= w.size(); ++i) {
        if (i < w.size() && w[i] != L'\n') continue;
        if (i == w.size() && start == i) {  // nothing after the last line break
            t.endNl = i > 0;
            break;
        }
        size_t end = i;
        if (end > start && w[end - 1] == L'\r') {
            --end;
            if (i < w.size()) ++t.crlf;
        } else if (i < w.size()) {
            ++t.lf;
        }
        t.lines.emplace_back(w, start, end - start);
        start = i + 1;
    }
    return true;
}

// ---- matching lines
struct DiffSpan {
    int a0, a1, b0, b1;
};

// Myers' greedy algorithm on a stretch: the pairs of lines it keeps. Nothing if it takes more than kDiffMyersMax steps.
void DiffMyers(const std::vector<int>& A, const std::vector<int>& B, const DiffSpan& s, std::vector<std::pair<int, int>>& out) {
    const int n = s.a1 - s.a0, m = s.b1 - s.b0;
    const int maxD = std::min(n + m, kDiffMyersMax), off = maxD + 1;
    std::vector<int> v(2 * static_cast<size_t>(maxD) + 3, 0);
    std::vector<std::vector<int>> trace;  // v as each step found it: k from -d-1 to d+1
    int found = -1;
    for (int d = 0; d <= maxD && found < 0; ++d) {
        trace.emplace_back(v.begin() + (off - d - 1), v.begin() + (off + d + 2));
        for (int k = -d; k <= d; k += 2) {
            int x = k == -d || (k != d && v[off + k - 1] < v[off + k + 1]) ? v[off + k + 1] : v[off + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && A[s.a0 + x] == B[s.b0 + y]) ++x, ++y;
            v[off + k] = x;
            if (x >= n && y >= m) {
                found = d;
                break;
            }
        }
    }
    if (found < 0) return;
    int x = n, y = m;
    for (int d = found; d > 0; --d) {
        const std::vector<int>& pv = trace[d];
        auto V = [&](int k) { return pv[k + d + 1]; };
        const int k = x - y;
        const int pk = k == -d || (k != d && V(k - 1) < V(k + 1)) ? k + 1 : k - 1;
        const int px = V(pk), py = px - pk;
        const int sx = pk == k + 1 ? px : px + 1;  // where this step's run of equal lines starts
        while (x > sx) {
            --x, --y;
            out.push_back({s.a0 + x, s.b0 + y});
        }
        x = px, y = py;
    }
    while (x > 0 && y > 0) {
        --x, --y;
        out.push_back({s.a0 + x, s.b0 + y});
    }
}

// The pairs of equal lines (a, b) that line the two sides up, in order.
std::vector<std::pair<int, int>> DiffMatch(const std::vector<int>& A, const std::vector<int>& B, uint64_t gen) {
    std::vector<std::pair<int, int>> out;
    std::vector<DiffSpan> todo{{0, static_cast<int>(A.size()), 0, static_cast<int>(B.size())}};
    while (!todo.empty() && g_diffGen == gen) {
        DiffSpan s = todo.back();
        todo.pop_back();
        while (s.a0 < s.a1 && s.b0 < s.b1 && A[s.a0] == B[s.b0]) out.push_back({s.a0++, s.b0++});
        while (s.a0 < s.a1 && s.b0 < s.b1 && A[s.a1 - 1] == B[s.b1 - 1]) out.push_back({--s.a1, --s.b1});
        if (s.a0 == s.a1 || s.b0 == s.b1) continue;
        // the lines that are there once on each side, and of those the longest run that keeps the order on both
        std::unordered_map<int, std::array<int, 3>> seen;  // line -> times on the left, times on the right, where on the right
        for (int i = s.a0; i < s.a1; ++i) ++seen[A[i]][0];
        for (int j = s.b0; j < s.b1; ++j)
            if (auto it = seen.find(B[j]); it != seen.end() && it->second[1]++ == 0) it->second[2] = j;
        std::vector<std::pair<int, int>> uniq;
        for (int i = s.a0; i < s.a1; ++i) {
            const auto& c = seen[A[i]];
            if (c[0] == 1 && c[1] == 1) uniq.push_back({i, c[2]});
        }
        std::vector<int> tails, prev(uniq.size(), -1);  // longest increasing run of the right side's places
        for (size_t u = 0; u < uniq.size(); ++u) {
            const auto at = std::lower_bound(tails.begin(), tails.end(), uniq[u].second, [&](int t, int j) { return uniq[t].second < j; });
            if (at != tails.begin()) prev[u] = *(at - 1);
            if (at == tails.end()) tails.push_back(static_cast<int>(u));
            else *at = static_cast<int>(u);
        }
        if (!tails.empty()) {
            std::vector<int> chain;
            for (int u = tails.back(); u >= 0; u = prev[u]) chain.push_back(u);
            int pa = s.a0, pb = s.b0;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const auto [i, j] = uniq[*it];
                todo.push_back({pa, i, pb, j});
                out.push_back({i, j});
                pa = i + 1, pb = j + 1;
            }
            todo.push_back({pa, s.a1, pb, s.b1});
            continue;
        }
        DiffMyers(A, B, s, out);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void DiffLines(DiffResult& r, uint64_t gen) {
    std::unordered_map<std::wstring_view, int> ids;
    std::vector<int> A, B;
    A.reserve(r.lines[0].size());
    B.reserve(r.lines[1].size());
    for (const std::wstring& s : r.lines[0]) A.push_back(ids.try_emplace(s, static_cast<int>(ids.size())).first->second);
    for (const std::wstring& s : r.lines[1]) B.push_back(ids.try_emplace(s, static_cast<int>(ids.size())).first->second);
    const std::vector<std::pair<int, int>> same = DiffMatch(A, B, gen);
    int i = 0, j = 0;
    auto between = [&](int ai, int bj) {  // what isn't the same up to there: paired as changed, the rest removed or added
        const int na = ai - i, nb = bj - j, both = std::min(na, nb);
        for (int k = 0; k < both; ++k) r.ops.push_back({i + k, j + k, 1});
        for (int k = both; k < na; ++k) r.ops.push_back({i + k, -1, 2});
        for (int k = both; k < nb; ++k) r.ops.push_back({-1, j + k, 3});
        r.removed += na;
        r.added += nb;
    };
    for (const auto& [a, b] : same) {
        if (a < i || b < j) continue;  // can't happen; just in case
        between(a, b);
        r.ops.push_back({a, b, 0});
        i = a + 1, j = b + 1;
    }
    between(static_cast<int>(A.size()), static_cast<int>(B.size()));
}

// ---- bytes
void DiffBytes(const uint8_t* a, const uint8_t* b, size_t n, uint64_t base, DiffResult& r, bool& inRun) {
    constexpr size_t kBlock = 4096;
    for (size_t o = 0; o < n; o += kBlock) {
        const size_t len = std::min(kBlock, n - o);
        if (!inRun && memcmp(a + o, b + o, len) == 0) continue;
        for (size_t i = o; i < o + len; ++i) {
            if (a[i] == b[i]) {
                inRun = false;
                continue;
            }
            ++r.diffBytes;
            if (inRun) continue;
            inRun = true;
            if (r.runs.size() < kDiffRuns) r.runs.push_back(base + i);
            else r.moreRuns = true;
        }
    }
}

// Too big to read whole: both read side by side.
void DiffStream(DiffResult& r, uint64_t gen) {
    HANDLE f[2];
    for (int s = 0; s < 2; ++s) {
        f[s] = CreateFileW(ToExtendedPath(r.path[s]).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f[s] == INVALID_HANDLE_VALUE && r.note.empty()) r.note = LeafOf(r.path[s]) + L": " + ErrorText(GetLastError());
    }
    if (r.note.empty()) {
        std::vector<uint8_t> ba(1 << 20), bb(1 << 20);
        const uint64_t common = std::min(r.size[0], r.size[1]);
        uint64_t at = 0;
        bool inRun = false;
        double last = NowMs();
        while (at < common && g_diffGen == gen) {
            const DWORD want = static_cast<DWORD>(std::min<uint64_t>(ba.size(), common - at));
            DWORD ga = 0, gb = 0;
            if (!ReadFile(f[0], ba.data(), want, &ga, nullptr) || !ReadFile(f[1], bb.data(), want, &gb, nullptr)) {
                r.note = ErrorText(GetLastError());
                break;
            }
            const DWORD got = std::min(ga, gb);
            if (got == 0) break;
            DiffBytes(ba.data(), bb.data(), got, at, r, inRun);
            at += got;
            g_diffDone = at;
            if (NowMs() - last > 150 && !g_diffNotify.exchange(true)) {
                last = NowMs();
                PostMessageW(g_hwnd, WM_APP_DIFF, 0, 0);
            }
        }
    }
    for (HANDLE h : f)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void DiffThread(std::wstring pa, std::wstring pb, uint64_t gen) {
    auto r = std::make_unique<DiffResult>();
    r->gen = gen;
    r->path[0] = pa;
    r->path[1] = pb;
    for (int s = 0; s < 2; ++s) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (GetFileAttributesExW(ToExtendedPath(r->path[s]).c_str(), GetFileExInfoStandard, &d)) {
            r->size[s] = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            r->time[s] = (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        } else if (r->note.empty()) {
            r->note = LeafOf(r->path[s]) + L": " + ErrorText(GetLastError());
        }
    }
    g_diffDone = 0;
    if (r->note.empty() && r->size[0] <= kDiffTextMax && r->size[1] <= kDiffTextMax) {
        g_diffTotal = r->size[0] + r->size[1];
        std::vector<uint8_t> da, db;
        if (DiffReadAll(pa, da, r->note) && DiffReadAll(pb, db, r->note)) {
            DiffText ta, tb;
            if (da == db) {
                r->kind = 1;
            } else if (DiffDecode(da, ta) && DiffDecode(db, tb)) {
                if (ta.lines == tb.lines) {
                    r->kind = 2;
                    std::vector<std::wstring> how;
                    if (ta.enc != tb.enc) how.push_back(L"the encoding (" + ta.enc + L" and " + tb.enc + L")");
                    auto eol = [](const DiffText& t) { return t.crlf && !t.lf ? L"CRLF" : t.lf && !t.crlf ? L"LF" : t.crlf ? L"mixed" : L""; };
                    if (*eol(ta) && *eol(tb) && wcscmp(eol(ta), eol(tb)) != 0) how.push_back(std::wstring(L"the line endings (") + eol(ta) + L" and " + eol(tb) + L")");
                    if (ta.endNl != tb.endNl) how.push_back(L"a line break at the end");
                    r->note = how.empty() ? L"Same text, written differently" : L"Same text; they differ only in ";
                    for (size_t i = 0; i < how.size(); ++i) r->note += (i == 0 ? L"" : i + 1 == how.size() ? L" and " : L", ") + how[i];
                } else {
                    r->kind = 3;
                    r->lines[0] = std::move(ta.lines);
                    r->lines[1] = std::move(tb.lines);
                    DiffLines(*r, gen);
                }
            } else {
                r->kind = 4;
                bool inRun = false;
                const size_t common = std::min(da.size(), db.size());
                DiffBytes(da.data(), db.data(), common, 0, *r, inRun);
            }
        }
    } else if (r->note.empty()) {
        g_diffTotal = std::min(r->size[0], r->size[1]);
        DiffStream(*r, gen);
        if (r->note.empty()) r->kind = r->size[0] == r->size[1] && r->runs.empty() ? 1 : 4;
    }
    if (r->kind == 4 && r->size[0] != r->size[1] && r->runs.empty()) r->runs.push_back(std::min(r->size[0], r->size[1]));  // one goes on
    if (g_diffGen != gen) return;
    DiffResult* raw = r.release();
    if (!PostMessageW(g_hwnd, WM_APP_DIFF, 1, reinterpret_cast<LPARAM>(raw))) delete raw;
}

// ---- the view
struct DfShow {
    int op;    // the line pair
    int fold;  // > 0: instead, this many equal lines folded away, from op on
};
enum class DfK { None, Close, Prev, Next, All, Fold, Overview };
struct DfHit {
    D2D1_RECT_F r;
    DfK k;
    int idx = 0;
};

struct DiffView {
    bool open = false;
    std::unique_ptr<DiffResult> r;
    std::vector<DfShow> show;
    std::vector<int> hunkAt;         // the shown row where each change starts
    std::unordered_set<int> opened;  // folds opened by a click (by their first line)
    bool all = false;                // every line, nothing folded
    float scroll = 0, viewH = 0;
    int hcol = 0;                    // columns scrolled to the right
    int cur = -1;                    // the change looked at (bytes: the run)
    bool goFirst = false;            // to the first change once the view's height is known
    D2D1_RECT_F box{}, body{}, overview{};
    std::vector<DfHit> hits;
    DfHit hover{};
    bool hovering = false;
    uint64_t hexRow = 0;             // bytes: the first row of 16 on screen
    int hexRows = 0;
    uint64_t hexLoaded = UINT64_MAX;
    int hexLoadedRows = 0;
    std::vector<uint8_t> hex[2];
} g_df;
constexpr int kDfContext = 3;
constexpr float kDfRow = 18.f;

void DfBuild() {
    g_df.show.clear();
    g_df.hunkAt.clear();
    if (!g_df.r || g_df.r->kind != 3) return;
    const std::vector<DiffOp>& ops = g_df.r->ops;
    const int n = static_cast<int>(ops.size());
    for (int i = 0; i < n;) {
        if (ops[i].kind != 0) {
            if (i == 0 || ops[i - 1].kind == 0) g_df.hunkAt.push_back(static_cast<int>(g_df.show.size()));
            g_df.show.push_back({i, 0});
            ++i;
            continue;
        }
        int j = i;
        while (j < n && ops[j].kind == 0) ++j;
        const int top = i == 0 ? 0 : kDfContext, bot = j == n ? 0 : kDfContext;
        if (g_df.all || j - i <= top + bot + 2 || g_df.opened.count(i)) {
            for (int k = i; k < j; ++k) g_df.show.push_back({k, 0});
        } else {
            for (int k = i; k < i + top; ++k) g_df.show.push_back({k, 0});
            g_df.show.push_back({i + top, j - i - top - bot});
            for (int k = j - bot; k < j; ++k) g_df.show.push_back({k, 0});
        }
        i = j;
    }
}

int DfCount() { return !g_df.r ? 0 : g_df.r->kind == 3 ? static_cast<int>(g_df.hunkAt.size()) : static_cast<int>(g_df.r->runs.size()); }

void DfClamp() {
    if (g_df.r && g_df.r->kind == 3) {
        const float total = g_df.show.size() * kDfRow;
        g_df.scroll = std::clamp(g_df.scroll, 0.f, std::max(0.f, total - g_df.viewH + kDfRow));
    } else if (g_df.r) {
        const uint64_t rows = (std::max(g_df.r->size[0], g_df.r->size[1]) + 15) / 16;
        const uint64_t maxRow = rows > static_cast<uint64_t>(g_df.hexRows) ? rows - g_df.hexRows : 0;
        g_df.hexRow = std::min(g_df.hexRow, maxRow);
    }
    g_df.hcol = std::max(0, g_df.hcol);
}

// To change h: in sight, a third of the way down.
void DfGo(int h) {
    const int n = DfCount();
    if (n == 0) return;
    g_df.cur = std::clamp(h, 0, n - 1);
    if (g_df.r->kind == 3) {
        g_df.scroll = g_df.hunkAt[g_df.cur] * kDfRow - g_df.viewH / 3;
        g_df.hcol = 0;
    } else {
        const uint64_t row = g_df.r->runs[g_df.cur] / 16;
        g_df.hexRow = row > 3 ? row - 3 : 0;
    }
    DfClamp();
    Invalidate();
}

void DiffClose() {
    if (!g_df.open) return;
    g_df.open = false;
    g_df.r.reset();
    g_df.show.clear();
    g_df.hits.clear();
    Invalidate();
}

void DiffShowResult(std::unique_ptr<DiffResult> r) {
    PopupClose();
    PaletteClose();
    ViewEditorClose();
    MenuEditorClose();
    SettingsClose();
    CancelRename();
    g_df = DiffView{};
    g_df.r = std::move(r);
    g_df.open = true;
    g_df.goFirst = true;
    DfBuild();
    Invalidate();
}

// Compares two files in the background: the status line says how it goes, and the view opens if they differ.
void CompareFiles(const std::wstring& a, const std::wstring& b) {
    if (Key(a) == Key(b)) {
        S.message = L"That's the same file twice";
        Invalidate();
        return;
    }
    g_diffWhat = LeafOf(a) == LeafOf(b) ? L"the two " + LeafOf(a) : LeafOf(a) + L" and " + LeafOf(b);
    g_diffBusy = true;
    S.message = L"Comparing " + g_diffWhat + L"…";
    std::thread(DiffThread, a, b, ++g_diffGen).detach();
    Invalidate();
}

void DiffApply(WPARAM wp, LPARAM lp) {
    if (wp == 0) {  // progress, on big files
        g_diffNotify = false;
        if (g_diffBusy && g_diffTotal > (32u << 20)) {
            const int pct = static_cast<int>(std::min<uint64_t>(100, g_diffDone * 100 / std::max<uint64_t>(1, g_diffTotal)));
            S.message = L"Comparing " + g_diffWhat + L"…  " + std::to_wstring(pct) + L"%";
            Invalidate();
        }
        return;
    }
    std::unique_ptr<DiffResult> r(reinterpret_cast<DiffResult*>(lp));
    if (r->gen != g_diffGen) return;
    g_diffBusy = false;
    wchar_t sz[64];
    FormatSize(r->size[0], sz, 64);
    switch (r->kind) {
        case 0: S.message = L"Couldn't compare them: " + r->note; break;
        case 1: S.message = r->size[0] ? L"Identical: " + g_diffWhat + L" are the same " + sz + L", byte for byte" : L"Identical: both are empty"; break;
        case 2: S.message = r->note; break;
        default:
            S.message.clear();
            DiffShowResult(std::move(r));
            break;
    }
    Invalidate();
}

// ---- drawing
std::wstring DfExpand(const std::wstring& s) {  // tabs to the next multiple of four
    if (s.find(L'\t') == std::wstring::npos) return s;
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\t') o.append(4 - o.size() % 4, L' ');
        else o += c;
    }
    return o;
}
int DfCol(const std::wstring& s, size_t upTo) {  // the column where character upTo lands once tabs are expanded
    int col = 0;
    for (size_t i = 0; i < upTo && i < s.size(); ++i) col = s[i] == L'\t' ? col + 4 - col % 4 : col + 1;
    return col;
}

constexpr UINT32 kDfRed = 0xF85149, kDfGreen = 0x3FB950, kDfAmber = 0xE0A458;
void DfDrawHex(float L, float colW, const D2D1_RECT_F& body, float cw);

void DfHeader(float x0, float y0, float R) {
    const DiffResult& r = *g_df.r;
    const float mid = std::floor((x0 + R) / 2);
    for (int s = 0; s < 2; ++s) {
        const float l = s ? mid + 14 : x0 + 18, rr = s ? R - 48 : mid - 14;
        wchar_t sz[64], dt[64];
        FormatSize(r.size[s], sz, 64);
        FormatDate(r.time[s], dt, 64);
        const bool newer = r.time[s] > r.time[1 - s] + 20000000;  // two seconds
        Fill(D2D1::RectF(l, y0 + 12, l + 3, y0 + 46), s ? kDfGreen : kDfRed, 0.8f);
        Color(cText);
        Text(LeafOf(r.path[s]), g_fmtNameBold.Get(), l + 12, y0 + 8, rr, y0 + 28);
        const std::wstring info = std::wstring(sz) + L"  ·  " + dt + (newer ? L"  ·  newer" : L"");
        const float iw = MeasureW(info, g_fmtStatus.Get());
        Color(cFaint);
        Text(ParentOf(r.path[s]), g_fmtStatus.Get(), l + 12, y0 + 28, std::max(l + 40, rr - iw - 14), y0 + 48);
        Color(newer ? cDim : cFaint);
        Text(info, g_fmtStatusR.Get(), rr - iw - 4, y0 + 28, rr, y0 + 48);
    }
    Fill(D2D1::RectF(mid, y0 + 10, mid + 1, y0 + 48), cBorder);
}

void DrawDiff(float W, float H) {
    if (!g_df.open || !g_df.r) return;
    g_df.hits.clear();
    const DiffResult& r = *g_df.r;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    const float x0 = 12, y0 = kTabH + 6, R = W - 12, B = H - 12;
    g_df.box = D2D1::RectF(x0, y0, R, B);
    Round(g_df.box, 10, cBg);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_df.box, 10, 10), g_brush.Get(), 1.f);
    auto isHover = [&](DfK k, int idx = 0) { return g_df.hovering && g_df.hover.k == k && g_df.hover.idx == idx; };
    DfHeader(x0, y0, R);
    const D2D1_RECT_F cb = D2D1::RectF(R - 40, y0 + 13, R - 12, y0 + 41);
    if (isHover(DfK::Close)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(DfK::Close) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_df.hits.push_back({cb, DfK::Close});
    HLine(x0, R, y0 + 56);

    // where things go, and the first change in sight before anything says where we are
    const float by = y0 + 57, bb = by + 36;
    g_df.body = D2D1::RectF(x0 + 1, bb + 1, R - 1, B - 26);
    g_df.viewH = g_df.body.bottom - g_df.body.top;
    if (r.kind == 4) g_df.hexRows = std::max(1, static_cast<int>(g_df.viewH / kDfRow) - 1);
    if (g_df.goFirst && g_df.viewH > 0) {
        g_df.goFirst = false;
        if (r.kind == 4 || (!g_df.hunkAt.empty() && g_df.hunkAt[0] * kDfRow > g_df.viewH / 2)) DfGo(0);
        else g_df.cur = g_df.hunkAt.empty() ? -1 : 0;
    }
    DfClamp();

    // the bar: what differs, and the buttons
    float tx = x0 + 18;
    auto part = [&](const std::wstring& s, UINT32 c) {
        Color(c);
        Text(s, g_fmtStatus.Get(), tx, by, R - 300, bb);
        tx += MeasureW(s, g_fmtStatus.Get());
    };
    const int n = DfCount();
    if (r.kind == 3) {
        part(Thousands(n) + (n == 1 ? L" change" : L" changes") + L"     ", cText);
        part(L"−" + Thousands(r.removed), kDfRed);
        part(L"   ", cText);
        part(L"+" + Thousands(r.added), kDfGreen);
        part(L"   lines", cFaint);
    } else {
        const uint64_t first = r.runs.empty() ? 0 : r.runs[0];
        wchar_t hexAt[32];
        swprintf(hexAt, 32, L"0x%llX", static_cast<unsigned long long>(first));
        part(L"Different from byte " + Thousands(static_cast<int>(std::min<uint64_t>(first, INT_MAX))) + L" (" + hexAt + L")", cText);
        if (r.diffBytes) part(L"   ·   " + Thousands(static_cast<int>(std::min<uint64_t>(r.diffBytes, INT_MAX))) + (r.diffBytes == 1 ? L" byte differs" : L" bytes differ"), cDim);
        if (r.size[0] != r.size[1]) {
            wchar_t d[64];
            FormatSize(r.size[0] > r.size[1] ? r.size[0] - r.size[1] : r.size[1] - r.size[0], d, 64);
            part(L"   ·   " + LeafOf(r.path[r.size[1] > r.size[0]]) + L" is " + d + L" longer", cDim);
        }
    }
    // buttons, from the right: next, previous, "2 of 12", and the fold switch
    float bx = R - 14;
    auto button = [&](DfK k, const std::wstring& label, bool on = true) {
        const float w = MeasureW(label, g_fmtStatus.Get()) + 22;
        const D2D1_RECT_F b = D2D1::RectF(bx - w, by + 5, bx, bb - 5);
        Round(b, 6, isHover(k) && on ? 0x3A3A44 : cTop);
        Color(on ? cText : cFaint);
        Text(label, g_fmtCenter.Get(), b.left, b.top, b.right, b.bottom);
        if (on) g_df.hits.push_back({b, k});
        bx = b.left - 6;
    };
    button(DfK::Next, L"Next  ↓", n > 0);
    button(DfK::Prev, L"↑  Previous", n > 0);
    if (n) {
        const std::wstring where = (g_df.cur >= 0 ? Thousands(g_df.cur + 1) + L" of " : L"") + Thousands(n) + (r.moreRuns ? L"+" : L"");
        const float w = MeasureW(where, g_fmtStatus.Get());
        Color(cDim);
        Text(where, g_fmtStatusR.Get(), bx - w - 8, by, bx - 4, bb);
        bx -= w + 18;
    }
    if (r.kind == 3) button(DfK::All, g_df.all ? L"Fold equal lines" : L"Show all lines");
    HLine(x0, R, bb);

    // the lines
    const D2D1_RECT_F& body = g_df.body;
    const float cw = MonoCharW(), ovW = 12;
    g_df.overview = D2D1::RectF(body.right - ovW, body.top, body.right, body.bottom);
    const float L = body.left, Rt = body.right - ovW - 2, colW = std::floor((Rt - L) / 2);
    if (r.kind == 4) {
        DfDrawHex(L, colW, body, cw);
    } else {
        const size_t most = std::max(r.lines[0].size(), r.lines[1].size());
        const float gw = (most >= 100000 ? 6 : most >= 10000 ? 5 : most >= 1000 ? 4 : 3) * cw + 16;
        const int curFrom = g_df.cur >= 0 && g_df.cur < static_cast<int>(g_df.hunkAt.size()) ? g_df.hunkAt[g_df.cur] : -1;
        int curTo = curFrom;
        while (curTo >= 0 && curTo + 1 < static_cast<int>(g_df.show.size()) && !g_df.show[curTo + 1].fold && r.ops[g_df.show[curTo + 1].op].kind != 0) ++curTo;
        g_rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
        const int first = std::max(0, static_cast<int>(g_df.scroll / kDfRow));
        for (int i = first; i < static_cast<int>(g_df.show.size()); ++i) {
            const float y = body.top + i * kDfRow - g_df.scroll;
            if (y >= body.bottom) break;
            const DfShow& sh = g_df.show[i];
            if (sh.fold) {
                const D2D1_RECT_F fr = D2D1::RectF(L, y, Rt, y + kDfRow);
                Fill(fr, 0xFFFFFF, isHover(DfK::Fold, i) ? 0.07f : 0.025f);
                Color(isHover(DfK::Fold, i) ? cDim : cFaint);
                Text(L"⋯   " + Thousands(sh.fold) + L" equal lines   ⋯", g_fmtCenter.Get(), L, y, Rt, y + kDfRow);
                g_df.hits.push_back({fr, DfK::Fold, i});
                continue;
            }
            const DiffOp& op = r.ops[sh.op];
            for (int s = 0; s < 2; ++s) {
                const float cx = L + s * colW, cr = cx + colW - (s ? 0 : 1);
                const int idx = s ? op.b : op.a;
                const UINT32 tint = s ? kDfGreen : kDfRed;
                if (op.kind == 1) Fill(D2D1::RectF(cx, y, cr, y + kDfRow), tint, 0.10f);
                else if (op.kind != 0 && idx >= 0) Fill(D2D1::RectF(cx, y, cr, y + kDfRow), tint, 0.15f);
                else if (op.kind != 0) Fill(D2D1::RectF(cx, y, cr, y + kDfRow), 0x000000, 0.22f);
                if (idx < 0) continue;
                Color(op.kind ? cDim : cFaint);
                Text(std::to_wstring(idx + 1), g_fmtMonoR.Get(), cx, y, cx + gw - 10, y + kDfRow);
                const std::wstring& raw = r.lines[s][idx];
                const float tx0 = cx + gw;
                if (op.kind == 1) {  // the part that changed, brighter
                    const std::wstring& other = r.lines[1 - s][s ? op.a : op.b];
                    size_t p = 0, q = 0;
                    while (p < raw.size() && p < other.size() && raw[p] == other[p]) ++p;
                    while (q < raw.size() - p && q < other.size() - p && raw[raw.size() - 1 - q] == other[other.size() - 1 - q]) ++q;
                    const int c0 = DfCol(raw, p) - g_df.hcol, c1 = DfCol(raw, raw.size() - q) - g_df.hcol;
                    if (c1 > c0 && c1 > 0) Fill(D2D1::RectF(tx0 + std::max(0, c0) * cw, y + 1, std::min(cr, tx0 + c1 * cw), y + kDfRow - 1), tint, 0.32f);
                }
                const std::wstring shown = DfExpand(raw);
                if (static_cast<int>(shown.size()) > g_df.hcol) {
                    Color(cText);
                    Text(shown.c_str() + g_df.hcol, std::min<size_t>(shown.size() - g_df.hcol, 2000), g_fmtMono.Get(), tx0, y, cr - 4, y + kDfRow);
                }
            }
            if (i >= curFrom && i <= curTo && curFrom >= 0) Fill(D2D1::RectF(L, y, L + 3, y + kDfRow), cAccent);
        }
        g_rt->PopAxisAlignedClip();
        Fill(D2D1::RectF(L + colW - 1, body.top, L + colW, body.bottom), cBorder);
        // the overview: where the changes are in the whole, and what is on screen
        const D2D1_RECT_F& ov = g_df.overview;
        const float total = std::max({1.f, static_cast<float>(g_df.show.size()), std::floor(g_df.viewH / kDfRow)});  // short ones: as on screen
        const float oh = ov.bottom - ov.top;
        Fill(ov, 0xFFFFFF, 0.03f);
        for (size_t i = 0; i < g_df.show.size();) {
            if (g_df.show[i].fold || r.ops[g_df.show[i].op].kind == 0) {
                ++i;
                continue;
            }
            size_t j = i;
            bool del = false, add = false;
            while (j < g_df.show.size() && !g_df.show[j].fold && r.ops[g_df.show[j].op].kind != 0) {
                const int k = r.ops[g_df.show[j].op].kind;
                del |= k != 3;
                add |= k != 2;
                ++j;
            }
            const float t0 = ov.top + oh * i / total, t1 = std::max(t0 + 2, ov.top + oh * j / total);
            Fill(D2D1::RectF(ov.left + 2, t0, ov.right - 2, t1), del && add ? kDfAmber : del ? kDfRed : kDfGreen, 0.85f);
            i = j;
        }
        const float vt = ov.top + oh * (g_df.scroll / kDfRow) / total, vb = std::min(ov.bottom, vt + oh * (g_df.viewH / kDfRow) / total);
        Fill(D2D1::RectF(ov.left, vt, ov.right, std::max(vt + 4, vb)), 0xFFFFFF, 0.10f);
        g_df.hits.push_back({ov, DfK::Overview});
    }
    HLine(x0, R, B - 26);
    Color(cFaint);
    Text(r.kind == 3 ? L"F7 and Shift+F7 go to the next and previous change   ·   A shows all lines or folds the equal ones   ·   Esc closes"
                     : L"F7 and Shift+F7 go to the next and previous difference   ·   Esc closes",
         g_fmtStatus.Get(), x0 + 18, B - 25, R - 18, B - 2);
}

// ---- bytes, in hex
void DfLoadHex() {
    const uint64_t from = g_df.hexRow * 16;
    if (g_df.hexLoaded == from && g_df.hexLoadedRows == g_df.hexRows) return;
    g_df.hexLoaded = from;
    g_df.hexLoadedRows = g_df.hexRows;
    for (int s = 0; s < 2; ++s) {
        g_df.hex[s].clear();
        HANDLE h = CreateFileW(ToExtendedPath(g_df.r->path[s]).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(from);
        g_df.hex[s].resize(static_cast<size_t>(g_df.hexRows) * 16);
        DWORD got = 0;
        if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !ReadFile(h, g_df.hex[s].data(), static_cast<DWORD>(g_df.hex[s].size()), &got, nullptr)) got = 0;
        g_df.hex[s].resize(got);
        CloseHandle(h);
    }
}

void DfDrawHex(float L, float colW, const D2D1_RECT_F& body, float cw) {
    DfLoadHex();
    const DiffResult& r = *g_df.r;
    const uint64_t most = std::max(r.size[0], r.size[1]);
    const int offDigits = most > 0xFFFFFFFFull ? 12 : 8;
    const bool ascii = colW > (offDigits + 2 + 16 * 3 + 2 + 16) * cw + 24;
    const uint64_t curRun = g_df.cur >= 0 && g_df.cur < static_cast<int>(r.runs.size()) ? r.runs[g_df.cur] : UINT64_MAX;
    g_rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int row = 0; row < g_df.hexRows; ++row) {
        const float y = body.top + 6 + row * kDfRow;
        const uint64_t base = (g_df.hexRow + row) * 16;
        if (base >= most) break;
        for (int s = 0; s < 2; ++s) {
            const float cx = L + s * colW + 16;
            const std::vector<uint8_t>& me = g_df.hex[s];
            const std::vector<uint8_t>& other = g_df.hex[1 - s];
            wchar_t off[24];
            swprintf(off, 24, offDigits == 8 ? L"%08llX" : L"%012llX", static_cast<unsigned long long>(base));
            Color(cFaint);
            Text(off, g_fmtMono.Get(), cx, y, cx + (offDigits + 1) * cw, y + kDfRow);
            for (int b = 0; b < 16; ++b) {
                const size_t i = static_cast<size_t>(row) * 16 + b;
                if (i >= me.size()) break;
                const bool differs = i >= other.size() || other[i] != me[i];
                const float hx = cx + (offDigits + 2 + b * 3 + (b >= 8 ? 1 : 0)) * cw;
                if (differs) Fill(D2D1::RectF(hx - 1, y + 1, hx + 2 * cw + 1, y + kDfRow - 1), s ? kDfGreen : kDfRed, base + b >= curRun && base + b < curRun + 16 ? 0.45f : 0.25f);
                wchar_t hb[3];
                swprintf(hb, 3, L"%02X", me[i]);
                Color(differs ? cText : cDim);
                Text(hb, 2, g_fmtMono.Get(), hx, y, hx + 2 * cw + 2, y + kDfRow);
                if (ascii) {
                    const float ax = cx + (offDigits + 2 + 16 * 3 + 2 + b) * cw;
                    const wchar_t ch = me[i] >= 32 && me[i] < 127 ? static_cast<wchar_t>(me[i]) : L'·';
                    if (differs) Fill(D2D1::RectF(ax, y + 1, ax + cw, y + kDfRow - 1), s ? kDfGreen : kDfRed, 0.25f);
                    Color(differs ? cText : cFaint);
                    Text(&ch, 1, g_fmtMono.Get(), ax, y, ax + cw + 2, y + kDfRow);
                }
            }
        }
    }
    g_rt->PopAxisAlignedClip();
    Fill(D2D1::RectF(L + colW - 1, body.top, L + colW, body.bottom), cBorder);
    // the overview: where the runs are in the file
    const D2D1_RECT_F& ov = g_df.overview;
    const float oh = ov.bottom - ov.top;
    Fill(ov, 0xFFFFFF, 0.03f);
    const double whole = static_cast<double>(std::max<uint64_t>(1, most));
    float lastY = -10;
    for (uint64_t at : r.runs) {
        const float y = ov.top + static_cast<float>(oh * (static_cast<double>(at) / whole));
        if (y - lastY < 2) continue;
        lastY = y;
        Fill(D2D1::RectF(ov.left + 2, y, ov.right - 2, y + 2), kDfAmber, 0.85f);
    }
    const float vt = ov.top + static_cast<float>(oh * (static_cast<double>(g_df.hexRow * 16) / whole));
    const float vb = ov.top + static_cast<float>(oh * (static_cast<double>((g_df.hexRow + g_df.hexRows) * 16) / whole));
    Fill(D2D1::RectF(ov.left, vt, ov.right, std::max(vt + 4, std::min(ov.bottom, vb))), 0xFFFFFF, 0.10f);
    g_df.hits.push_back({ov, DfK::Overview});
}

// ---- mouse and keyboard
int DfHitAt(float x, float y) {
    for (int i = static_cast<int>(g_df.hits.size()) - 1; i >= 0; --i)
        if (InRect(g_df.hits[i].r, x, y)) return i;
    return -1;
}

void DiffMouseMove(float x, float y) {
    const int h = DfHitAt(x, y);
    const bool was = g_df.hovering;
    const DfHit old = g_df.hover;
    g_df.hovering = h >= 0;
    g_df.hover = h >= 0 ? g_df.hits[h] : DfHit{};
    if (was != g_df.hovering || old.k != g_df.hover.k || old.idx != g_df.hover.idx) Invalidate();
}

void DfScrollRows(float rows) {
    if (!g_df.r) return;
    if (g_df.r->kind == 3) g_df.scroll += rows * kDfRow;
    else g_df.hexRow = rows < 0 && g_df.hexRow < static_cast<uint64_t>(-rows) ? 0 : g_df.hexRow + static_cast<int64_t>(rows);
    DfClamp();
    Invalidate();
}

void DfToggleAll() {
    if (!g_df.r || g_df.r->kind != 3) return;
    // the line at the top of the view stays there
    const int top = std::clamp(static_cast<int>(g_df.scroll / kDfRow), 0, std::max(0, static_cast<int>(g_df.show.size()) - 1));
    const int op = g_df.show.empty() ? 0 : g_df.show[top].op;
    g_df.all = !g_df.all;
    DfBuild();
    for (size_t i = 0; i < g_df.show.size(); ++i)
        if (g_df.show[i].op >= op || (g_df.show[i].fold && g_df.show[i].op + g_df.show[i].fold > op)) {
            g_df.scroll = i * kDfRow;
            break;
        }
    DfClamp();
    Invalidate();
}

void DfStep(int dir) {
    const int n = DfCount();
    if (!n) return;
    if (g_df.r->kind == 3 && g_df.cur >= 0) {
        // from what is on screen: the next change below the top third, or the one above
        DfGo(g_df.cur + dir);
        return;
    }
    DfGo(g_df.cur < 0 ? (dir > 0 ? 0 : n - 1) : g_df.cur + dir);
}

void DiffClick(float x, float y) {
    if (!InRect(g_df.box, x, y)) return;  // the view fills the window: a click outside it is on the tabs
    const int hi = DfHitAt(x, y);
    if (hi < 0) return;
    const DfHit h = g_df.hits[hi];
    switch (h.k) {
        case DfK::Close: DiffClose(); return;
        case DfK::Next: DfStep(1); return;
        case DfK::Prev: DfStep(-1); return;
        case DfK::All: DfToggleAll(); return;
        case DfK::Fold:
            if (h.idx >= 0 && h.idx < static_cast<int>(g_df.show.size())) {
                g_df.opened.insert(g_df.show[h.idx].op - (g_df.show[h.idx].op > 0 && g_df.r->ops[g_df.show[h.idx].op - 1].kind == 0 ? kDfContext : 0));
                DfBuild();
                DfClamp();
                Invalidate();
            }
            return;
        case DfK::Overview: {
            const float f = (y - g_df.overview.top) / std::max(1.f, g_df.overview.bottom - g_df.overview.top);
            if (g_df.r->kind == 3) g_df.scroll = f * g_df.show.size() * kDfRow - g_df.viewH / 2;
            else {
                const uint64_t rows = (std::max(g_df.r->size[0], g_df.r->size[1]) + 15) / 16;
                const uint64_t at = static_cast<uint64_t>(f * static_cast<double>(rows));
                g_df.hexRow = at > static_cast<uint64_t>(g_df.hexRows / 2) ? at - g_df.hexRows / 2 : 0;
            }
            DfClamp();
            Invalidate();
            return;
        }
        default: return;
    }
}

void DiffWheel(int delta, bool sideways) {
    if (sideways && g_df.r && g_df.r->kind == 3) {
        g_df.hcol = std::max(0, g_df.hcol - delta / 120 * 8);
        Invalidate();
        return;
    }
    DfScrollRows(-delta / 120.f * 3);
}

void DiffKey(WPARAM vk) {
    const bool alt = KeyDown(VK_MENU), shift = KeyDown(VK_SHIFT);
    const float page = std::max(1.f, std::floor(g_df.viewH / kDfRow) - 2);
    switch (vk) {
        case VK_ESCAPE: DiffClose(); break;
        case VK_F7: DfStep(shift ? -1 : 1); break;
        case 'N': DfStep(1); break;
        case 'P': DfStep(-1); break;
        case 'A': DfToggleAll(); break;
        case VK_DOWN: alt ? DfStep(1) : DfScrollRows(1); break;
        case VK_UP: alt ? DfStep(-1) : DfScrollRows(-1); break;
        case VK_NEXT: DfScrollRows(page); break;
        case VK_PRIOR: DfScrollRows(-page); break;
        case VK_HOME:
            g_df.scroll = 0;
            g_df.hexRow = 0;
            g_df.hcol = 0;
            Invalidate();
            break;
        case VK_END: DfScrollRows(1e9f); break;
        case VK_RIGHT:
            g_df.hcol += 8;
            Invalidate();
            break;
        case VK_LEFT:
            g_df.hcol = std::max(0, g_df.hcol - 8);
            Invalidate();
            break;
        default: break;
    }
}

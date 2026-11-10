// Comparing two folders (Ctrl+K → "compare" with two folders selected, or in split view the two panes' folders; also in
// the right-click menu). Both are walked with all their subfolders at once, and the tab lists what differs, one row
// for each thing, with its two sides where the columns are: its date and size in the first folder, its date and size
// in the second, a dash where it isn't, and between them an arrow from the side that has it, or has it newer, to the
// other (≠ when neither is newer). A folder only on one side shows as that folder alone. Files with the same size and
// date count as the same without being read; with the same size and another date they're read, and if the bytes are
// the same they count as the same too (files only in the cloud aren't read: that would download them). A row is the
// item on the side the arrow leaves from, and everything that works on search results works on it; F5 and F6 copy and
// move it the way the arrow points, and Ctrl+K → "compare" on a file that is on both sides compares the two. When
// something in either folder changes, it compares again.
// Included from app.cpp after diff.inl.

constexpr UINT WM_APP_CMP = WM_APP + 24;
constexpr UINT_PTR kTimerCmp = 23;
constexpr int64_t kCmpSameTime = 20000000;  // two seconds: FAT drives and some network drives keep dates to two seconds

enum CmpGroup { CmpOnlyA, CmpNewerA, CmpOnlyB, CmpNewerB, CmpDiffer, CmpGroups };

struct CmpItem {
    int group = 0;
    std::wstring dirKey, name;  // where it is: its folder's key, and its name
    std::wstring rel;           // its path below its side's folder
    int hint = -1;              // its index in the folder's listing last time
    std::wstring odirKey, oname;  // the same on the other side, when it is there too
    int ohint = -1;
};

// What a row's other side is, next to the row itself (which is the side the arrow leaves from).
struct CmpOther {
    const Listing* l = nullptr;
    int e = -1;
    int group = 0;
};

// What a running comparison and the tab share: the comparison stops when gen moves on.
struct CmpLive {
    std::atomic<uint64_t> gen{0};
    std::atomic<int> dirs{0}, reading{0};
    std::atomic<bool> notify{false};
};

struct CmpState {
    std::wstring root[2], label[2];
    std::shared_ptr<CmpLive> live = std::make_shared<CmpLive>();
    uint64_t gen = 0;
    bool busy = false, again = false, dirty = false;
    std::vector<CmpItem> items;   // by folder, then by name
    std::vector<CmpOther> other;  // one for each row
    int shown[CmpGroups]{};       // how many rows of each kind
    int same = 0, sameBytes = 0;  // files the same on both sides; of those, the ones with another date and the same bytes
    int failed = 0;               // folders that couldn't be read
};

struct CmpOut {
    const CmpLive* live = nullptr;
    uint64_t gen = 0;
    std::vector<std::shared_ptr<const Listing>> lists;
    std::vector<CmpItem> items;
    int same = 0, sameBytes = 0, dirs = 0, failed = 0;
    std::wstring error;  // one of the folders couldn't be read at all
};

std::mutex g_cmpCacheM;
std::unordered_map<std::wstring, bool> g_cmpCache;  // two files with their size and dates -> the same bytes

bool CmpSameBytes(const std::wstring& a, const std::wstring& b, const CmpLive& live, uint64_t gen) {
    HANDLE f[2];
    const std::wstring p[2] = {a, b};
    for (int s = 0; s < 2; ++s)
        f[s] = CreateFileW(ToExtendedPath(p[s]).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool same = f[0] != INVALID_HANDLE_VALUE && f[1] != INVALID_HANDLE_VALUE;
    static thread_local std::vector<uint8_t> ba(1 << 20), bb(1 << 20);
    while (same && live.gen == gen) {
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
    return same && live.gen == gen;
}

void CmpThread(std::wstring rootA, std::wstring rootB, std::shared_ptr<CmpLive> live, uint64_t gen, bool showHidden, SortBy by, bool asc, bool ff) {
    auto out = std::make_unique<CmpOut>();
    out->live = live.get();
    out->gen = gen;
    const std::wstring root[2] = {rootA, rootB};
    struct Check {
        std::wstring path[2], dirKey[2], name[2], rel;
        uint64_t size;
        int64_t time[2];
    };
    std::vector<Check> checks;
    double last = NowMs();
    auto progress = [&] {
        if (NowMs() - last < 150) return;
        last = NowMs();
        if (!live->notify.exchange(true)) PostMessageW(g_hwnd, WM_APP_CMP, 0, 0);
    };
    auto shown = [&](const Entry& e) { return showHidden || !(e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)); };
    auto add = [&](int group, const Listing& l, const Entry& e, const std::wstring& rel, const Listing* ol = nullptr, const Entry* oe = nullptr) {
        CmpItem it;
        it.group = group;
        it.dirKey = Key(l.path);
        it.name.assign(l.name(e), e.nameLen);
        it.rel = rel.empty() ? it.name : rel + L"\\" + it.name;
        if (ol) {
            it.odirKey = Key(ol->path);
            it.oname.assign(ol->name(*oe), oe->nameLen);
        }
        out->items.push_back(std::move(it));
    };
    std::deque<std::wstring> q{L""};
    while (!q.empty() && live->gen == gen) {
        const std::wstring rel = std::move(q.front());
        q.pop_front();
        std::shared_ptr<Listing> l[2];
        for (int s = 0; s < 2; ++s) {
            l[s] = std::make_shared<Listing>();
            EnumerateDir(rel.empty() ? root[s] : Join(root[s], rel.c_str()), EnumMethod::Nt, *l[s]);
        }
        live->dirs += 2;
        if (l[0]->error || l[1]->error) {
            if (rel.empty()) {
                const int s = l[0]->error ? 0 : 1;
                out->error = LeafOf(root[s]) + L": " + ErrorText(l[s]->error);
                break;
            }
            ++out->failed;  // what is in it can't be told apart: neither side is listed
            continue;
        }
        for (auto& x : l) SortListing(*x, by, asc, ff);
        std::unordered_map<std::wstring, int> inB;
        for (size_t j = 0; j < l[1]->entries.size(); ++j) {
            const Entry& e = l[1]->entries[j];
            if (shown(e)) inB.emplace(Key(std::wstring(l[1]->name(e), e.nameLen)), static_cast<int>(j));
        }
        std::vector<uint8_t> matched(l[1]->entries.size(), 0);
        for (const Entry& ea : l[0]->entries) {
            if (!shown(ea)) continue;
            const std::wstring name(l[0]->name(ea), ea.nameLen);
            const auto it = inB.find(Key(name));
            if (it == inB.end()) {
                add(CmpOnlyA, *l[0], ea, rel);
                continue;
            }
            matched[it->second] = 1;
            const Entry& eb = l[1]->entries[it->second];
            if (ea.isDir() && eb.isDir()) {
                if (!(ea.attrs & FILE_ATTRIBUTE_REPARSE_POINT) && !(eb.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) q.push_back(rel.empty() ? name : rel + L"\\" + name);
                continue;  // links to folders: being on both sides is enough
            }
            if (ea.isDir() != eb.isDir()) {  // a folder on one side, a file on the other
                add(CmpDiffer, *l[0], ea, rel, l[1].get(), &eb);
                continue;
            }
            const int64_t dt = ea.mtime - eb.mtime;
            const bool sameTime = dt <= kCmpSameTime && dt >= -kCmpSameTime;
            if (ea.size == eb.size && sameTime) {
                ++out->same;
                continue;
            }
            if (ea.size == eb.size && !OnlineOnly(ea.attrs) && !OnlineOnly(eb.attrs)) {  // the bytes tell, later
                Check c;
                for (int s = 0; s < 2; ++s) {
                    const Entry& e = s ? eb : ea;
                    c.name[s].assign(l[s]->name(e), e.nameLen);
                    c.path[s] = Join(l[s]->path, c.name[s].c_str());
                    c.dirKey[s] = Key(l[s]->path);
                    c.time[s] = e.mtime;
                }
                c.rel = rel;
                c.size = ea.size;
                checks.push_back(std::move(c));
                continue;
            }
            if (sameTime) add(CmpDiffer, *l[0], ea, rel, l[1].get(), &eb);
            else if (dt > 0) add(CmpNewerA, *l[0], ea, rel, l[1].get(), &eb);
            else add(CmpNewerB, *l[1], eb, rel, l[0].get(), &ea);
        }
        for (size_t j = 0; j < l[1]->entries.size(); ++j)
            if (!matched[j] && shown(l[1]->entries[j])) add(CmpOnlyB, *l[1], l[1]->entries[j], rel);
        out->lists.push_back(std::move(l[0]));
        out->lists.push_back(std::move(l[1]));
        progress();
    }
    live->reading = static_cast<int>(checks.size());
    for (const Check& c : checks) {
        if (live->gen != gen) break;
        const std::wstring key = Key(c.path[0]) + L"|" + Key(c.path[1]) + L"|" + std::to_wstring(c.size) + L"|" + std::to_wstring(c.time[0]) + L"|" +
                                 std::to_wstring(c.time[1]);
        int known = -1;
        {
            std::lock_guard<std::mutex> lock(g_cmpCacheM);
            if (auto it = g_cmpCache.find(key); it != g_cmpCache.end()) known = it->second;
        }
        const bool same = known >= 0 ? known == 1 : CmpSameBytes(c.path[0], c.path[1], *live, gen);
        if (live->gen != gen) break;
        if (known < 0) {
            std::lock_guard<std::mutex> lock(g_cmpCacheM);
            if (g_cmpCache.size() > 20000) g_cmpCache.clear();
            g_cmpCache[key] = same;
        }
        --live->reading;
        progress();
        if (same) {
            ++out->same;
            ++out->sameBytes;
            continue;
        }
        const int s = c.time[0] > c.time[1] ? 0 : 1;
        CmpItem it;
        it.group = s ? CmpNewerB : CmpNewerA;
        it.dirKey = c.dirKey[s];
        it.name = c.name[s];
        it.rel = c.rel.empty() ? c.name[s] : c.rel + L"\\" + c.name[s];
        it.odirKey = c.dirKey[1 - s];
        it.oname = c.name[1 - s];
        out->items.push_back(std::move(it));
    }
    if (live->gen != gen) return;
    // each folder's things together, the top folder's first
    std::stable_sort(out->items.begin(), out->items.end(), [](const CmpItem& a, const CmpItem& b) {
        const size_t da = a.rel.size() - a.name.size(), db = b.rel.size() - b.name.size();
        if (da != db || _wcsnicmp(a.rel.c_str(), b.rel.c_str(), da) != 0)
            if (const int c = StrCmpLogicalW(a.rel.substr(0, da).c_str(), b.rel.substr(0, db).c_str())) return c < 0;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    out->dirs = live->dirs;
    CmpOut* raw = out.release();
    if (!PostMessageW(g_hwnd, WM_APP_CMP, 1, reinterpret_cast<LPARAM>(raw))) delete raw;
}

// How each side is called in the titles: the folder's name, or more of its path when both have the same name.
void CmpLabels(CmpState& st) {
    std::wstring a = st.root[0], b = st.root[1];
    std::wstring la = LeafOf(a), lb = LeafOf(b);
    for (int up = 0; up < 3 && Key(la) == Key(lb); ++up) {
        a = ParentOf(a);
        b = ParentOf(b);
        if (a.empty() || b.empty()) break;
        la = LeafOf(a) + L"\\" + la;
        lb = LeafOf(b) + L"\\" + lb;
    }
    if (Key(la) == Key(lb)) {
        la = st.root[0];
        lb = st.root[1];
    }
    st.label[0] = la;
    st.label[1] = lb;
}

void CmpRun(Tab& t) {
    CmpState& st = *t.cmpState;
    st.gen = ++st.live->gen;
    st.busy = true;
    st.dirty = false;
    st.live->dirs = 0;
    st.live->reading = 0;
    const SortBy by = static_cast<SortBy>(g_sortBy.load());
    std::thread(CmpThread, st.root[0], st.root[1], st.live, st.gen, g_cfg.showHidden, by, g_sortAsc.load(), FoldersFirstFor(by)).detach();
}

// Stops a comparison and forgets it (the tab goes back to its folder through StopRecursive).
void CmpStop(Tab& t) {
    if (t.cmpState) ++t.cmpState->live->gen;
    t.cmp = false;
    t.cmpState.reset();
}

void CompareFolders(Tab& t, const std::wstring& a, const std::wstring& b) {
    const std::wstring ka = Key(a), kb = Key(b);
    if (ka == kb) {
        S.message = L"That's the same folder twice";
    } else if (RecUnder(ka, kb) || RecUnder(kb, ka)) {
        S.message = L"One of them is inside the other: there's nothing to compare";
    } else if (a.empty() || b.empty() || IsVirtual(a) || IsVirtual(b) || InArchive(a) || InArchive(b)) {
        S.message = L"Only folders on a drive can be compared";
    } else {
        if (t.recursive) StopRecursive(t, false);
        CancelRename();
        t.recursive = true;
        t.recDone = false;
        t.recDirs = 0;
        t.recGen = 0;
        t.rec.clear();
        t.recKeys.clear();
        t.filter.clear();
        t.cmp = true;
        t.cmpState = std::make_shared<CmpState>();
        t.cmpState->root[0] = a;
        t.cmpState->root[1] = b;
        CmpLabels(*t.cmpState);
        Rebuild(t, nullptr, L"");
        CmpRun(t);
        S.message.clear();
    }
    Invalidate();
}

void CmpApply(WPARAM wp, LPARAM lp) {
    if (wp == 0) {  // progress
        for (auto& tp : g_tabs)
            if (tp->cmp && tp->cmpState) tp->cmpState->live->notify = false;
        Invalidate();
        return;
    }
    std::unique_ptr<CmpOut> out(reinterpret_cast<CmpOut*>(lp));
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.cmp || !t.cmpState || t.cmpState->live.get() != out->live || t.cmpState->gen != out->gen) continue;
        CmpState& st = *t.cmpState;
        st.busy = false;
        if (!out->error.empty()) {
            S.message = L"Couldn't compare: " + out->error;
            StopRecursive(t, true);
            break;
        }
        const bool had = t.recDone;
        Place p = SavePlace(t);  // before the listings go: the rows point to them
        t.rec = std::move(out->lists);
        t.recKeys.clear();
        for (const auto& l : t.rec) t.recKeys[Key(l->path)] = l.get();
        st.items = std::move(out->items);
        st.same = out->same;
        st.sameBytes = out->sameBytes;
        st.failed = out->failed;
        t.recDirs = out->dirs;
        t.recDone = true;
        Rebuild(t, had ? &p : nullptr, L"");
        if (!had) {
            t.scroll = 0;
            EnsureVisible(t);
        }
        if (st.again) {
            st.again = false;
            CmpRun(t);
        }
        Invalidate();
        break;
    }
}

// The rows: one for each thing that differs, as it is now (one deleted or moved away leaves the list), with what it is
// on the other side next to it. With a filter, only the ones that match it.
void BuildCmpRows(Tab& t) {
    CmpState* st = t.cmpState.get();
    if (!st) return;
    st->other.clear();
    for (int& n : st->shown) n = 0;
    if (!t.recDone) return;
    const bool filtered = !t.filter.empty();
    const FilterSpec spec = filtered ? ParseFilter(t.filter) : FilterSpec{};
    const NearPattern close(spec.fuzzy, g_cfg.typoMin);
    auto find = [&](const std::wstring& dirKey, const std::wstring& name, int& hint, const Listing*& l) {
        auto at = t.recKeys.find(dirKey);
        if (at == t.recKeys.end()) return -1;
        l = at->second;
        const bool hinted = hint >= 0 && hint < static_cast<int>(l->entries.size()) && l->entries[hint].nameLen == name.size() &&
                            _wcsnicmp(l->name(l->entries[hint]), name.c_str(), name.size()) == 0;
        if (!hinted) hint = FindName(*l, name);
        return hint;
    };
    for (CmpItem& it : st->items) {
        const Listing* l = nullptr;
        const int e = find(it.dirKey, it.name, it.hint, l);
        if (e < 0) continue;
        if (filtered) {
            int sc = 0, pc = 0;
            const Entry& en = l->entries[e];
            if (!FilterPass(spec, en, l->name(en), en.nameLen, sc, nullptr, close, pc) || pc) continue;
        }
        CmpOther o;
        o.group = it.group;
        if (!it.odirKey.empty()) {
            o.e = find(it.odirKey, it.oname, it.ohint, o.l);
            if (o.e < 0) o.l = nullptr;
        }
        t.rows.push_back({l, e, 1});
        st->other.push_back(o);
        ++st->shown[it.group];
    }
}

// A row's two sides: the entry on each (null where it isn't), the side the row itself is, and whether neither is newer.
struct CmpSides {
    const Entry* e[2] = {nullptr, nullptr};
    int from = 0;
    bool differ = false;
};
CmpSides CmpSidesOf(const Tab& t, int row) {
    CmpSides sd;
    const CmpState* st = t.cmpState.get();
    if (!st || row < 0 || row >= static_cast<int>(st->other.size()) || row >= static_cast<int>(t.rows.size())) return sd;
    const CmpOther& o = st->other[row];
    const Row& r = t.rows[row];
    sd.from = o.group == CmpOnlyB || o.group == CmpNewerB ? 1 : 0;
    sd.differ = o.group == CmpDiffer;
    sd.e[sd.from] = &r.l->entries[r.e];
    if (o.l && o.e >= 0 && o.e < static_cast<int>(o.l->entries.size())) sd.e[1 - sd.from] = &o.l->entries[o.e];
    return sd;
}

// Over the two sides, each folder's name.
void DrawCmpTitles(const Tab& t, const CmpCols& z, float y0, float y1) {
    const CmpState* st = t.cmpState.get();
    if (!st || z.detail < 0) return;
    Color(cText);
    for (int s = 0; s < 2; ++s) Text(st->label[s], g_fmtRight.Get(), z.l[s], y0, z.r[s], y1);
}

// A row's two sides, and between them the way it goes. The side that has it, or has it newer, is the brighter one.
void DrawCmpSides(const Tab& t, int row, const CmpCols& z, float y, float a, bool onSel) {
    const CmpSides sd = CmpSidesOf(t, row);
    if (!sd.e[0] && !sd.e[1]) return;
    const UINT32 strong = onSel ? 0xFFFFFF : cText, weak = onSel ? 0xC6D4EE : cDim, none = onSel ? 0x9FB3D9 : cFaint;
    const float date = z.date;
    wchar_t buf[64];
    for (int s = 0; z.detail >= 0 && s < 2; ++s) {
        const Entry* e = sd.e[s];
        if (!e) {  // not on this side
            Color(none, a);
            Text(L"—", g_fmtCenter.Get(), z.l[s], y, z.r[s], y + kRow);
            continue;
        }
        const bool lead = s == sd.from && !sd.differ;
        if (z.detail == 0) {  // no room for more than a word
            Color(lead ? strong : weak, a);
            Text(sd.differ ? (e->isDir() ? L"folder" : sd.e[1 - s] && sd.e[1 - s]->isDir() ? L"file" : L"differs") : !lead ? L"older" : sd.e[1 - s] ? L"newer" : L"only here",
                 g_fmtRight.Get(), z.l[s], y, z.r[s], y + kRow);
            continue;
        }
        if (e->mtime) {
            FormatDate(e->mtime, buf, 64);
            Color(lead ? strong : weak, a);
            Text(buf, g_fmtRight.Get(), z.l[s], y, z.detail == 2 ? z.l[s] + date : z.r[s], y + kRow);
        }
        if (z.detail == 2) {  // what differs with the same date is the size
            if (e->isDir()) wcscpy_s(buf, L"folder");
            else FormatSize(e->size, buf, 64);
            Color(lead || (sd.differ && !e->isDir()) ? strong : weak, a);
            Text(buf, g_fmtRight.Get(), z.l[s] + date + 14, y, z.r[s], y + kRow);
        }
    }
    if (sd.differ) {
        Color(onSel ? 0xFFFFFF : kDfAmber, a);
        Text(L"≠", g_fmtCenter.Get(), z.mid - 14, y, z.mid + 14, y + kRow);
    } else {
        Color(onSel ? 0xFFFFFF : cAccent, a);
        Glyph(sd.from == 0 ? gFwd : gBack, g_fmtCap.Get(), z.mid - 12, y, z.mid + 12, y + kRow);
    }
}

// For the tests. Which way a row goes: A+ and B+ only on that side, A>B and A<B newer on A and on B, A!=B neither.
std::wstring CmpRowMark(const Tab& t, int row) {
    const CmpSides sd = CmpSidesOf(t, row);
    if (sd.differ) return L"A!=B";
    if (!sd.e[1 - sd.from]) return sd.from ? L"B+" : L"A+";
    return sd.from ? L"A<B" : L"A>B";
}
// And what its two sides show: each one's size.
std::wstring CmpRowText(const Tab& t, int row) {
    const CmpSides sd = CmpSidesOf(t, row);
    auto side = [&](int s) -> std::wstring {
        if (!sd.e[s]) return L"—";
        if (sd.e[s]->isDir()) return L"folder";
        wchar_t buf[64];
        FormatSize(sd.e[s]->size, buf, 64);
        return buf;
    };
    return RowName(t, row) + L": " + side(0) + (sd.differ ? L" ≠ " : sd.from == 0 ? L" → " : L" ← ") + side(1);
}

// Which side a folder is on: 0, 1, or -1.
int CmpSide(const CmpState& st, const std::wstring& dir) {
    const std::wstring k = Key(dir);
    for (int s = 0; s < 2; ++s)
        if (RecUnder(k, Key(st.root[s]))) return s;
    return -1;
}

// Where a folder is below its side's folder ("" at the top), for the rows.
std::wstring CmpRelDir(const Tab& t, const std::wstring& dir) {
    if (!t.cmpState) return L"";
    const int s = CmpSide(*t.cmpState, dir);
    if (s < 0 || dir.size() <= t.cmpState->root[s].size()) return L"";
    std::wstring rel = dir.substr(t.cmpState->root[s].size());
    while (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
    return rel;
}

// The same place on the other side.
std::wstring CmpPartner(const CmpState& st, const std::wstring& path, int* side = nullptr) {
    const int s = CmpSide(st, path);
    if (side) *side = s;
    if (s < 0 || Key(path) == Key(st.root[s])) return L"";
    std::wstring rel = path.substr(st.root[s].size());
    while (!rel.empty() && rel[0] == L'\\') rel.erase(0, 1);
    return Join(st.root[1 - s], rel.c_str());
}

std::wstring CmpStatus(const Tab& t) {
    const CmpState* st = t.cmpState.get();
    if (!st) return L"";
    if (!t.recDone) {
        const int reading = st->live->reading.load();
        return L"Comparing   ·   " + Thousands(st->live->dirs.load()) + L" folders read" + (reading ? L"   ·   " + Thousands(reading) + L" files to read" : L"") + L"…";
    }
    int differ = 0;
    for (int g = 0; g < CmpGroups; ++g) differ += st->shown[g];
    const std::wstring s = Thousands(differ) + (differ == 1 ? L" difference" : L" differences");
    const std::wstring same = Thousands(st->same) + (st->same == 1 ? L" file" : L" files") + L" the same";
    const std::wstring busy = st->busy ? L"   ·   comparing again…" : L"";
    const std::wstring failed = st->failed ? L"   ·   " + Thousands(st->failed) + (st->failed == 1 ? L" folder couldn't be read" : L" folders couldn't be read") : L"";
    if (!differ)
        return (t.filter.empty() ? L"No differences: " + same : L"No differences match") + failed + busy + L"   ·   Esc goes back";
    return s + L"  ·  " + same + failed + busy + L"   ·   " + ShortcutFor(Cmd::CopyToOther) + L" copies the way the arrow points   ·   Esc goes back";
}

// A folder Jolt read again: if it is one of the compared ones, it shows as it is now, and the comparison runs again.
void CmpSoon(Tab& t) {
    if (!t.cmpState) return;
    t.cmpState->dirty = true;
    SetTimer(g_hwnd, kTimerCmp, 400, nullptr);
}

bool CmpTake(Tab& t, const std::shared_ptr<const Listing>& l) {
    CmpState* st = t.cmpState.get();
    if (!st || !t.recDone || IsVirtual(l->path)) return false;
    const std::wstring k = Key(l->path);
    if (CmpSide(*st, l->path) < 0) return false;
    auto at = t.recKeys.count(k) ? RecFind(t, l->path) : t.rec.end();
    if (at != t.rec.end() && !l->error && SameContent(**at, *l)) return false;
    CmpSoon(t);
    if (at == t.rec.end() || l->error) return false;  // the next comparison will see it
    Place p = SavePlace(t);
    *at = l;
    t.recKeys[k] = l.get();
    Rebuild(t, &p, L"");
    return true;
}

void CmpTimer() {
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.cmp || !t.cmpState || !t.cmpState->dirty || !t.recDone) continue;
        if (t.cmpState->busy) t.cmpState->again = true;
        else CmpRun(t);
        t.cmpState->dirty = false;
    }
    Invalidate();
}

// F5 / F6 in a comparison: each selected item to the same place on the other side, the way its arrow points.
bool CmpTransfer(bool move) {
    Tab& t = T();
    if (!t.cmp || !t.cmpState) return false;
    const CmpState& st = *t.cmpState;
    std::vector<std::wstring> files, dests;
    int nowhere = 0;
    for (int i = 0; i < RowCount(t); ++i) {
        if (!IsSel(t, i) || IsHeader(t, i)) continue;
        const std::wstring path = RowPath(t, i), there = CmpPartner(st, path);
        if (there.empty()) continue;
        const std::wstring dir = ParentOf(there);
        if (!PathExists(dir)) {
            ++nowhere;
            continue;
        }
        files.push_back(path);
        dests.push_back(dir);
    }
    if (files.empty()) {
        S.message = nowhere ? L"Its folder isn't on the other side anymore" : L"Select what to copy to the other side";
        Invalidate();
        return true;
    }
    if (FileOp(move ? FileOpKind::Move : FileOpKind::Copy, files, dests[0], 0, nullptr, nullptr, &dests)) {
        const int n = static_cast<int>(files.size());
        S.message = (move ? L"Moved " : L"Copied ") + (n == 1 ? LeafOf(files[0]) : Thousands(n) + L" items") + L" to the other side";
        if (nowhere) S.message += L"; " + Thousands(nowhere) + L" had no folder to go to there";
    }
    CmpSoon(t);
    Invalidate();
    return true;
}

// Ctrl+K → "compare" on one row of a comparison: that file with the one on the other side.
bool CmpComparePartner(Tab& t) {
    if (!t.cmp || !t.cmpState || t.sel < 0 || t.sel >= RowCount(t) || IsHeader(t, t.sel)) return false;
    int side = -1;
    const std::wstring path = RowPath(t, t.sel), there = CmpPartner(*t.cmpState, path, &side);
    if (there.empty()) return false;
    const DWORD a = GetFileAttributesW(ToExtendedPath(there).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) S.message = LeafOf(path) + L" is only on one side: there's nothing to compare it with";
    else if (EntryAt(t, t.sel).isDir() || (a & FILE_ATTRIBUTE_DIRECTORY)) S.message = L"A folder: open it, or compare its files one by one";
    else CompareFiles(side == 0 ? path : there, side == 0 ? there : path);
    Invalidate();
    return true;
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
    return n == 1 && (t.cmp || g_split);
}

// ---- Ctrl+K → "compare": what it works on
// Two selected here: those two (files, or folders). In split view: a file here with the one with its name in the other
// pane's folder (or else the one selected there), and otherwise the two panes' folders, the left one first.
void CompareCmd() {
    Tab& t = T();
    if (SelCount(t) <= 1 && CmpComparePartner(t)) return;
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
        else if (da) CompareFolders(t, a, b);
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
        CompareFolders(t, PT(0).path, PT(1).path);
        return;
    }
    S.message = L"Select two files or two folders to compare them, or open the other one in the other pane (split view, " + ShortcutFor(Cmd::Split) + L")";
    Invalidate();
}

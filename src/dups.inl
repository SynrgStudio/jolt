// Duplicate files (Ctrl+Alt+F): the subfolder search walks the folder, then the files that share a size are compared
// by a SHA-256 of their first 64 KB and, when those match, of the whole file. Each set of copies shows as a group
// with the oldest first. Everything that works on search results works here, and Ctrl+Shift+A selects the extra
// copies. Files that are only in the cloud are left out (comparing them would download them), and so are hard links
// to one same file, which don't take space twice.
// Included from app.cpp after search.inl.

constexpr UINT WM_APP_DUP = WM_APP + 13;
std::atomic<uint64_t> g_dupActive{0};  // the current comparison; older ones stop on their own
std::atomic<int> g_dupDoneFiles{0}, g_dupTotalFiles{0};
std::atomic<bool> g_dupNotify{false};

struct DupCand {
    uint64_t size = 0;
    std::vector<std::wstring> paths;
};
struct DupFound {
    uint64_t size = 0;
    std::vector<std::pair<int64_t, std::wstring>> files;  // (date modified, path)
};
struct DupResult {
    uint64_t gen = 0;
    std::vector<DupFound> sets;
};

// SHA-256 of the file's first `limit` bytes (all of it with UINT64_MAX). id: volume and file index, so hard links to
// one file are told apart from copies. mtime: for the order inside each set.
bool DupHashFile(BCRYPT_ALG_HANDLE alg, const std::wstring& path, uint64_t limit, uint64_t gen, std::string& out, std::string* id,
                 int64_t* mtime) {
    HANDLE f = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (id || mtime) {
        BY_HANDLE_FILE_INFORMATION bi{};
        if (GetFileInformationByHandle(f, &bi)) {
            if (id) *id = std::to_string(bi.dwVolumeSerialNumber) + ":" + std::to_string((static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow);
            if (mtime) *mtime = (static_cast<int64_t>(bi.ftLastWriteTime.dwHighDateTime) << 32) | bi.ftLastWriteTime.dwLowDateTime;
        }
    }
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    static thread_local std::vector<uint8_t> buf(1 << 20);
    uint64_t left = limit;
    while (ok && left > 0 && g_dupActive == gen) {
        const DWORD want = static_cast<DWORD>(std::min<uint64_t>(buf.size(), left));
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), want, &got, nullptr)) ok = false;
        if (!ok || got == 0) break;
        ok = BCRYPT_SUCCESS(BCryptHashData(h, buf.data(), got, 0));
        left -= got;
    }
    if (g_dupActive != gen) ok = false;
    out.assign(32, '\0');
    if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(h, reinterpret_cast<PUCHAR>(out.data()), 32, 0));
    if (h) BCryptDestroyHash(h);
    CloseHandle(f);
    return ok;
}

void DupThread(std::vector<DupCand> groups, uint64_t gen) {
    auto res = std::make_unique<DupResult>();
    res->gen = gen;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) alg = nullptr;
    constexpr uint64_t kHead = 64 * 1024;
    double last = NowMs();
    auto tick = [&] {
        g_dupDoneFiles.fetch_add(1);
        if (NowMs() - last > 150 && !g_dupNotify.exchange(true)) {
            last = NowMs();
            PostMessageW(g_hwnd, WM_APP_DUP, 0, 0);
        }
    };
    using Files = std::vector<std::pair<int64_t, std::wstring>>;
    std::sort(groups.begin(), groups.end(), [](const DupCand& a, const DupCand& b) { return a.size > b.size; });  // where the space is
    for (DupCand& g : groups) {
        if (g_dupActive != gen || !alg) break;
        // 1. the first 64 KB (the whole file when it is smaller)
        std::unordered_set<std::string> ids;
        std::unordered_map<std::string, Files> byHead;
        for (const std::wstring& p : g.paths) {
            std::string h, id;
            int64_t mt = 0;
            const bool ok = DupHashFile(alg, p, kHead, gen, h, &id, &mt);
            tick();
            if (!ok || (!id.empty() && !ids.insert(id).second)) continue;
            byHead[h].push_back({mt, p});
        }
        // 2. what matched there: the whole file
        for (auto& [h, files] : byHead) {
            if (files.size() < 2) continue;
            if (g.size <= kHead) {
                res->sets.push_back({g.size, std::move(files)});
                continue;
            }
            g_dupTotalFiles.fetch_add(static_cast<int>(files.size()));
            std::unordered_map<std::string, Files> byAll;
            for (auto& f : files) {
                std::string all;
                const bool ok = DupHashFile(alg, f.second, UINT64_MAX, gen, all, nullptr, nullptr);
                tick();
                if (ok) byAll[all].push_back(std::move(f));
            }
            for (auto& [h2, same] : byAll)
                if (same.size() >= 2) res->sets.push_back({g.size, std::move(same)});
        }
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (g_dupActive != gen) return;
    for (DupFound& s : res->sets) std::sort(s.files.begin(), s.files.end());  // the oldest first, it is the one kept
    std::stable_sort(res->sets.begin(), res->sets.end(), [](const DupFound& a, const DupFound& b) {
        return a.size * (a.files.size() - 1) > b.size * (b.files.size() - 1);  // the most space first
    });
    DupResult* r = res.release();
    if (!PostMessageW(g_hwnd, WM_APP_DUP, 1, reinterpret_cast<LPARAM>(r))) delete r;
}

// The walk is done: the files that share a size with another go to be compared.
void DupStart(Tab& t) {
    const bool hide = !g_cfg.showHidden;
    auto wanted = [&](const Entry& e) {
        return !e.isDir() && e.size > 0 && !(e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) && !OnlineOnly(e.attrs) &&
               !(hide && (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)));
    };
    std::unordered_map<uint64_t, int> count;
    for (const auto& l : t.rec)
        for (const Entry& e : l->entries)
            if (wanted(e)) ++count[e.size];
    std::unordered_map<uint64_t, std::vector<std::wstring>> bySize;
    int total = 0;
    for (const auto& l : t.rec)
        for (const Entry& e : l->entries) {
            if (!wanted(e) || count[e.size] < 2) continue;
            bySize[e.size].push_back(Join(l->path, l->name(e)));
            ++total;
        }
    std::vector<DupCand> groups;
    for (auto& [size, paths] : bySize) groups.push_back({size, std::move(paths)});
    t.dupGen = ++g_dupActive;
    t.dupHashing = true;
    g_dupDoneFiles = 0;
    g_dupTotalFiles = total;
    std::thread(DupThread, std::move(groups), t.dupGen).detach();
}

void DupApply(WPARAM wp, LPARAM lp) {
    if (wp == 0) {  // progress
        g_dupNotify = false;
        Invalidate();
        return;
    }
    std::unique_ptr<DupResult> r(reinterpret_cast<DupResult*>(lp));
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.dups || t.dupGen != r->gen) continue;
        auto head = std::make_shared<Listing>();  // the group titles: one per set
        head->path = t.path;
        t.dupSets.clear();
        for (DupFound& s : r->sets) {
            DupSet d;
            d.size = s.size;
            for (auto& [mt, p] : s.files) d.files.push_back({Key(ParentOf(p)), LeafOf(p)});
            wchar_t sz[64];
            FormatSize(s.size, sz, 64);
            head->groups.push_back({0, LeafOf(s.files[0].second) + L"  ·  " + sz});
            t.dupSets.push_back(std::move(d));
        }
        t.dupHead = head;
        t.dupHashing = false;
        t.dupDone = true;
        Rebuild(t, nullptr, L"");
        Invalidate();
    }
}

// The rows: each set under its title, with the copies it still has (deleting or moving one updates it). A set left
// with one copy stays, saying so: the list going empty after Del would look like everything was deleted.
// With a filter, the sets where some copy matches, with all their copies.
void BuildDupRows(Tab& t) {
    if (!t.dupDone || !t.dupHead) return;
    const bool filtered = !t.filter.empty();
    const FilterSpec spec = filtered ? ParseFilter(t.filter) : FilterSpec{};
    const NearPattern close(spec.fuzzy, g_cfg.typoMin);
    std::vector<Row> members;
    for (size_t si = 0; si < t.dupSets.size(); ++si) {
        DupSet& s = t.dupSets[si];
        s.shown = 0;
        members.clear();
        bool match = !filtered;
        for (DupFile& f : s.files) {
            auto it = t.recKeys.find(f.dirKey);
            if (it == t.recKeys.end()) continue;
            const Listing* l = it->second;
            const bool hinted = f.hint >= 0 && f.hint < static_cast<int>(l->entries.size()) && l->entries[f.hint].nameLen == f.name.size() &&
                                _wcsnicmp(l->name(l->entries[f.hint]), f.name.c_str(), f.name.size()) == 0;
            const int i = hinted ? f.hint : FindName(*l, f.name);
            f.hint = i;
            if (i < 0 || l->entries[i].size != s.size) continue;  // gone, or changed since
            members.push_back({l, i, 1});
            if (!match) {
                int sc = 0, pc = 0;
                const Entry& e = l->entries[i];
                match = FilterPass(spec, e, l->name(e), e.nameLen, sc, nullptr, close, pc) && !pc;
            }
        }
        if (members.empty() || !match) continue;
        s.shown = static_cast<int>(members.size());
        t.rows.push_back({t.dupHead.get(), -1 - static_cast<int>(si), 0});
        if (!GroupFolded(t, t.dupHead.get(), si)) t.rows.insert(t.rows.end(), members.begin(), members.end());
    }
}

// Ctrl+Shift+A: every copy but the first (the oldest) of each set, ready for Del.
void SelectExtraCopies(Tab& t) {
    if (!t.dups || !t.dupDone) {
        S.message = L"Only in duplicate results: " + ShortcutFor(Cmd::FindDuplicates) + L" looks for them";
        return;
    }
    t.implicitSel = false;
    int n = 0, firstSel = -1;
    uint64_t bytes = 0;
    bool keep = false;
    for (int i = 0; i < RowCount(t); ++i) {
        if (IsHeader(t, i)) {
            t.selMask[i] = 0;
            keep = true;
            continue;
        }
        t.selMask[i] = !keep;
        if (!keep) {
            ++n;
            bytes += EntryAt(t, i).size;
            if (firstSel < 0) firstSel = i;
        }
        keep = false;
    }
    if (firstSel >= 0) {
        t.sel = t.anchor = firstSel;
        EnsureVisible(t);
    }
    wchar_t sz[64];
    FormatSize(bytes, sz, 64);
    S.message = n ? Thousands(n) + (n == 1 ? L" extra copy selected, " : L" extra copies selected, ") + sz + L"; the oldest of each set stays"
                  : L"No extra copies to select";
}

// The status line in duplicate results.
std::wstring DupStatus(const Tab& t) {
    if (!t.recDone) return L"Looking for duplicates   ·   " + Thousands(t.recDirs) + L" folders read…";
    if (!t.dupDone) return L"Comparing files   ·   " + Thousands(g_dupDoneFiles.load()) + L" of " + Thousands(g_dupTotalFiles.load()) + L"…";
    int sets = 0, extra = 0, single = 0;
    uint64_t bytes = 0;
    for (const DupSet& s : t.dupSets) {
        if (s.shown == 1) ++single;
        if (s.shown < 2) continue;
        ++sets;
        extra += s.shown - 1;
        bytes += s.size * static_cast<uint64_t>(s.shown - 1);
    }
    const std::wstring left1 = single ? L"   ·   " + Thousands(single) + (single == 1 ? L" set down to one copy" : L" sets down to one copy") : L"";
    if (!sets) {
        if (single) return L"No extra copies left" + left1 + L"   ·   Esc goes back";
        return t.filter.empty() ? L"No duplicates   ·   Esc goes back" : L"No duplicates match   ·   Esc clears";
    }
    wchar_t sz[64];
    FormatSize(bytes, sz, 64);
    return Thousands(sets) + (sets == 1 ? L" set" : L" sets") + L"  ·  " + Thousands(extra) + (extra == 1 ? L" extra copy" : L" extra copies") + L"  ·  " + sz +
           L" could be freed" + left1 + L"   ·   " + ShortcutFor(Cmd::SelectExtraCopies) + L" selects them   ·   Esc goes back";
}

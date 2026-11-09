// Global drive index, so Ctrl+P can find any folder or file instantly
// (the Everything idea). Each fixed drive stores a compact tree: one node per item with
// the index of its parent folder and its name in a shared block.
//  - If Jolt runs as administrator and the drive is NTFS, the whole disk table (MFT) is read:
//    seconds for millions of files. Otherwise it's walked with NtQueryDirectoryFile in the background.
//  - Saved to %LOCALAPPDATA%\Jolt\index-X.bin: it's available right at startup and gets
//    rebuilt in the background (at low disk priority) to catch up.
//  - What's created while Jolt is open is added automatically (each drive is watched); what's
//    deleted is dropped when showing results.
//  - Fuzzy search runs on several threads and stops if typing continues.
// Included from app.cpp before palette.inl.

constexpr UINT WM_APP_IDX = WM_APP + 8;

struct IdxNode {
    uint32_t parent;  // UINT32_MAX = root
    uint32_t nameOff;
    uint16_t nameLen;
    uint8_t dir;
    uint8_t depth;
};
// While being built it lives in the vectors. Once saved, it's read straight from the file: Windows brings in only
// what's used and can release it at any time, so it takes up none of Jolt's own memory.
struct IdxVolume {
    wchar_t letter = 0;
    std::vector<IdxNode> nodes;  // nodes[0] = the root ("C:")
    std::vector<wchar_t> names;
    const IdxNode* node = nullptr;
    const wchar_t* name = nullptr;
    size_t count = 0;
    const void* view = nullptr;
    IdxVolume() = default;
    IdxVolume(const IdxVolume&) = delete;
    ~IdxVolume() {
        if (view) UnmapViewOfFile(view);
    }
    void Seal() {
        node = nodes.data();
        name = names.data();
        count = nodes.size();
    }
};

std::mutex g_ixm;
std::vector<std::shared_ptr<const IdxVolume>> g_ixVols;
std::vector<std::wstring> g_ixAdded;  // created this session (full paths)
std::atomic<uint64_t> g_ixCount{0};
std::atomic<int> g_ixBuilding{0};
std::atomic<uint64_t> g_ixGen{0};  // current search

struct IdxResult {
    uint64_t gen = 0;
    std::wstring query;
    std::vector<std::wstring> hits;
    uint64_t total = 0;  // how many there are, shown or not
    size_t limit = 0;    // how many were asked for
};
IdxResult g_ixRes;          // last result (window thread)
std::wstring g_ixAsked;     // last search requested
size_t g_ixLimit = 0;       // how many results of it are wanted: more as the list is scrolled
constexpr size_t kIdxPage = 200, kIdxMost = 12800;
// Something in the results no longer exists.
struct IdxGone {
    uint64_t gen;
    std::wstring path;
};
constexpr UINT WM_APP_IDXGONE = WM_APP + 12;

// Status bar notice (from any thread).
constexpr UINT WM_APP_IDXMSG = WM_APP + 9;
void IdxStatus(const std::wstring& s) {
    auto* p = new std::wstring(s);
    if (!PostMessageW(g_hwnd, WM_APP_IDXMSG, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

bool g_ixTestOnly = false;  // tests: the index is only the folder they asked for (IdxTestOnly), nothing replaces it

void IdxPublish(std::shared_ptr<const IdxVolume> v) {
    std::lock_guard lk(g_ixm);
    if (g_ixTestOnly) return;
    bool replaced = false;
    for (auto& x : g_ixVols)
        if (x->letter == v->letter) {
            x = v;
            replaced = true;
        }
    if (!replaced) g_ixVols.push_back(v);
    uint64_t n = 0;
    for (auto& x : g_ixVols) n += x->count;
    g_ixCount = n;
}

void IdxAddNode(IdxVolume& v, uint32_t parent, const wchar_t* name, size_t len, bool dir) {
    IdxNode n;
    n.parent = parent;
    n.nameOff = static_cast<uint32_t>(v.names.size());
    n.nameLen = static_cast<uint16_t>(std::min<size_t>(len, 65535));
    n.dir = dir;
    n.depth = parent == UINT32_MAX ? 0 : static_cast<uint8_t>(std::min(250, v.nodes[parent].depth + 1));
    v.names.insert(v.names.end(), name, name + n.nameLen);
    v.nodes.push_back(n);
}

// A folder and everything below it, into the index under the node top.
void IdxWalkFrom(IdxVolume& v, uint32_t top, const std::wstring& topPath, bool progress) {
    std::vector<std::pair<uint32_t, std::wstring>> stack{{top, topPath}};
    Listing l;
    uint64_t base = g_ixCount;
    while (!stack.empty()) {
        auto [idx, path] = std::move(stack.back());
        stack.pop_back();
        l = Listing{};
        EnumerateDir(path, EnumMethod::Nt, l);
        for (const Entry& e : l.entries) {
            uint32_t me = static_cast<uint32_t>(v.nodes.size());
            IdxAddNode(v, idx, l.name(e), e.nameLen, e.isDir());
            if (e.isDir() && !(e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back({me, Join(path, l.name(e))});
        }
        if (progress && (v.nodes.size() & 0x3FFF) < l.entries.size()) g_ixCount = base + v.nodes.size();
    }
}

// Normal walk (no special permissions).
std::shared_ptr<IdxVolume> IdxWalk(wchar_t letter) {
    auto v = std::make_shared<IdxVolume>();
    v->letter = letter;
    wchar_t root[3] = {letter, L':', 0};
    IdxAddNode(*v, UINT32_MAX, root, 2, true);
    IdxWalkFrom(*v, 0, std::wstring(root) + L"\\", true);
    v->Seal();
    return v;
}

// Direct read of the disk table (NTFS, administrator only).
std::shared_ptr<IdxVolume> IdxMft(wchar_t letter) {
    wchar_t root[4] = {letter, L':', L'\\', 0}, fs[32] = {};
    if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 32) || wcscmp(fs, L"NTFS") != 0) return nullptr;
    wchar_t dev[8] = {L'\\', L'\\', L'.', L'\\', letter, L':', 0};
    HANDLE h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;  // no permission: walk it normally
    struct Raw {
        uint64_t frn, parent;
        uint32_t nameOff;
        uint16_t nameLen;
        bool dir;
    };
    std::vector<Raw> raws;
    std::vector<wchar_t> names;
    MFT_ENUM_DATA_V0 med{0, 0, MAXLONGLONG};
    std::vector<BYTE> buf(1 << 20);
    DWORD got = 0;
    while (DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) &&
           got > sizeof(USN)) {
        BYTE* p = buf.data() + sizeof(USN);
        while (p < buf.data() + got) {
            auto* r = reinterpret_cast<USN_RECORD_V2*>(p);
            if (r->RecordLength == 0) break;
            if (r->MajorVersion == 2) {
                const wchar_t* nm = reinterpret_cast<const wchar_t*>(p + r->FileNameOffset);
                uint16_t len = static_cast<uint16_t>(r->FileNameLength / sizeof(wchar_t));
                raws.push_back({r->FileReferenceNumber, r->ParentFileReferenceNumber, static_cast<uint32_t>(names.size()), len,
                                (r->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
                names.insert(names.end(), nm, nm + len);
            }
            p += r->RecordLength;
        }
        med.StartFileReferenceNumber = *reinterpret_cast<DWORDLONG*>(buf.data());
    }
    CloseHandle(h);
    if (raws.empty()) return nullptr;
    std::unordered_map<uint64_t, uint32_t> byFrn;
    byFrn.reserve(raws.size());
    for (size_t i = 0; i < raws.size(); ++i) byFrn[raws[i].frn] = static_cast<uint32_t>(i);
    // parents before children (to compute depth and make sure the parent exists first)
    auto v = std::make_shared<IdxVolume>();
    v->letter = letter;
    wchar_t rootName[3] = {letter, L':', 0};
    IdxAddNode(*v, UINT32_MAX, rootName, 2, true);
    std::vector<uint32_t> nodeOf(raws.size(), UINT32_MAX);
    // the drive root is its own parent
    for (size_t i = 0; i < raws.size(); ++i) {
        auto it = byFrn.find(raws[i].parent);
        if (it != byFrn.end() && it->second == i) nodeOf[i] = 0;
    }
    std::vector<uint32_t> chain;
    for (size_t i = 0; i < raws.size(); ++i) {
        if (nodeOf[i] != UINT32_MAX) continue;
        // climb to an already placed ancestor (or the root), then descend adding nodes, so the parent always exists
        chain.clear();
        uint32_t cur = static_cast<uint32_t>(i), parentNode = 0;
        for (int guard = 0; guard < 512; ++guard) {
            chain.push_back(cur);
            auto it = byFrn.find(raws[cur].parent);
            if (it == byFrn.end()) break;
            if (nodeOf[it->second] != UINT32_MAX) {
                parentNode = nodeOf[it->second];
                break;
            }
            cur = it->second;
        }
        for (size_t k = chain.size(); k-- > 0;) {
            const Raw& r = raws[chain[k]];
            if (nodeOf[chain[k]] != UINT32_MAX) {
                parentNode = nodeOf[chain[k]];
                continue;
            }
            if (parentNode == 0 && r.nameLen > 0 && names[r.nameOff] == L'$') {  // NTFS internal files
                nodeOf[chain[k]] = 0;
                continue;
            }
            nodeOf[chain[k]] = static_cast<uint32_t>(v->nodes.size());
            IdxAddNode(*v, parentNode, names.data() + r.nameOff, r.nameLen, r.dir);
            parentNode = nodeOf[chain[k]];
        }
    }
    v->Seal();
    return v;
}

std::wstring IdxFile(wchar_t letter) { return LocalDataDir() + L"\\index-" + std::wstring(1, letter) + L".bin"; }

// A new index is written separately (".new"): the one in use can't be overwritten while it's mapped.
std::wstring IdxNewFile(wchar_t letter) { return IdxFile(letter) + L".new"; }

void IdxSave(const IdxVolume& v) {
    std::wstring f = IdxNewFile(v.letter), tmp = IdxFile(v.letter) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    uint32_t head[3] = {0x58445952, static_cast<uint32_t>(v.nodes.size()), static_cast<uint32_t>(v.names.size())};  // "RYDX"
    DWORD w;
    bool ok = WriteFile(h, head, sizeof(head), &w, nullptr) &&
              WriteFile(h, v.nodes.data(), static_cast<DWORD>(v.nodes.size() * sizeof(IdxNode)), &w, nullptr) &&
              WriteFile(h, v.names.data(), static_cast<DWORD>(v.names.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::shared_ptr<IdxVolume> IdxLoad(wchar_t letter) {
    HANDLE h = CreateFileW(IdxFile(letter).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    HANDLE m = GetFileSizeEx(h, &size) && size.QuadPart >= 12 ? CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(h);
    if (!m) return nullptr;
    const void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);  // the view keeps the mapping alive
    if (!view) return nullptr;
    const auto* head = static_cast<const uint32_t*>(view);
    const uint64_t want = 12 + static_cast<uint64_t>(head[1]) * sizeof(IdxNode) + static_cast<uint64_t>(head[2]) * sizeof(wchar_t);
    if (head[0] != 0x58445952 || static_cast<uint64_t>(size.QuadPart) != want) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    auto v = std::make_shared<IdxVolume>();
    v->letter = letter;
    v->view = view;
    v->count = head[1];
    v->node = reinterpret_cast<const IdxNode*>(head + 3);
    v->name = reinterpret_cast<const wchar_t*>(v->node + head[1]);
    return v;
}

// Switches to the new index: releases the previous mapping (a running search holds it for a moment),
// replaces the file and maps it. An index still living in memory stays visible until then.
bool IdxPromote(wchar_t letter) {
    const std::wstring fresh = IdxNewFile(letter);
    if (GetFileAttributesW(fresh.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    {
        std::lock_guard lk(g_ixm);
        for (size_t i = 0; i < g_ixVols.size(); ++i)
            if (g_ixVols[i]->letter == letter && g_ixVols[i]->view) {
                g_ixVols.erase(g_ixVols.begin() + i);
                break;
            }
    }
    bool moved = false;
    for (int i = 0; i < 50 && !moved; ++i) {
        moved = MoveFileExW(fresh.c_str(), IdxFile(letter).c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!moved) Sleep(100);
    }
    if (auto v = IdxLoad(letter)) IdxPublish(v);
    return moved;
}

// Watches a whole drive: whatever is created is added to this session's list.
void IdxWatch(wchar_t letter) {
    wchar_t root[4] = {letter, L':', L'\\', 0};
    HANDLE d = CreateFileW(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (d == INVALID_HANDLE_VALUE) return;
    std::vector<BYTE> buf(64 * 1024);
    DWORD got = 0;
    while (ReadDirectoryChangesW(d, buf.data(), static_cast<DWORD>(buf.size()), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, &got, nullptr, nullptr)) {
        if (!got) continue;  // overflowed: that batch is lost (the next build recovers it)
        std::lock_guard lk(g_ixm);
        for (BYTE* p = buf.data();;) {
            auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
            if (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
                if (g_ixAdded.size() < 20000) g_ixAdded.push_back(root + rel);
            }
            if (!fni->NextEntryOffset) break;
            p += fni->NextEntryOffset;
        }
    }
    CloseHandle(d);
}

// Hours since a drive's index was saved (or a large number if there's none).
double IdxAgeHours(wchar_t letter) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(IdxFile(letter).c_str(), GetFileExInfoStandard, &d)) return 1e9;
    int64_t t = (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return static_cast<double>(NowFileTime() - t) / 36000000000.0;
}

std::vector<wchar_t> IdxLetters() {
    std::vector<wchar_t> letters;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED) letters.push_back(root[0]);
    }
    return letters;
}

// "jolt.exe --index-mft" mode: Jolt launches it as administrator; reads each drive's MFT, saves and exits.
void IdxElevatedMain() {
    for (wchar_t c : IdxLetters())
        if (auto v = IdxMft(c)) IdxSave(*v);
}

// Asks for administrator permission once and reindexes by reading each drive's table (seconds).
void IdxReindexElevated() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"--index-mft";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        IdxStatus(L"Reindex cancelled (administrator permission is required)");
        return;
    }
    g_ixBuilding = 1;
    IdxStatus(L"Reindexing the drives…");
    HANDLE proc = sei.hProcess;
    std::thread([proc] {
        // each drive is used as soon as its index is written, and progress is reported
        const double t0 = NowMs();
        const std::vector<wchar_t> letters = IdxLetters();
        std::vector<bool> done(letters.size(), false);
        std::wstring progress;
        auto check = [&] {
            for (size_t i = 0; i < letters.size(); ++i) {
                if (!done[i] && IdxPromote(letters[i])) {
                    done[i] = true;
                    wchar_t b[64];
                    swprintf(b, 64, L"%c: %.0f s", letters[i], (NowMs() - t0) / 1000.0);
                    progress += (progress.empty() ? L"" : L"   ·   ") + std::wstring(b);
                    IdxStatus(L"Reindexing…   " + progress);
                }
            }
        };
        while (WaitForSingleObject(proc, 500) == WAIT_TIMEOUT) check();
        check();
        CloseHandle(proc);
        g_ixBuilding = 0;
        int ok = 0;
        for (bool d : done) ok += d;
        wchar_t b[160];
        if (ok == 0)
            swprintf(b, 160, L"Couldn't reindex: couldn't read the drives' tables");
        else
            swprintf(b, 160, L"Index ready: %s items in %.0f s%s", Thousands(static_cast<int>(std::min<uint64_t>(g_ixCount, INT_MAX))).c_str(),
                     (NowMs() - t0) / 1000.0, ok < static_cast<int>(letters.size()) ? L" (some drives couldn't be read)" : L"");
        IdxStatus(b);
    }).detach();
}

void IdxBuilder() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // disk and CPU in the background
    const std::vector<wchar_t> letters = IdxLetters();
    for (wchar_t c : letters) {
        IdxPromote(c);  // one left unused because Jolt closed first
        if (auto v = IdxLoad(c)) IdxPublish(v);  // last run's index, right away
    }
    for (wchar_t c : letters) std::thread(IdxWatch, c).detach();
    for (;;) {
        for (wchar_t c : letters) {
            // an index under 12 hours old is good enough (new items join live; deleted ones are dropped when shown)
            if (IdxAgeHours(c) < 12) continue;
            g_ixBuilding = 1;
            auto v = IdxMft(c);
            if (!v) v = IdxWalk(c);
            IdxPublish(v);
            IdxSave(*v);
            v.reset();
            IdxPromote(c);  // moves from the vectors to the file
            std::lock_guard lk(g_ixm);
            const std::wstring pre = std::wstring(1, c) + L":";
            g_ixAdded.erase(std::remove_if(g_ixAdded.begin(), g_ixAdded.end(), [&](const std::wstring& p) { return p.rfind(pre, 0) == 0; }),
                            g_ixAdded.end());
        }
        g_ixBuilding = 0;
        Sleep(30 * 60 * 1000);  // every half hour, check whether any index has gone stale
    }
}

std::wstring IdxPath(const IdxVolume& v, uint32_t i) {
    std::vector<uint32_t> chain;
    for (uint32_t c = i; c != UINT32_MAX && chain.size() < 300; c = v.node[c].parent) chain.push_back(c);
    std::wstring p;
    for (size_t k = chain.size(); k-- > 0;) {
        const IdxNode& n = v.node[chain[k]];
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p.append(v.name + n.nameOff, n.nameLen);
        if (k == chain.size() - 1) p += L'\\';  // "C:\"
    }
    return p;
}

// The nodes of a folder and of the folders above it in a drive's index (chain[d]: the one at depth d), or nothing if it
// isn't in it. One pass over the whole drive: the last answer is kept.
std::vector<uint32_t> IdxChain(const IdxVolume& v, const std::wstring& folder) {
    std::vector<std::pair<size_t, size_t>> parts;  // "C:\Users\me": where "Users" and "me" start, and how long they are
    for (size_t i = 3; i < folder.size();) {
        size_t j = folder.find(L'\\', i);
        if (j == std::wstring::npos) j = folder.size();
        if (j > i) parts.push_back({i, j - i});
        i = j + 1;
    }
    if (parts.empty() || parts.size() >= 250) return {};
    const size_t D = parts.size();
    std::vector<std::vector<uint32_t>> at(D + 1);
    for (size_t i = 1; i < v.count; ++i) {
        const IdxNode& nd = v.node[i];
        if (nd.depth == 0 || nd.depth > D || !nd.dir) continue;
        const auto [off, len] = parts[nd.depth - 1];
        if (nd.nameLen == len &&
            CompareStringOrdinal(v.name + nd.nameOff, nd.nameLen, folder.c_str() + off, static_cast<int>(len), TRUE) == CSTR_EQUAL)
            at[nd.depth].push_back(static_cast<uint32_t>(i));
    }
    std::vector<uint32_t> chain{0};
    for (size_t d = 1; d <= D; ++d) {
        auto it = std::find_if(at[d].begin(), at[d].end(), [&](uint32_t c) { return v.node[c].parent == chain.back(); });
        if (it == at[d].end()) return {};
        chain.push_back(*it);
    }
    return chain;
}

std::mutex g_ixChainM;
struct {
    std::weak_ptr<const IdxVolume> vol;  // not kept alive by this: a newer index replaces its file
    std::wstring folder;
    std::vector<uint32_t> chain;
} g_ixChain;

std::vector<uint32_t> IdxChainOf(const std::shared_ptr<const IdxVolume>& v, const std::wstring& folder) {
    {
        std::lock_guard lk(g_ixChainM);
        if (!g_ixChain.vol.expired() && !g_ixChain.vol.owner_before(v) && !v.owner_before(g_ixChain.vol) && Key(g_ixChain.folder) == Key(folder))
            return g_ixChain.chain;
    }
    std::vector<uint32_t> chain = IdxChain(*v, folder);
    std::lock_guard lk(g_ixChainM);
    g_ixChain.vol = v;
    g_ixChain.folder = folder;
    g_ixChain.chain = chain;
    return chain;
}

// NearFolderPoints for node i, chain being the folder looked at and the ones above it, on the same drive.
int IdxNear(const IdxVolume& v, uint32_t i, const std::vector<uint32_t>& chain) {
    if (chain.size() < 2) return 0;
    const int D = static_cast<int>(chain.size()) - 1, di = v.node[i].depth;
    if (di >= 250) return 0;  // depths stop being counted there
    uint32_t c = i;
    int dc = di;
    for (; dc > D; --dc) c = v.node[c].parent;
    for (; dc > 0 && c != chain[dc]; --dc) {
        if (dc <= D - 2) return 0;  // they meet more than two levels up: not near
        c = v.node[c].parent;
    }
    return NearFolderPoints(dc, D - dc, di - dc);
}

// Searches on a separate thread and notifies the window with the best results. here: the folder being looked at, what is
// near it ranks higher.
void IdxSearch(std::wstring query, uint64_t gen, size_t limit, std::wstring here) {
    std::vector<std::shared_ptr<const IdxVolume>> vols;
    std::vector<std::wstring> added;
    {
        std::lock_guard lk(g_ixm);
        vols = g_ixVols;
        added = g_ixAdded;
    }
    const std::wstring pat = Lower(query);
    const NearPattern close(pat, g_cfg.typoMin);
    uint32_t hereVol = UINT32_MAX;
    std::vector<uint32_t> chain;
    for (uint32_t vi = 0; vi < vols.size() && here.size() >= 3 && here[1] == L':'; ++vi)
        if (towupper(vols[vi]->letter) == towupper(here[0])) {
            hereVol = vi;
            chain = IdxChainOf(vols[vi], here);
        }
    struct Hit {
        int key;
        uint32_t vol, node;  // vol = UINT32_MAX: from "added"
        bool operator<(const Hit& o) const { return key > o.key; }  // min-heap
    };
    const size_t kTop = limit;  // per thread: they could all be in one
    std::atomic<uint64_t> total{0};
    unsigned nth = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    // Every name through keyOf, on several threads, keeping the best of each.
    auto scan = [&](auto keyOf, std::vector<Hit>& out) {
        std::vector<std::vector<Hit>> part(nth);
        std::vector<std::thread> th;
        for (unsigned t = 0; t < nth; ++t) {
            th.emplace_back([&, t] {
                auto& heap = part[t];
                int key;
                for (uint32_t vi = 0; vi < vols.size(); ++vi) {
                    const IdxVolume& v = *vols[vi];
                    const size_t n = v.count;
                    for (size_t i = 1 + t; i < n; i += nth) {
                        if ((i & 0xFFFF) < nth && g_ixGen != gen) return;
                        const IdxNode& nd = v.node[i];
                        if (!keyOf(v.name + nd.nameOff, nd, key, vi, static_cast<uint32_t>(i))) continue;
                        if (heap.size() < kTop) {
                            heap.push_back({key, vi, static_cast<uint32_t>(i)});
                            std::push_heap(heap.begin(), heap.end());
                        } else if (key > heap.front().key) {
                            std::pop_heap(heap.begin(), heap.end());
                            heap.back() = {key, vi, static_cast<uint32_t>(i)};
                            std::push_heap(heap.begin(), heap.end());
                        }
                    }
                }
            });
        }
        for (auto& x : th) x.join();
        for (auto& p : part) out.insert(out.end(), p.begin(), p.end());
    };
    // The best ones, to the window, without touching the disks: on a mechanical one checking each result takes up to
    // half a second the first time, and that's what used to make the list wait.
    auto publish = [&](std::vector<Hit>& all) {
        std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) { return a.key > b.key; });
        auto res = std::make_unique<IdxResult>();
        res->gen = gen;
        res->query = query;
        res->total = total;
        res->limit = limit;
        std::unordered_set<std::wstring> seen;
        for (const Hit& h : all) {
            if (res->hits.size() >= limit) break;
            std::wstring p = h.vol == UINT32_MAX ? added[h.node] : IdxPath(*vols[h.vol], h.node);
            if (seen.insert(Key(p)).second) res->hits.push_back(std::move(p));
        }
        std::vector<std::wstring> shown = res->hits;
        IdxResult* raw = res.release();
        if (!PostMessageW(g_hwnd, WM_APP_IDX, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        return shown;
    };
    std::vector<Hit> all;
    scan([&](const wchar_t* name, const IdxNode& nd, int& key, uint32_t vi, uint32_t i) {
        int sc;
        if (!FuzzyMatch(name, nd.nameLen, pat, sc, nullptr)) return false;
        const int nearPts = vi == hereVol ? IdxNear(*vols[vi], i, chain) : 0;
        key = (sc + NameRank(name, nd.nameLen, pat) + nearPts) * 8 - std::min<int>(nd.depth, 40) + (nd.dir ? 4 : 0);
        total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }, all);
    if (g_ixGen != gen) return;
    // what was created this session
    int sc;
    for (uint32_t i = 0; i < added.size(); ++i) {
        std::wstring leaf = LeafOf(added[i]);
        if (FuzzyMatch(leaf.c_str(), leaf.size(), pat, sc, nullptr)) {
            all.push_back({(sc + NameRank(leaf.c_str(), leaf.size(), pat) + NearFolderPoints(added[i], here)) * 8, UINT32_MAX, i});
            ++total;
        }
    }
    std::vector<std::wstring> shown = publish(all);
    // Then the names close to it, in a second pass: going through all the disks again takes longer than the real
    // matches, and those shouldn't have to wait. A list full of good matches has no room for them anyway.
    const auto good = std::count_if(all.begin(), all.end(), [](const Hit& h) { return h.key > kNearFloor / 2; });
    if (close.mistakes >= 0 && good < 40) {
        const size_t known = all.size();
        scan([&](const wchar_t* name, const IdxNode& nd, int& key, uint32_t, uint32_t) {
            const int percent = NearMatch(name, nd.nameLen, close);
            if (!percent) return false;
            key = NearScore(percent, nd.nameLen);
            ++total;
            return true;
        }, all);
        if (g_ixGen != gen) return;
        for (uint32_t i = 0; i < added.size(); ++i) {
            std::wstring leaf = LeafOf(added[i]);
            if (const int percent = NearMatch(leaf.c_str(), leaf.size(), close)) {
                all.push_back({NearScore(percent, leaf.size()), UINT32_MAX, i});
                ++total;
            }
        }
        if (all.size() > known) shown = publish(all);
    }
    // With the list already on screen, what no longer exists comes off it. In the background so it doesn't get in the
    // way of the disks, and only while this is still the search that matters.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (const std::wstring& p : shown) {
        if (g_ixGen != gen) return;
        if (GetFileAttributesW(ToExtendedPath(p).c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() == ERROR_ACCESS_DENIED) continue;
        auto* gone = new IdxGone{gen, p};
        if (!PostMessageW(g_hwnd, WM_APP_IDXGONE, 0, reinterpret_cast<LPARAM>(gone))) delete gone;
    }
}

std::wstring g_ixHere;  // the folder being looked at when it was asked

void IdxRequest(const std::wstring& query, const std::wstring& here) {
    if (query == g_ixAsked) return;
    g_ixAsked = query;
    g_ixHere = here;
    g_ixLimit = kIdxPage;
    uint64_t gen = ++g_ixGen;
    if (query.size() < 2) return;
    std::thread(IdxSearch, query, gen, g_ixLimit, here).detach();
}

// The end of the list is near: the same search, with room for more.
void IdxMore() {
    if (g_ixRes.query != g_ixAsked || g_ixRes.limit < g_ixLimit || g_ixRes.hits.size() >= g_ixRes.total || g_ixLimit >= kIdxMost) return;
    g_ixLimit *= 2;
    const uint64_t gen = ++g_ixGen;
    std::thread(IdxSearch, g_ixAsked, gen, g_ixLimit, g_ixHere).detach();
}

// How many names in the index are a real match for the pattern (lowercase), on several threads. Stops (with whatever it
// counted) when current is no longer gen.
uint64_t IdxCountMatches(const std::wstring& pat, const std::atomic<uint64_t>& current, uint64_t gen) {
    std::vector<std::shared_ptr<const IdxVolume>> vols;
    std::vector<std::wstring> added;
    {
        std::lock_guard lk(g_ixm);
        vols = g_ixVols;
        added = g_ixAdded;
    }
    std::atomic<uint64_t> total{0};
    const unsigned nth = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nth; ++t)
        th.emplace_back([&, t] {
            uint64_t mine = 0;
            int sc;
            for (const auto& vp : vols)
                for (size_t i = 1 + t; i < vp->count; i += nth) {
                    if ((i & 0xFFFF) < nth && current != gen) return;
                    const IdxNode& nd = vp->node[i];
                    if (FuzzyMatch(vp->name + nd.nameOff, nd.nameLen, pat, sc, nullptr) && sc > kNearFloor / 2) ++mine;
                }
            total += mine;
        });
    for (auto& x : th) x.join();
    int sc;
    for (const std::wstring& p : added) {
        const std::wstring leaf = LeafOf(p);
        if (FuzzyMatch(leaf.c_str(), leaf.size(), pat, sc, nullptr) && sc > kNearFloor / 2) ++total;
    }
    return total;
}

// Tests: the index is only this folder (and the folders above it, on its drive), so what they find doesn't depend on the
// PC. Nothing is saved, and the real index doesn't come back while this Jolt runs.
void IdxTestOnly(const std::wstring& folder) {
    if (folder.size() < 3 || folder[1] != L':') return;
    auto v = std::make_shared<IdxVolume>();
    v->letter = static_cast<wchar_t>(towupper(folder[0]));
    wchar_t root[3] = {v->letter, L':', 0};
    IdxAddNode(*v, UINT32_MAX, root, 2, true);
    uint32_t at = 0;
    for (size_t i = 3; i < folder.size();) {
        size_t j = folder.find(L'\\', i);
        if (j == std::wstring::npos) j = folder.size();
        if (j > i) {
            IdxAddNode(*v, at, folder.c_str() + i, j - i, true);
            at = static_cast<uint32_t>(v->nodes.size() - 1);
        }
        i = j + 1;
    }
    IdxWalkFrom(*v, at, folder, false);
    v->Seal();
    std::lock_guard lk(g_ixm);
    g_ixTestOnly = true;
    g_ixVols.assign(1, v);
    g_ixAdded.clear();
    g_ixCount = v->count;
}

void IdxStart() { std::thread(IdxBuilder).detach(); }
void IdxStartReadOnly() {
    std::thread([] {
        for (wchar_t c : IdxLetters())
            if (auto v = IdxLoad(c)) IdxPublish(v);
    }).detach();
}

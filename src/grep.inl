// Content search: "content:word" (or content:"two words") in the filter keeps only the files whose text
// contains it, and shows a snippet with the match next to the name. Files are read on a thread, in the
// order the list asks for them, and results come back in batches. Included from app.cpp after search.inl.

constexpr UINT WM_APP_GREP = WM_APP + 10;
constexpr size_t kGrepMaxBytes = 32u << 20;  // bigger files are skipped

struct GrepJob {
    std::wstring path, term;  // term in lowercase
    uint64_t gen;
};
struct GrepResult {
    uint64_t gen;
    std::vector<std::pair<std::wstring, GrepHit>> hits;  // key -> hit
};

std::atomic<uint64_t> g_grepGen{0};
std::mutex g_grepMu;
std::condition_variable g_grepCv;
std::deque<GrepJob> g_grepQueue;
bool g_grepThreadUp = false;
std::atomic<int> g_grepPending{0};  // files queued but not reported yet

// Case-insensitive find; returns npos if absent.
size_t GrepFind(const std::wstring& text, const std::wstring& term) {
    if (term.empty() || text.size() < term.size()) return std::wstring::npos;
    const wchar_t first = term[0];
    const size_t last = text.size() - term.size();
    for (size_t i = 0; i <= last; ++i) {
        if (static_cast<wchar_t>(towlower(text[i])) != first) continue;
        size_t k = 1;
        while (k < term.size() && static_cast<wchar_t>(towlower(text[i + k])) == term[k]) ++k;
        if (k == term.size()) return i;
    }
    return std::wstring::npos;
}

// Reads a file as text: UTF-16 with a BOM, UTF-8 (with or without), or the system code page.
// Empty (with ok = false) for binaries and files that are too big.
std::wstring GrepRead(const std::wstring& path, bool& ok) {
    ok = false;
    if (OnlineOnlyPath(path)) return L"";  // reading it would download it
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > kGrepMaxBytes) {
        CloseHandle(h);
        return L"";
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    const bool read = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) != 0;
    CloseHandle(h);
    if (!read) return L"";
    bytes.resize(got);
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    std::wstring text;
    if (bytes.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        const bool be = b[0] == 0xFE;
        text.resize((bytes.size() - 2) / 2);
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char lo = b[2 + 2 * i], hi = b[3 + 2 * i];
            text[i] = static_cast<wchar_t>(be ? (lo << 8) | hi : (hi << 8) | lo);
        }
        ok = true;
        return text;
    }
    size_t start = bytes.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;
    // a NUL in the first bytes: not text
    if (memchr(bytes.data() + start, 0, std::min<size_t>(bytes.size() - start, 4096))) return L"";
    const char* s = bytes.data() + start;
    const int n = static_cast<int>(bytes.size() - start);
    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, n, nullptr, 0);
    const UINT cp = w > 0 ? CP_UTF8 : CP_ACP;
    if (w <= 0) w = MultiByteToWideChar(cp, 0, s, n, nullptr, 0);
    if (w <= 0) return L"";
    text.resize(w);
    MultiByteToWideChar(cp, 0, s, n, text.data(), w);
    ok = true;
    return text;
}

// A line of context around the match, with line breaks and runs of spaces flattened.
GrepHit GrepSnippet(const std::wstring& text, size_t at, size_t len) {
    GrepHit hit;
    const size_t from = at > 40 ? at - 40 : 0, to = std::min(text.size(), at + len + 70);
    std::wstring s;
    bool space = false;
    for (size_t i = from; i < to; ++i) {
        wchar_t c = text[i];
        if (i == at) hit.at = static_cast<int>(s.size());
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') {
            if (!space && !s.empty()) s += L' ';
            space = true;
            if (i == at) hit.at = static_cast<int>(s.size() - 1);
            continue;
        }
        space = false;
        s += c;
    }
    hit.match = true;
    hit.len = static_cast<int>(len);
    hit.snippet = (from > 0 ? L"…" : L"") + s + (to < text.size() ? L"…" : L"");
    if (from > 0) ++hit.at;
    return hit;
}

void GrepThread() {
    for (;;) {
        GrepJob job;
        {
            std::unique_lock lk(g_grepMu);
            g_grepCv.wait(lk, [] { return !g_grepQueue.empty(); });
            job = std::move(g_grepQueue.front());
            g_grepQueue.pop_front();
        }
        auto res = std::make_unique<GrepResult>();
        res->gen = job.gen;
        const double start = NowMs();
        for (;;) {
            if (job.gen == g_grepGen) {
                GrepHit hit;
                bool ok;
                const std::wstring text = GrepRead(job.path, ok);
                if (ok) {
                    const size_t at = GrepFind(text, job.term);
                    if (at != std::wstring::npos) hit = GrepSnippet(text, at, job.term.size());
                }
                res->hits.emplace_back(Key(job.path), std::move(hit));
            }
            std::unique_lock lk(g_grepMu);
            // a batch carries one generation only: the main thread discards whole batches by it
            if (g_grepQueue.empty() || g_grepQueue.front().gen != res->gen || NowMs() - start > 60) break;
            job = std::move(g_grepQueue.front());
            g_grepQueue.pop_front();
        }
        GrepResult* r = res.release();
        if (!PostMessageW(g_hwnd, WM_APP_GREP, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }
}

// A new term: forget everything and start over.
void GrepStart(const std::wstring& term) {
    g_grepTerm = term;
    ++g_grepGen;
    g_grepHits.clear();
    g_grepSeen.clear();
    std::lock_guard lk(g_grepMu);
    g_grepQueue.clear();
    g_grepPending = 0;
}

void GrepStop() {
    if (g_grepTerm.empty()) return;
    GrepStart(L"");
}

// The list wants to know about this file: read it when its turn comes.
void GrepWant(const std::wstring& full) {
    g_grepSeen.insert(Key(full));
    ++g_grepPending;
    {
        std::lock_guard lk(g_grepMu);
        g_grepQueue.push_back({full, g_grepTerm, g_grepGen});
        if (!g_grepThreadUp) {
            g_grepThreadUp = true;
            std::thread(GrepThread).detach();
        }
    }
    g_grepCv.notify_one();
}

// Results back on the main thread: the lists that are waiting for them get rebuilt in place.
void ApplyGrepResult(GrepResult* r) {
    std::unique_ptr<GrepResult> hold(r);
    if (r->gen != g_grepGen) return;
    for (auto& [key, hit] : r->hits) g_grepHits[key] = std::move(hit);
    g_grepPending -= static_cast<int>(r->hits.size());
    if (g_grepPending < 0) g_grepPending = 0;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (t.filter.empty() || ParseFilter(t.filter).content != g_grepTerm) continue;
        Place p = SavePlace(t);
        Rebuild(t, &p, L"");
    }
    Invalidate();
}

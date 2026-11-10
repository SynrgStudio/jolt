// fzf-style fuzzy search: the pattern has to appear as a subsequence (case-insensitive);
// the score rewards contiguous matches, word starts and the start of the text, and penalizes
// gaps. Also returns the positions so they can be highlighted.

// Lowercase with accents stripped: "Café" matches "cafe", "Résumé" matches "resume".
inline wchar_t FzLower(wchar_t c) {
    if (c < 128) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    if (c >= 0xC0 && c <= 0xFF) {
        static const char fold[] = "aaaaaaaceeeeiiiidnooooo*ouuuuyts" "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
        char f = fold[c - 0xC0];
        if (f != '*' && f != '/' && f != 's' && f != 't' && f != 'd') return static_cast<wchar_t>(f);
    }
    return towlower(c);
}

inline bool FzBoundary(const wchar_t* text, size_t i) {
    if (i == 0) return true;
    wchar_t p = text[i - 1], c = text[i];
    if (p == L' ' || p == L'-' || p == L'_' || p == L'.' || p == L'/' || p == L'\\' || p == L'(' || p == L'[') return true;
    if (iswlower(p) && iswupper(c)) return true;  // camelCase
    if (!iswdigit(p) && iswdigit(c)) return true;
    return false;
}

constexpr int kNearFloor = -1000000;     // names only close to what was typed score around here, under every good match
constexpr int kScattered = 2 * kNearFloor;  // and letters scattered along a name score under those

// pat is already lowercase. Returns false if there's no match.
bool FuzzyMatch(const wchar_t* text, size_t n, const std::wstring& pat, int& score, std::vector<int>* pos) {
    const size_t m = pat.size();
    if (m == 0) {
        score = 0;
        if (pos) pos->clear();
        return true;
    }
    if (m > n) return false;
    // 1) is it a subsequence? (forward scan, records where it ends)
    size_t pi = 0, end = 0;
    for (size_t i = 0; i < n && pi < m; ++i)
        if (FzLower(text[i]) == FzLower(pat[pi])) {
            ++pi;
            end = i;
        }
    if (pi < m) return false;
    // 2) backward scan from the end to shrink the window
    size_t start = end;
    for (size_t i = end + 1, k = m; i > 0 && k > 0;) {
        --i;
        if (FzLower(text[i]) == FzLower(pat[k - 1])) {
            --k;
            start = i;
        }
    }
    // 3) within the window, pick the best nearby position for each letter (a word boundary if there is one)
    std::vector<int> p;
    p.reserve(m);
    size_t i = start;
    for (size_t k = 0; k < m; ++k) {
        size_t best = n;
        for (size_t j = i; j <= end; ++j) {
            if (FzLower(text[j]) != FzLower(pat[k])) continue;
            if (best == n) best = j;
            if (FzBoundary(text, j) || (k > 0 && j == i)) {
                best = j;
                break;
            }
            if (j > best + 2) break;  // don't stray too far looking for a word boundary
        }
        if (best == n) return false;  // shouldn't happen
        p.push_back(static_cast<int>(best));
        i = best + 1;
    }
    // 4) score
    int s = 0;
    for (size_t k = 0; k < m; ++k) {
        int at = p[k];
        s += 16;
        if (k > 0 && at == p[k - 1] + 1) s += 12;        // contiguous
        if (FzBoundary(text, at)) s += 10;               // word start
        if (k == 0 && at == 0) s += 20;                  // text start
        if (k > 0) s -= std::min(8, at - p[k - 1] - 1);  // gap
    }
    // exact substring match: big bonus
    if (p.back() - p.front() + 1 == static_cast<int>(m)) s += 30;
    s -= static_cast<int>(std::min<size_t>(n, 64) / 8);  // shorter texts first, just barely
    // A word this long isn't an abbreviation: spread over more than one and a half times its length, it matched by
    // chance. With millions of names there are always plenty of those, and they would bury the name that was meant.
    if (m >= 6 && (p.back() - p.front() + 1) * 2 > static_cast<int>(m) * 3) s += kScattered;
    score = s;
    if (pos) *pos = std::move(p);
    return true;
}

// Among many matches, what was probably meant: a name that is exactly what was typed, and programs. Points on top of
// FuzzyMatch's score. A program only counts with the word whole in its name, not with its letters here and there.
constexpr int kExactProgram = 6000, kExactName = 4000, kProgramName = 2000;

int NameRank(const wchar_t* name, size_t n, const std::wstring& pat) {
    const size_t m = pat.size();
    if (m == 0 || m > n) return 0;
    size_t stem = n;
    for (size_t i = n; i > 0; --i)
        if (name[i - 1] == L'.') {
            stem = i - 1;
            break;
        }
    bool program = false;
    for (const wchar_t* ext : {L".exe", L".lnk", L".bat", L".appref-ms"})  // not .cmd: those are the shims in node_modules
        program |= n - stem == wcslen(ext) && _wcsnicmp(name + stem, ext, n - stem) == 0;
    auto sameAt = [&](size_t at) {
        for (size_t k = 0; k < m; ++k)
            if (FzLower(name[at + k]) != FzLower(pat[k])) return false;
        return true;
    };
    if ((stem == m || n == m) && sameAt(0)) return program ? kExactProgram : kExactName;
    if (program)
        for (size_t at = 0; at + m <= n; ++at)
            if (sameAt(at)) return kProgramName;
    return 0;
}

// ---- typos: names close to what was typed, for when FuzzyMatch finds nothing in them.
// The pattern is compared with the best stretch of the name, its letters in order. A wrong, extra or missing letter is
// a mistake; two neighbours swapped are not, all the letters are right. The result is the percentage of letters right,
// so the minimum also says how many mistakes a word has room for: at 75%, none in 3 letters, one in 4, two in 8.
// Counting letters without their order was tried and is useless: thousands of names have them all somewhere.
constexpr int kNearText = 260;  // names longer than this are compared up to here

struct NearPattern {
    wchar_t p[64]{};
    int m = 0, mistakes = -1;  // mistakes < 0: off
    NearPattern(const std::wstring& pat, int minPercent) {
        // a path being typed isn't a name; two letters are too few to tell what was meant
        if (minPercent <= 0 || pat.size() < 3 || pat.size() > 64 || pat.find_first_of(L"\\/:") != std::wstring::npos) return;
        m = static_cast<int>(pat.size());
        for (int i = 0; i < m; ++i) p[i] = FzLower(pat[i]);
        mistakes = m * (100 - minPercent) / 100;
    }
};

// Percentage of the pattern that is in the text, or 0 if that's under the minimum.
int NearMatch(const wchar_t* text, size_t len, const NearPattern& np) {
    const int m = np.m, n = static_cast<int>(std::min<size_t>(len, kNearText));
    if (np.mistakes < 0 || n + np.mistakes < m) return 0;
    // most names go here: each letter typed that the name doesn't have, counting repeats, is a mistake for sure
    wchar_t t[kNearText];
    uint16_t have[64] = {};
    for (int j = 0; j < n; ++j) {
        t[j] = FzLower(text[j]);
        ++have[t[j] & 63];
    }
    const int k = np.mistakes;
    int absent = 0;
    for (int i = 0; i < m; ++i) {
        uint16_t& h = have[np.p[i] & 63];
        if (h) --h;
        else if (++absent > k) return 0;
    }
    // Edit distance against any stretch of the text: a row per letter of the text, and it can start anywhere. Of each
    // row only the start can stay within the mistakes allowed, so it goes as far as the last two rows did, and a little.
    int rows[3][67];
    int *before = rows[0], *prev = rows[1], *cur = rows[2];
    for (int i = 0; i <= m + 2; ++i) {
        before[i] = cur[i] = k + 1;
        prev[i] = std::min(i, k + 1);
    }
    int reach = std::min(m, k), reachBefore = 0, best = k + 1;
    for (int j = 0; j < n && best > 0; ++j) {
        const int limit = std::max(reach, reachBefore) + 2;
        cur[0] = 0;
        int i = 1;
        for (; i <= m && (i <= limit || cur[i - 1] <= k); ++i) {
            int v = std::min(prev[i - 1] + (np.p[i - 1] != t[j]), std::min(prev[i], cur[i - 1]) + 1);
            if (i > 1 && j > 0 && np.p[i - 1] == t[j - 1] && np.p[i - 2] == t[j]) v = std::min(v, before[i - 2]);  // swapped: free
            cur[i] = v;
        }
        cur[i] = cur[i + 1] = k + 1;  // what comes after is out of reach
        reachBefore = reach;
        for (reach = i - 1; reach > 0 && cur[reach] > k; --reach) {}
        if (reach == m) best = std::min(best, cur[m]);
        int* spare = before;
        before = prev;
        prev = cur;
        cur = spare;
    }
    return best > k ? 0 : (m - best) * 100 / m;
}

inline int NearScore(int percent, size_t len) { return kNearFloor + percent * 1000 - static_cast<int>(std::min<size_t>(len, 255)); }

// The letters of the text that were typed right, to highlight them. Only for what's on screen: it keeps the whole table.
bool NearPositions(const wchar_t* text, size_t len, const std::wstring& pat, int minPercent, std::vector<int>& pos) {
    const NearPattern np(pat, minPercent);
    const int m = np.m, n = static_cast<int>(std::min<size_t>(len, kNearText));
    if (np.mistakes < 0 || n == 0) return false;
    std::vector<wchar_t> t(n);
    for (int j = 0; j < n; ++j) t[j] = FzLower(text[j]);
    const int w = m + 1;
    std::vector<int> d(static_cast<size_t>(n + 1) * w);
    for (int i = 0; i <= m; ++i) d[i] = i;
    int end = 0;
    for (int j = 1; j <= n; ++j) {
        d[static_cast<size_t>(j) * w] = 0;
        for (int i = 1; i <= m; ++i) {
            const size_t at = static_cast<size_t>(j) * w + i;
            int v = std::min(d[at - w - 1] + (np.p[i - 1] != t[j - 1]), std::min(d[at - w], d[at - 1]) + 1);
            if (i > 1 && j > 1 && np.p[i - 1] == t[j - 2] && np.p[i - 2] == t[j - 1]) v = std::min(v, d[at - 2 * w - 2]);
            d[at] = v;
        }
        if (d[static_cast<size_t>(j) * w + m] < d[static_cast<size_t>(end) * w + m]) end = j;
    }
    if (d[static_cast<size_t>(end) * w + m] > np.mistakes) return false;
    pos.clear();
    for (int i = m, j = end; i > 0 && j > 0;) {  // back along the way that gave the result
        const size_t at = static_cast<size_t>(j) * w + i;
        if (i > 1 && j > 1 && np.p[i - 1] == t[j - 2] && np.p[i - 2] == t[j - 1] && d[at] == d[at - 2 * w - 2]) {
            pos.push_back(j - 1);
            pos.push_back(j - 2);
            i -= 2;
            j -= 2;
        } else if (d[at] == d[at - w - 1] + (np.p[i - 1] != t[j - 1])) {
            if (np.p[i - 1] == t[j - 1]) pos.push_back(j - 1);
            --i;
            --j;
        } else if (d[at] == d[at - 1] + 1) {
            --i;
        } else {
            --j;
        }
    }
    std::sort(pos.begin(), pos.end());
    return !pos.empty();
}

// ---- Ctrl+P: what is near the folder being looked at ranks higher, the more so the fewer folders between them.
// common: how many folders below the drive both share (0: only the drive); up: how many levels above the folder being
// looked at they meet; down: how far below that meeting point the result is. Meeting more than two levels up isn't near.
inline int NearFolderPoints(int common, int up, int down) {
    if (common < 1 || up < 0 || down < 0 || up > 2) return 0;
    return 96 / (2 + 2 * up + down);  // 48 for the folder itself, 32 for what's in it, 16 for what's in a sibling folder
}

// The same, from the two paths ("C:\a\b"), compared folder by folder.
int NearFolderPoints(const std::wstring& path, const std::wstring& here) {
    if (path.size() < 3 || here.size() < 3 || path[1] != L':' || here[1] != L':') return 0;  // folders on drives only
    const size_t n = std::min(path.size(), here.size());
    size_t i = 0, shared = 0;  // where the part both have ends, at a folder's edge
    for (; i < n && towupper(path[i]) == towupper(here[i]); ++i)
        if (path[i] == L'\\') shared = i;
    if (i == n && (path.size() == n || path[n] == L'\\') && (here.size() == n || here[n] == L'\\')) shared = n;
    auto depth = [](const std::wstring& s, size_t end) {  // folders below the drive, up to there
        int d = 0;
        for (size_t k = 0; k < end; ++k) d += s[k] == L'\\' && k + 1 < s.size();
        return d;
    };
    const int common = depth(path, shared);
    return NearFolderPoints(common, depth(here, here.size()) - common, depth(path, path.size()) - common);
}

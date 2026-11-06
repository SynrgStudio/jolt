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
    score = s;
    if (pos) *pos = std::move(p);
    return true;
}

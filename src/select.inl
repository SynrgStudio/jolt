// Selecting by rule: invert the selection, take everything with the highlighted item's extension, or type a
// pattern in the palette with the filter's own terms (*.png, >10mb, ext:pdf, type:folder, newer:7d, !tmp, tag:x).
// While the pattern is typed the list already shows what it takes; Esc puts the selection back.
// Included from app.cpp after palette.inl.

// With a filter, the close names below the real matches stay out, as with Ctrl+A.
bool OnlyRealRows(const Tab& t) { return t.nearRows > 0 && t.nearRows < RowCount(t); }
bool Selectable(const Tab& t, int i, bool onlyReal) { return !IsHeader(t, i) && !(onlyReal && t.rows[i].closeness); }

void InvertSelection(Tab& t) {
    const bool onlyReal = OnlyRealRows(t);
    const int n = std::min(RowCount(t), static_cast<int>(t.selMask.size()));
    int on = 0;
    for (int i = 0; i < n; ++i) {
        t.selMask[i] = Selectable(t, i, onlyReal) && !t.selMask[i];
        on += t.selMask[i];
    }
    S.message = on ? Thousands(on) + L" selected" : L"Nothing selected";
}

// Every file with the extension of the highlighted one; on a folder, every folder.
void SelectSameExtension(Tab& t) {
    if (t.sel < 0 || t.sel >= RowCount(t) || IsHeader(t, t.sel)) {
        S.message = L"Highlight a file first";
        return;
    }
    const bool dirs = EntryAt(t, t.sel).isDir();
    const std::wstring ext = dirs ? L"" : ExtOf(RowNameP(t, t.sel), EntryAt(t, t.sel).nameLen);
    const bool onlyReal = OnlyRealRows(t);
    const int n = std::min(RowCount(t), static_cast<int>(t.selMask.size()));
    int on = 0;
    for (int i = 0; i < n; ++i) {
        bool take = false;
        if (Selectable(t, i, onlyReal)) {
            const Entry& e = EntryAt(t, i);
            take = dirs ? e.isDir() : !e.isDir() && ExtOf(RowNameP(t, i), e.nameLen) == ext;
        }
        t.selMask[i] = take;
        on += take;
    }
    const bool one = on == 1;
    S.message = Thousands(on) + (dirs ? (one ? L" folder selected" : L" folders selected")
                                 : ext.empty() ? (one ? L" file without an extension selected" : L" files without an extension selected")
                                               : L" " + ext + (one ? L" file selected" : L" files selected"));
}

// ---- by pattern (the palette in mode 3)
struct SelPattern {
    Tab* tab = nullptr;
    uint64_t gen = 0;                 // the rowsGen that before and match belong to
    std::vector<std::wstring> keys;   // the selection when the palette opened, by path, in case the list changes meanwhile
    std::vector<uint8_t> before;      // the same, by row
    std::vector<uint8_t> match;       // the rows the pattern takes
    std::vector<int> actions;         // per palette item: 0 select, 1 add, 2 take out, -1 nothing
    bool content = false;             // content: was typed, which only the filter can do
} g_selPat;

void SelPatternOpen() {
    Tab& t = T();
    if (RowCount(t) == 0) {
        S.message = L"Nothing to select here";
        Invalidate();
        return;
    }
    g_selPat.tab = &t;
    g_selPat.gen = t.rowsGen;
    g_selPat.before = t.selMask;
    g_selPat.keys.clear();
    for (int i = 0; i < std::min(RowCount(t), static_cast<int>(t.selMask.size())); ++i)
        if (t.selMask[i]) g_selPat.keys.push_back(RowKey(t, i));
    g_selPat.match.assign(t.selMask.size(), 0);
    S.message.clear();  // the status bar counts the selection as it changes
    PaletteOpen(3);
}

// What the pattern takes: the filter's terms, and the plain words must all be in the name.
void SelPatternCompute() {
    Tab& t = *g_selPat.tab;
    const int n = RowCount(t);
    if (g_selPat.gen != t.rowsGen || static_cast<int>(g_selPat.before.size()) != n) {  // the list changed: find the selection again
        const std::unordered_set<std::wstring> ks(g_selPat.keys.begin(), g_selPat.keys.end());
        g_selPat.before.assign(n, 0);
        for (int i = 0; i < n; ++i) g_selPat.before[i] = !IsHeader(t, i) && ks.count(RowKey(t, i));
        g_selPat.gen = t.rowsGen;
    }
    g_selPat.match.assign(n, 0);
    const std::wstring text = Trim(g_pal.text);
    if (text.empty()) return;
    const FilterSpec s = ParseFilter(text);
    g_selPat.content = !s.content.empty();
    if (g_selPat.content) return;
    std::vector<std::wstring> words;
    for (size_t p = 0; p < s.fuzzy.size();) {
        size_t sp = s.fuzzy.find(L' ', p);
        if (sp == std::wstring::npos) sp = s.fuzzy.size();
        if (sp > p) words.push_back(s.fuzzy.substr(p, sp - p));
        p = sp + 1;
    }
    for (int i = 0; i < n; ++i) {
        if (IsHeader(t, i)) continue;
        const Entry& e = EntryAt(t, i);
        const wchar_t* name = RowNameP(t, i);
        if (!FilterTermsPass(s, e, name, e.nameLen)) continue;
        if (!words.empty()) {
            const std::wstring low = Lower(std::wstring(name, e.nameLen));
            bool all = true;
            for (const std::wstring& w : words) all &= low.find(w) != std::wstring::npos;
            if (!all) continue;
        }
        if (!s.tags.empty() || !s.notTags.empty()) {
            if (RowDir(t, i).empty()) continue;
            const std::wstring full = Join(RowDir(t, i), name);
            bool ok = true;
            for (const std::wstring& tg : s.tags) ok &= HasTag(full, tg);
            for (const std::wstring& tg : s.notTags) ok &= !HasTag(full, tg);
            if (!ok) continue;
        }
        g_selPat.match[i] = 1;
    }
}

// Puts the selection the action gives on the list (-1: the one from before).
void SelPatternApply(int action, bool commit) {
    if (!g_selPat.tab || g_selPat.tab != &T()) return;
    Tab& t = T();
    if (g_selPat.gen != t.rowsGen || static_cast<int>(g_selPat.match.size()) != RowCount(t)) SelPatternCompute();
    const int n = std::min(RowCount(t), static_cast<int>(t.selMask.size()));
    int first = -1, on = 0;
    for (int i = 0; i < n; ++i) {
        const bool b = g_selPat.before[i], m = g_selPat.match[i];
        t.selMask[i] = action == 0 ? m : action == 1 ? b || m : action == 2 ? b && !m : b;
        on += t.selMask[i];
        if (t.selMask[i] && first < 0) first = i;
    }
    if (commit) {
        if (first >= 0 && !IsSel(t, t.sel)) {  // the cursor goes to the first one taken, so Del and the rest act on what is seen
            t.sel = t.anchor = first;
            EnsureVisible(t);
        }
        S.message = on ? Thousands(on) + L" selected" : L"Nothing selected";
        g_selPat.tab = nullptr;
    }
    Invalidate();
}

// The list shows what the highlighted palette row would do.
void SelPatternPreview() {
    const int item = g_pal.sel >= 0 && g_pal.sel < static_cast<int>(g_pal.hits.size()) ? g_pal.hits[g_pal.sel].first : -1;
    SelPatternApply(item >= 0 && item < static_cast<int>(g_selPat.actions.size()) ? g_selPat.actions[item] : -1, false);
}

// Esc, a click outside, or another palette: the selection is the one from before.
void SelPatternCancel() {
    if (!g_selPat.tab) return;
    SelPatternApply(-1, false);
    g_selPat.tab = nullptr;
}

void SelPatternBuild() {
    g_pal.all.clear();
    g_selPat.actions.clear();
    if (!g_selPat.tab || g_selPat.tab != &T()) return;
    SelPatternCompute();
    auto add = [&](std::wstring title, std::wstring detail, int action) {
        PalItem it;
        it.title = std::move(title);
        it.detail = std::move(detail);
        it.pinned = true;  // always listed, in this order: the text is a pattern, not a search among the rows
        if (action >= 0) it.run = [action] { SelPatternApply(action, true); };
        g_pal.all.push_back(std::move(it));
        g_selPat.actions.push_back(action);
    };
    if (Trim(g_pal.text).empty()) {
        add(L"*.png   report*   >10mb   ext:pdf,jpg   type:folder   newer:7d   !tmp   tag:done", L"plain words: names that contain them", -1);
        return;
    }
    if (g_selPat.content) {
        add(L"content: works in the filter: type it in the list, then " + ShortcutFor(Cmd::SelectAll), L"", -1);
        return;
    }
    int before = 0, match = 0, both = 0;
    for (size_t i = 0; i < g_selPat.match.size(); ++i) {
        before += g_selPat.before[i];
        match += g_selPat.match[i];
        both += g_selPat.before[i] && g_selPat.match[i];
    }
    if (!match) {
        add(L"Nothing here matches", L"", -1);
        return;
    }
    add(match == 1 ? L"Select the one that matches" : L"Select the " + Thousands(match) + L" that match", L"replaces the selection", 0);
    if (before > both && match > both)  // adding differs from replacing, and adds something
        add(L"Add them to the selection", Thousands(before) + L" selected now, " + Thousands(before + match - both) + L" after", 1);
    if (both)
        add(L"Take them out of the selection", Thousands(before) + L" selected now, " + Thousands(before - both) + L" after", 2);
}

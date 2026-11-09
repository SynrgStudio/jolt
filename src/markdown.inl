// Markdown, rendered for the preview: headings, paragraphs with bold, italic, code, links and strikethrough, lists
// (nested, numbered, tasks), quotes (and GitHub's [!NOTE] alerts), code blocks (highlighted when the language is
// known), rules and tables. HTML tags are left out, except that <br> breaks the line and an <img> leaves its alt
// text. Parsed on the preview's thread into blocks; laid out with DirectWrite on the main thread for the panel's
// width. Included from app.cpp after syntax.inl, before preview.inl.

// Brushes for colored runs inside text layouts (the preview's Markdown and hex), for this render target. Released
// with it (PreviewReleaseBitmap).
std::unordered_map<UINT32, ComPtr<ID2D1SolidColorBrush>> g_runBrushes;
ID2D1SolidColorBrush* RunBrush(UINT32 rgb) {
    auto& b = g_runBrushes[rgb];
    if (!b) g_rt->CreateSolidColorBrush(D2D1::ColorF(rgb), &b);
    return b.Get();
}

enum MdStyle : uint8_t { MdBold = 1, MdItalic = 2, MdCode = 4, MdLink = 8, MdStrike = 16, MdImage = 32 };
struct MdSpan {
    uint32_t start, len;
    uint8_t style;
};
enum class MdKind : uint8_t { Para, Heading, Item, Quote, Code, Rule, Table };
struct MdBlock {
    MdKind kind = MdKind::Para;
    int level = 0;        // headings: 1 to 6; list items: how deep
    std::wstring marker;  // list items: "•", "3.", "☐", "☑"
    std::wstring text;    // without the markup
    std::vector<MdSpan> spans;
    std::wstring lang;                             // code blocks
    std::vector<std::vector<std::wstring>> cells;  // tables: the first row is the header
    std::vector<char> right;                       // tables: columns aligned right (---:)
    // laid out, on the main thread
    ComPtr<IDWriteTextLayout> lay;
    float h = 0;  // height, the space after it included
    std::vector<float> colW;
};
struct MdDoc {
    std::vector<MdBlock> blocks;
    float width = -1;  // what it was laid out for
    const void* fonts = nullptr;
    float height = 0;
};

constexpr UINT32 kMdText = 0xD4D4DA, kMdLink = 0x6FA4FF, kMdCode = 0xE6E6EA;

// --- parsing (any thread)

// The inline markup of s into b.text and b.spans.
void MdInline(const std::wstring& s, MdBlock& b, int depth = 0) {
    std::wstring& out = b.text;
    struct Open {
        uint8_t style;
        uint32_t at;
        wchar_t ch;
        size_t n;
    };
    std::vector<Open> open;
    const size_t npos = std::wstring::npos;
    size_t i = 0;
    while (i < s.size()) {
        const wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size() && wcschr(L"\\`*_{}[]()#+-.!~|<>\"'", s[i + 1])) {
            out += s[i + 1];
            i += 2;
            continue;
        }
        if (c == L'`') {  // code: up to the same number of backticks
            size_t n = 0;
            while (i + n < s.size() && s[i + n] == L'`') ++n;
            const size_t end = s.find(std::wstring(n, L'`'), i + n);
            if (end == npos) {
                out.append(n, L'`');
                i += n;
                continue;
            }
            std::wstring code = s.substr(i + n, end - i - n);
            if (code.size() >= 2 && code.front() == L' ' && code.back() == L' ') code = code.substr(1, code.size() - 2);
            b.spans.push_back({static_cast<uint32_t>(out.size()), static_cast<uint32_t>(code.size()), MdCode});
            out += code;
            i = end + n;
            continue;
        }
        if ((c == L'[' || (c == L'!' && i + 1 < s.size() && s[i + 1] == L'[')) && depth < 3) {  // [text](url), ![alt](src)
            const bool img = c == L'!';
            const size_t from = i + (img ? 1 : 0);
            size_t close = npos;
            int level = 0;
            for (size_t j = from; j < s.size(); ++j) {
                if (s[j] == L'\\') {
                    ++j;
                } else if (s[j] == L'[') {
                    ++level;
                } else if (s[j] == L']' && --level == 0) {
                    close = j;
                    break;
                }
            }
            if (close != npos && close + 1 < s.size() && (s[close + 1] == L'(' || s[close + 1] == L'[')) {
                const wchar_t o = s[close + 1], e = o == L'(' ? L')' : L']';
                size_t end = npos;
                int pl = 0;
                for (size_t j = close + 1; j < s.size(); ++j) {
                    if (s[j] == o) ++pl;
                    else if (s[j] == e && --pl == 0) {
                        end = j;
                        break;
                    }
                }
                if (end != npos) {
                    MdBlock inner;
                    MdInline(s.substr(from + 1, close - from - 1), inner, depth + 1);
                    const uint32_t at = static_cast<uint32_t>(out.size());
                    for (MdSpan sp : inner.spans) {
                        sp.start += at;
                        b.spans.push_back(sp);
                    }
                    out += inner.text;
                    b.spans.push_back({at, static_cast<uint32_t>(inner.text.size()), static_cast<uint8_t>(img ? MdImage : MdLink)});
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == L'<') {  // <https://...>, <br>, an <img>'s alt; other tags and comments out
            if (s.compare(i, 4, L"<!--") == 0) {
                const size_t e = s.find(L"-->", i);
                i = e == npos ? s.size() : e + 3;
                continue;
            }
            const size_t gt = s.find(L'>', i);
            if (gt != npos) {
                const std::wstring tag = s.substr(i + 1, gt - i - 1);
                if (tag.rfind(L"http://", 0) == 0 || tag.rfind(L"https://", 0) == 0 || tag.rfind(L"mailto:", 0) == 0) {
                    b.spans.push_back({static_cast<uint32_t>(out.size()), static_cast<uint32_t>(tag.size()), MdLink});
                    out += tag;
                    i = gt + 1;
                    continue;
                }
                if (!tag.empty() && (iswalpha(tag[0]) || (tag[0] == L'/' && tag.size() > 1 && iswalpha(tag[1])))) {
                    const size_t ns = tag[0] == L'/' ? 1 : 0;
                    size_t ne = ns;
                    while (ne < tag.size() && iswalnum(tag[ne])) ++ne;
                    const std::wstring name = Lower(tag.substr(ns, ne - ns));
                    if (name == L"br" || (ns && (name == L"p" || name == L"div" || name == L"tr" || name == L"h1" || name == L"h2" || name == L"h3"))) {
                        if (!out.empty() && out.back() != L'\n') out += L'\n';
                    } else if (name == L"img" && !ns) {
                        const size_t a = Lower(tag).find(L"alt=");
                        if (a != npos && a + 5 < tag.size()) {
                            const wchar_t q = tag[a + 4];
                            const size_t qe = tag.find(q, a + 5);
                            if ((q == L'"' || q == L'\'') && qe != npos && qe > a + 5) {
                                const std::wstring alt = tag.substr(a + 5, qe - a - 5);
                                b.spans.push_back({static_cast<uint32_t>(out.size()), static_cast<uint32_t>(alt.size()), MdImage});
                                out += alt;
                            }
                        }
                    }
                    i = gt + 1;
                    continue;
                }
            }
        }
        if (c == L'*' || c == L'_' || c == L'~') {  // emphasis: * _ italic, ** __ bold, *** both, ~~ struck through
            size_t n = 0;
            while (i + n < s.size() && s[i + n] == c && n < 3) ++n;
            const bool prevWord = i > 0 && iswalnum(s[i - 1]), nextWord = i + n < s.size() && iswalnum(s[i + n]);
            const bool nextSpace = i + n >= s.size() || iswspace(s[i + n]), prevSpace = i == 0 || iswspace(s[i - 1]);
            const uint8_t style = c == L'~' ? (n == 2 ? MdStrike : 0) : n >= 3 ? (MdBold | MdItalic) : n == 2 ? MdBold : MdItalic;
            if (style && !(c == L'_' && prevWord && nextWord)) {  // snake_case stays as it is
                auto it = std::find_if(open.rbegin(), open.rend(), [&](const Open& o) { return o.ch == c && o.n == n; });
                if (it != open.rend() && !prevSpace) {
                    const Open o = *it;
                    open.erase(std::next(it).base());
                    if (out.size() > o.at) b.spans.push_back({o.at, static_cast<uint32_t>(out.size() - o.at), o.style});
                    i += n;
                    continue;
                }
                if (!nextSpace && s.find(std::wstring(n, c), i + n) != npos) {
                    open.push_back({style, static_cast<uint32_t>(out.size()), c, n});
                    i += n;
                    continue;
                }
            }
            out.append(n, c);
            i += n;
            continue;
        }
        out += c;
        ++i;
    }
    if (depth) return;
    // no line breaks or spaces at the ends (left by tags)
    size_t lead = 0;
    while (lead < out.size() && (out[lead] == L'\n' || out[lead] == L' ')) ++lead;
    while (out.size() > lead && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    out.erase(0, lead);
    const uint32_t n = static_cast<uint32_t>(out.size());
    for (MdSpan& sp : b.spans) {
        const uint32_t a = sp.start >= lead ? sp.start - static_cast<uint32_t>(lead) : 0;
        const uint32_t e = std::min(n, sp.start + sp.len >= lead ? sp.start + sp.len - static_cast<uint32_t>(lead) : 0);
        sp.start = std::min(a, n);
        sp.len = e > sp.start ? e - sp.start : 0;
    }
}

// How long a list item's marker is ("-", "*", "+", "12.", "3)"), 0 when the line isn't one.
size_t MdItemMark(const std::wstring& l, bool& num) {
    num = false;
    if (l.size() >= 2 && (l[0] == L'-' || l[0] == L'*' || l[0] == L'+') && l[1] == L' ') return 1;
    size_t d = 0;
    while (d < l.size() && d < 9 && iswdigit(l[d])) ++d;
    if (d && d + 1 < l.size() && (l[d] == L'.' || l[d] == L')') && l[d + 1] == L' ') {
        num = true;
        return d + 1;
    }
    return 0;
}
// ---, ***, ___ (three or more, spaces allowed)
bool MdIsRule(const std::wstring& l) {
    wchar_t ch = 0;
    int n = 0;
    for (wchar_t c : l) {
        if (c == L' ') continue;
        if (c != L'-' && c != L'*' && c != L'_') return false;
        if (ch && c != ch) return false;
        ch = c;
        ++n;
    }
    return n >= 3;
}
// | --- | :---: | ---: |
bool MdIsTableSep(const std::wstring& l) {
    bool dash = false, bar = false;
    for (wchar_t c : l) {
        if (c == L'-') dash = true;
        else if (c == L'|') bar = true;
        else if (c != L':' && c != L' ' && c != L'\t') return false;
    }
    return dash && bar;
}
std::vector<std::wstring> MdCells(std::wstring l) {
    l = Trim(l);
    if (!l.empty() && l.front() == L'|') l.erase(0, 1);
    if (!l.empty() && l.back() == L'|' && (l.size() < 2 || l[l.size() - 2] != L'\\')) l.pop_back();
    std::vector<std::wstring> cells;
    std::wstring cur;
    for (size_t i = 0; i < l.size(); ++i) {
        if (l[i] == L'\\' && i + 1 < l.size() && l[i + 1] == L'|') {
            cur += L'|';
            ++i;
        } else if (l[i] == L'|') {
            cells.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += l[i];
        }
    }
    cells.push_back(Trim(cur));
    return cells;
}

bool MdStartsBlock(const std::wstring& raw) {
    size_t ind = 0;
    while (ind < raw.size() && raw[ind] == L' ') ++ind;
    const std::wstring l = raw.substr(ind);
    bool num;
    if (l.empty()) return true;
    if (l[0] == L'#' || l[0] == L'>' || l.rfind(L"```", 0) == 0 || l.rfind(L"~~~", 0) == 0) return true;
    return MdItemMark(l, num) != 0 || MdIsRule(l);
}

std::vector<MdBlock> MdParse(const std::wstring& src) {
    std::vector<std::wstring> lines;
    for (size_t p = 0; p <= src.size();) {
        const size_t nl = src.find(L'\n', p);
        lines.push_back(src.substr(p, nl == std::wstring::npos ? std::wstring::npos : nl - p));
        if (nl == std::wstring::npos) break;
        p = nl + 1;
    }
    for (auto& l : lines) {  // tabs as four spaces, no carriage returns
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        for (size_t t = l.find(L'\t'); t != std::wstring::npos; t = l.find(L'\t', t)) l.replace(t, 1, L"    ");
    }
    std::vector<MdBlock> out;
    std::wstring para;
    bool hardBreak = false;
    std::vector<size_t> listIndents;  // the indents of the open lists, for nesting
    auto flush = [&] {
        if (para.empty()) return;
        MdBlock b;
        MdInline(para, b);
        if (!Trim(b.text).empty()) out.push_back(std::move(b));
        para.clear();
    };
    auto indentOf = [](const std::wstring& l) {
        size_t n = 0;
        while (n < l.size() && l[n] == L' ') ++n;
        return n;
    };
    size_t i = 0;
    if (!lines.empty() && Trim(lines[0]) == L"---") {  // front matter: shown as it is, faint
        for (size_t j = 1; j < lines.size(); ++j)
            if (Trim(lines[j]) == L"---" || Trim(lines[j]) == L"...") {
                MdBlock b;
                b.kind = MdKind::Code;
                b.lang = L"yaml";
                for (size_t k = 1; k < j; ++k) b.text += (k > 1 ? L"\n" : L"") + lines[k];
                out.push_back(std::move(b));
                i = j + 1;
                break;
            }
    }
    for (; i < lines.size() && out.size() < 5000; ++i) {
        const std::wstring& raw = lines[i];
        const size_t ind = indentOf(raw);
        const std::wstring l = raw.substr(ind);
        if (l.empty()) {
            flush();
            continue;
        }
        bool num = false;
        const size_t mark = ind < 4 || !listIndents.empty() ? MdItemMark(l, num) : 0;
        if (!mark && ind < 4 && para.empty()) listIndents.clear();
        // fenced code
        if (ind < 4 && (l.rfind(L"```", 0) == 0 || l.rfind(L"~~~", 0) == 0)) {
            flush();
            const wchar_t f = l[0];
            size_t n = 0;
            while (n < l.size() && l[n] == f) ++n;
            MdBlock b;
            b.kind = MdKind::Code;
            b.lang = Lower(Trim(l.substr(n)));
            b.lang = b.lang.substr(0, b.lang.find_first_of(L" {,"));
            bool first = true;
            for (++i; i < lines.size(); ++i) {
                const std::wstring t = Trim(lines[i]);
                if (t.size() >= n && t.find_first_not_of(f) == std::wstring::npos) break;
                b.text += (first ? L"" : L"\n") + lines[i].substr(std::min(indentOf(lines[i]), ind));
                first = false;
            }
            out.push_back(std::move(b));
            continue;
        }
        // HTML comments over several lines
        if (l.rfind(L"<!--", 0) == 0 && l.find(L"-->") == std::wstring::npos) {
            flush();
            while (i + 1 < lines.size() && lines[i].find(L"-->") == std::wstring::npos) ++i;
            continue;
        }
        // # headings
        if (ind < 4 && l[0] == L'#') {
            size_t n = 0;
            while (n < l.size() && l[n] == L'#') ++n;
            if (n <= 6 && (n == l.size() || l[n] == L' ')) {
                flush();
                std::wstring t = Trim(l.substr(n));
                const size_t hashes = t.find_last_not_of(L'#');
                if (hashes != std::wstring::npos && hashes + 1 < t.size() && t[hashes] == L' ') t = Trim(t.substr(0, hashes));
                MdBlock b;
                b.kind = MdKind::Heading;
                b.level = static_cast<int>(n);
                MdInline(t, b);
                out.push_back(std::move(b));
                continue;
            }
        }
        // underlined headings (=== and --- under a paragraph), and rules
        if (ind < 4 && !para.empty() && !mark && l.find_first_not_of(L'=') == std::wstring::npos) {
            MdBlock b;
            b.kind = MdKind::Heading;
            b.level = 1;
            MdInline(para, b);
            para.clear();
            out.push_back(std::move(b));
            continue;
        }
        if (ind < 4 && MdIsRule(l)) {
            if (!para.empty() && l.find_first_not_of(L"- ") == std::wstring::npos && l.find(L' ') == std::wstring::npos) {
                MdBlock b;
                b.kind = MdKind::Heading;
                b.level = 2;
                MdInline(para, b);
                para.clear();
                out.push_back(std::move(b));
            } else {
                flush();
                MdBlock b;
                b.kind = MdKind::Rule;
                out.push_back(std::move(b));
            }
            continue;
        }
        // tables: a row, then | --- | --- |
        if (l.find(L'|') != std::wstring::npos && i + 1 < lines.size() && MdIsTableSep(lines[i + 1])) {
            flush();
            MdBlock b;
            b.kind = MdKind::Table;
            auto plain = [](const std::wstring& cell) {
                MdBlock t;
                MdInline(cell, t);
                return t.text;
            };
            std::vector<std::wstring> head = MdCells(l);
            for (auto& c : head) c = plain(c);
            const std::vector<std::wstring> sep = MdCells(lines[i + 1]);
            for (const std::wstring& sc : sep) b.right.push_back(sc.size() > 1 && sc.back() == L':' && sc.front() != L':');
            b.cells.push_back(std::move(head));
            for (i += 2; i < lines.size() && !Trim(lines[i]).empty() && lines[i].find(L'|') != std::wstring::npos && b.cells.size() < 1000; ++i) {
                std::vector<std::wstring> row = MdCells(lines[i]);
                for (auto& c : row) c = plain(c);
                b.cells.push_back(std::move(row));
            }
            --i;
            out.push_back(std::move(b));
            continue;
        }
        // quotes (nested ones flattened)
        if (ind < 4 && l[0] == L'>') {
            flush();
            std::wstring text;
            bool blank = false;
            for (; i < lines.size(); ++i) {
                std::wstring t = Trim(lines[i]);
                if (t.empty() || t[0] != L'>') break;
                while (!t.empty() && t[0] == L'>') t = Trim(t.substr(1));
                if (t.empty()) {
                    blank = true;
                    continue;
                }
                static const wchar_t* alerts[] = {L"NOTE", L"TIP", L"IMPORTANT", L"WARNING", L"CAUTION"};
                bool alert = false;
                for (const wchar_t* a : alerts)
                    if (text.empty() && t == L"[!" + std::wstring(a) + L"]") {  // GitHub alerts: [!NOTE] as a title
                        std::wstring name = Lower(a);
                        name[0] = towupper(name[0]);
                        text = L"**" + name + L"**";
                        blank = true;
                        alert = true;
                    }
                if (alert) continue;
                text += text.empty() ? L"" : blank ? L"\n" : L" ";
                text += t;
                blank = false;
            }
            --i;
            MdBlock b;
            b.kind = MdKind::Quote;
            MdInline(text, b);
            out.push_back(std::move(b));
            continue;
        }
        // list items, and the lines that carry on with them
        if (mark) {
            flush();
            while (!listIndents.empty() && listIndents.back() > ind) listIndents.pop_back();
            if (listIndents.empty() || listIndents.back() < ind) listIndents.push_back(ind);
            MdBlock b;
            b.kind = MdKind::Item;
            b.level = static_cast<int>(std::min<size_t>(listIndents.size() - 1, 5));
            std::wstring t = Trim(l.substr(mark + 1));
            if (num) b.marker = l.substr(0, mark);
            else b.marker = b.level == 0 ? L"•" : b.level == 1 ? L"◦" : L"▪";
            if (t.rfind(L"[ ] ", 0) == 0 || t == L"[ ]") {
                b.marker = L"☐";
                t = Trim(t.substr(3));
            } else if (t.rfind(L"[x] ", 0) == 0 || t.rfind(L"[X] ", 0) == 0) {
                b.marker = L"☑";
                t = Trim(t.substr(3));
            }
            while (i + 1 < lines.size() && !MdStartsBlock(lines[i + 1]) && lines[i + 1].find(L'|') == std::wstring::npos) {
                const bool brk = t.size() >= 2 && t.compare(t.size() - 2, 2, L"  ") == 0;
                t += (brk ? L"\n" : L" ") + Trim(lines[++i]);
            }
            MdInline(t, b);
            out.push_back(std::move(b));
            continue;
        }
        // a paragraph's line: two spaces or a backslash at the end break it there
        if (!para.empty()) para += hardBreak ? L"\n" : L" ";
        hardBreak = raw.size() >= 2 && raw.compare(raw.size() - 2, 2, L"  ") == 0;
        std::wstring t = Trim(l);
        if (!t.empty() && t.back() == L'\\') {
            t.pop_back();
            hardBreak = true;
        }
        para += t;
    }
    flush();
    return out;
}

// --- layout and drawing (main thread)

struct MdFonts {
    ComPtr<IDWriteTextFormat> body, code;
    std::wstring mono;
    float size = 13;
    const void* forFmt = nullptr;
} g_md;

void MdFontsReady() {
    if (g_md.forFmt == g_fmtName.Get()) return;
    wchar_t ui[128] = L"Segoe UI", mono[128] = L"Consolas";
    g_fmtName->GetFontFamilyName(ui, 128);
    g_fmtMono->GetFontFamilyName(mono, 128);
    g_md.mono = mono;
    g_md.size = g_fmtName->GetFontSize() + 1;
    g_md.body.Reset();
    g_md.code.Reset();
    g_dw->CreateTextFormat(ui, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, g_md.size, L"en-us", &g_md.body);
    g_dw->CreateTextFormat(mono, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f, L"en-us", &g_md.code);
    for (IDWriteTextFormat* f : {g_md.body.Get(), g_md.code.Get()}) {
        f->SetWordWrapping(f == g_md.code.Get() ? DWRITE_WORD_WRAPPING_CHARACTER : DWRITE_WORD_WRAPPING_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    g_md.forFmt = g_fmtName.Get();
}

const float kMdHeading[7] = {1, 1.75f, 1.42f, 1.2f, 1.05f, 1, 0.95f};  // by level, times the body size

// A code block's language as a file the highlighter knows.
const Lang* MdLang(const std::wstring& lang) {
    static const std::pair<const wchar_t*, const wchar_t*> alias[] = {
        {L"python", L"x.py"}, {L"javascript", L"x.js"}, {L"typescript", L"x.ts"}, {L"c++", L"x.cpp"}, {L"csharp", L"x.cs"},
        {L"c#", L"x.cs"}, {L"bash", L"x.sh"}, {L"shell", L"x.sh"}, {L"zsh", L"x.sh"}, {L"console", L"x.sh"}, {L"sh", L"x.sh"},
        {L"powershell", L"x.ps1"}, {L"pwsh", L"x.ps1"}, {L"rust", L"x.rs"}, {L"golang", L"x.go"}, {L"kotlin", L"x.kt"},
        {L"ruby", L"x.rb"}, {L"markdown", L"x.md"}, {L"dockerfile", L"Dockerfile"}, {L"makefile", L"Makefile"},
        {L"batch", L"x.bat"}, {L"jsonc", L"x.jsonc"}};
    if (lang.empty()) return nullptr;
    for (auto& [k, v] : alias)
        if (lang == k) return LangFor(v);
    return LangFor(L"x." + lang);
}

void MdLayout(MdDoc& d, float W) {
    MdFontsReady();
    d.width = W;
    d.fonts = g_fmtName.Get();
    d.height = 0;
    for (size_t bi = 0; bi < d.blocks.size(); ++bi) {
        MdBlock& b = d.blocks[bi];
        b.lay.Reset();
        const bool last = bi + 1 == d.blocks.size();
        const bool nextItem = !last && d.blocks[bi + 1].kind == MdKind::Item;
        float w = W;
        if (b.kind == MdKind::Item) w = W - 20 - b.level * 18.f;
        else if (b.kind == MdKind::Quote) w = W - 14;
        else if (b.kind == MdKind::Code) w = W - 20;
        if (b.kind == MdKind::Rule) {
            b.h = 18;
        } else if (b.kind == MdKind::Table) {
            const size_t nc = b.cells.empty() ? 0 : std::max_element(b.cells.begin(), b.cells.end(), [](auto& a, auto& c) { return a.size() < c.size(); })->size();
            b.colW.assign(nc, 40.f);
            for (size_t r = 0; r < b.cells.size(); ++r)
                for (size_t c = 0; c < b.cells[r].size(); ++c)
                    if (!b.cells[r][c].empty())
                        b.colW[c] = std::max(b.colW[c], std::min(240.f, MeasureW(b.cells[r][c], r ? g_fmtName.Get() : g_fmtNameBold.Get()) + 18));
            float tw = 0;
            for (float x : b.colW) tw += x;
            if (tw > W)
                for (float& x : b.colW) x = std::max(40.f, x * W / tw);
            b.h = b.cells.size() * 24.f + 12;
        } else {
            g_dw->CreateTextLayout(b.text.c_str(), static_cast<UINT32>(b.text.size()), b.kind == MdKind::Code ? g_md.code.Get() : g_md.body.Get(),
                                   std::max(20.f, w), 100000.f, &b.lay);
            if (!b.lay) {
                b.h = 0;
                continue;
            }
            const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(b.text.size())};
            if (b.kind == MdKind::Heading) {
                b.lay->SetFontSize(g_md.size * kMdHeading[std::clamp(b.level, 1, 6)], all);
                b.lay->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
                if (b.level == 6) b.lay->SetDrawingEffect(RunBrush(cDim), all);
            }
            if (b.kind == MdKind::Code) {
                if (const Lang* lang = MdLang(b.lang))
                    for (const Span& sp : Highlight(b.text, *lang))
                        b.lay->SetDrawingEffect(RunBrush(TokColor(sp.cls)), DWRITE_TEXT_RANGE{sp.start, sp.len});
            }
            for (const MdSpan& sp : b.spans) {
                const DWRITE_TEXT_RANGE r{sp.start, sp.len};
                if (sp.style & MdBold) b.lay->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
                if (sp.style & MdItalic) b.lay->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                if (sp.style & MdStrike) b.lay->SetStrikethrough(TRUE, r);
                if (sp.style & MdLink) b.lay->SetDrawingEffect(RunBrush(kMdLink), r);
                if (sp.style & MdImage) {
                    b.lay->SetDrawingEffect(RunBrush(cDim), r);
                    b.lay->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                }
                if (sp.style & MdCode) {
                    b.lay->SetFontFamilyName(g_md.mono.c_str(), r);
                    b.lay->SetFontSize(g_md.size - 1, r);
                    b.lay->SetDrawingEffect(RunBrush(kMdCode), r);
                }
            }
            DWRITE_TEXT_METRICS m{};
            b.lay->GetMetrics(&m);
            switch (b.kind) {
                case MdKind::Heading: b.h = (bi ? 12.f : 0.f) + m.height + (b.level <= 2 ? 11.f : 6.f); break;
                case MdKind::Item: b.h = m.height + (nextItem ? 3.f : 10.f); break;
                case MdKind::Quote: b.h = m.height + 8 + 12; break;
                case MdKind::Code: b.h = m.height + 16 + 12; break;
                default: b.h = m.height + 10; break;
            }
        }
        d.height += b.h;
    }
}

// Draws the document from y (its top, scroll applied) inside [L, R], skipping what is outside [top, bot].
void MdDraw(MdDoc& d, float L, float R, float y, float top, float bot) {
    if (d.width != R - L || d.fonts != g_fmtName.Get()) MdLayout(d, R - L);
    for (size_t bi = 0; bi < d.blocks.size(); ++bi) {
        MdBlock& b = d.blocks[bi];
        if (y > bot) break;
        if (y + b.h < top) {
            y += b.h;
            continue;
        }
        auto draw = [&](float x, float ty, UINT32 color) {
            // inline code on a faint chip, then the text
            for (const MdSpan& sp : b.spans) {
                if (!(sp.style & MdCode) || !sp.len) continue;
                UINT32 n = 0;
                b.lay->HitTestTextRange(sp.start, sp.len, 0, 0, nullptr, 0, &n);
                std::vector<DWRITE_HIT_TEST_METRICS> hm(n);
                if (n && SUCCEEDED(b.lay->HitTestTextRange(sp.start, sp.len, 0, 0, hm.data(), n, &n)))
                    for (const auto& h : hm) Round(D2D1::RectF(x + h.left - 3, ty + h.top, x + h.left + h.width + 3, ty + h.top + h.height), 3, 0xFFFFFF, 0.08f);
            }
            Color(color);
            g_rt->DrawTextLayout(D2D1::Point2F(x, ty), b.lay.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        switch (b.kind) {
            case MdKind::Rule: HLine(L, R, y + 8); break;
            case MdKind::Heading: {
                if (!b.lay) break;
                const float ty = y + (bi ? 12.f : 0.f);
                draw(L, ty, cText);
                if (b.level <= 2) {
                    DWRITE_TEXT_METRICS m{};
                    b.lay->GetMetrics(&m);
                    HLine(L, R, ty + m.height + 5);
                }
                break;
            }
            case MdKind::Item: {
                if (!b.lay) break;
                const float x = L + 20 + b.level * 18.f;
                UINT32 lines = 0;
                b.lay->GetLineMetrics(nullptr, 0, &lines);
                std::vector<DWRITE_LINE_METRICS> lm(std::max<UINT32>(1, lines));
                const float lh = lines && SUCCEEDED(b.lay->GetLineMetrics(lm.data(), lines, &lines)) ? lm[0].height : 18.f;
                const bool num = !b.marker.empty() && iswdigit(b.marker[0]);
                Color(num ? cDim : b.marker == L"☑" ? kMdLink : cDim);
                Text(b.marker, num ? g_fmtRight.Get() : g_fmtCenter.Get(), x - 20, y, x - (num ? 5.f : 4.f), y + lh);
                draw(x, y, kMdText);
                break;
            }
            case MdKind::Quote:
                if (!b.lay) break;
                Round(D2D1::RectF(L, y + 2, L + 3, y + b.h - 14), 1.5f, 0x3F3F48);
                draw(L + 14, y + 4, cDim);
                break;
            case MdKind::Code:
                if (!b.lay) break;
                Round(D2D1::RectF(L, y, R, y + b.h - 12), 6, 0xFFFFFF, 0.045f);
                draw(L + 10, y + 8, b.lang == L"yaml" && bi == 0 ? cDim : kMdText);
                break;
            case MdKind::Table: {
                float ry = y + 4;
                const float tw = std::min(R - L, [&] {
                    float s = 0;
                    for (float x : b.colW) s += x;
                    return s;
                }());
                for (size_t r = 0; r < b.cells.size(); ++r, ry += 24) {
                    if (r == 0) Fill(D2D1::RectF(L, ry, L + tw, ry + 24), 0xFFFFFF, 0.05f);
                    float x = L;
                    for (size_t c = 0; c < b.colW.size() && x < R; x += b.colW[c], ++c) {
                        if (c >= b.cells[r].size()) continue;
                        const bool right = c < b.right.size() && b.right[c];
                        Color(r == 0 ? cText : 0xC9C9D1);
                        Text(b.cells[r][c], r == 0 ? g_fmtNameBold.Get() : right ? g_fmtRight.Get() : g_fmtName.Get(), x + 8, ry,
                             std::min(R, x + b.colW[c] - 8), ry + 24);
                    }
                    HLine(L, L + tw, ry + 24);
                }
                float x = L;
                Fill(D2D1::RectF(L, y + 4, L + 1, ry), cBorder);
                for (float cw : b.colW) {
                    x += cw;
                    if (x <= R) Fill(D2D1::RectF(x - 1, y + 4, x, ry), cBorder);
                }
                HLine(L, L + tw, y + 4);
                break;
            }
            default:
                if (b.lay) draw(L, y, kMdText);
                break;
        }
        y += b.h;
    }
}

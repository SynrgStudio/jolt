// Syntax highlighting for the text preview: a small tokenizer that knows comments, strings, numbers,
// keywords and a few per-family things (tags, keys, headings, diff lines), driven by a table of
// languages picked by extension. It runs over the whole text once and leaves spans (offset, length,
// class) that the preview paints with one brush per class. Included from app.cpp before preview.inl.

enum class Tok : uint8_t { Plain, Comment, String, Number, Keyword, Meta, Tag, Attr, Heading, Added, Removed };
struct Span {
    uint32_t start, len;
    Tok cls;
};

// What each language needs. Keyword lists are space separated, matched as whole words.
struct Lang {
    const wchar_t* exts;         // ".c .h ..." (space separated, with the dot) or a bare file name
    const wchar_t* line1;        // line comment starters (up to two, space separated), "" = none
    const wchar_t* blockOpen;    // block comment, "" = none
    const wchar_t* blockClose;
    const wchar_t* quotes;       // string delimiters, e.g. "\"'`"
    bool triple;                 // python's """ and '''
    bool preproc;                // '#' at the start of a line is a directive (C family)
    bool caseless;               // keywords ignore case (SQL, batch)
    int family;                  // 0 code, 1 markup, 2 ini/yaml (keys and sections), 3 markdown, 4 diff, 5 json
    const wchar_t* keywords;
};

const wchar_t* kKwC =
    L"alignas alignof and asm auto bool break case catch char class concept const consteval constexpr constinit "
    L"continue decltype default delete do double dynamic_cast else enum explicit export extern false final float for "
    L"friend goto if inline int long mutable namespace new noexcept not nullptr operator or override private protected "
    L"public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct "
    L"switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void "
    L"volatile wchar_t while int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t";
const wchar_t* kKwJs =
    L"abstract arguments async await boolean break byte case catch char class const continue debugger default delete do "
    L"double else enum export extends false final finally float for from function get goto if implements import in "
    L"instanceof int interface let long native new null of package private protected public return set short static "
    L"super switch synchronized this throw throws transient true try typeof undefined var void volatile while with yield "
    L"type readonly keyof namespace declare as is any never unknown string number object symbol bigint";
const wchar_t* kKwCs =
    L"abstract as base bool break byte case catch char checked class const continue decimal default delegate do double "
    L"else enum event explicit extern false finally fixed float for foreach goto if implicit in int interface internal "
    L"is lock long namespace new null object operator out override params private protected public readonly ref return "
    L"sbyte sealed short sizeof stackalloc static string struct switch this throw true try typeof uint ulong unchecked "
    L"unsafe ushort using var virtual void volatile while async await record init nameof when with yield get set";
const wchar_t* kKwJava =
    L"abstract assert boolean break byte case catch char class const continue default do double else enum extends final "
    L"finally float for goto if implements import instanceof int interface long native new package private protected "
    L"public return short static strictfp super switch synchronized this throw throws transient try void volatile while "
    L"true false null var record sealed permits yield";
const wchar_t* kKwGo =
    L"break case chan const continue default defer else fallthrough for func go goto if import interface map package "
    L"range return select struct switch type var true false nil int int8 int16 int32 int64 uint uint8 uint16 uint32 "
    L"uint64 float32 float64 string bool byte rune error any make new len cap append";
const wchar_t* kKwRust =
    L"as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut "
    L"pub ref return self Self static struct super trait true type unsafe use where while i8 i16 i32 i64 i128 u8 u16 "
    L"u32 u64 u128 f32 f64 usize isize bool char str String Vec Option Some None Result Ok Err Box";
const wchar_t* kKwPy =
    L"False None True and as assert async await break class continue def del elif else except finally for from global "
    L"if import in is lambda nonlocal not or pass raise return try while with yield self print len range str int float "
    L"list dict set tuple bool bytes";
const wchar_t* kKwSh =
    L"if then else elif fi for while until do done case esac in function select time return exit export local readonly "
    L"declare unset source alias echo cd ls rm cp mv mkdir test set shift break continue true false";
const wchar_t* kKwPs =
    L"begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally for foreach "
    L"from function hidden if in inlinescript param process return static switch throw trap try until using var while "
    L"workflow true false null Write-Host Write-Output Get-ChildItem Get-Item Set-Item Get-Content Set-Content "
    L"Where-Object ForEach-Object Select-Object";
const wchar_t* kKwBat = L"echo set if else goto for in do call exit rem pause start cd dir copy move del md rd type not exist defined errorlevel setlocal endlocal enabledelayedexpansion";
const wchar_t* kKwLua = L"and break do else elseif end false for function goto if in local nil not or repeat return then true until while self";
const wchar_t* kKwRuby = L"alias and begin break case class def defined? do else elsif end ensure false for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield require attr_accessor puts";
const wchar_t* kKwSql =
    L"select from where and or not in is null as insert into values update set delete create table index view drop alter "
    L"add column primary key foreign references unique default join left right inner outer on group by order having "
    L"limit offset union all distinct exists between like case when then else end begin commit rollback transaction "
    L"int integer varchar text date datetime boolean true false with returning";
const wchar_t* kKwCss = L"import media supports font-face keyframes page charset namespace layer container important";
const wchar_t* kKwJson = L"true false null";
const wchar_t* kKwDocker = L"FROM RUN CMD LABEL EXPOSE ENV ADD COPY ENTRYPOINT VOLUME USER WORKDIR ARG ONBUILD STOPSIGNAL HEALTHCHECK SHELL AS";
const wchar_t* kKwSwift =
    L"associatedtype class deinit enum extension fileprivate func import init inout internal let open operator private "
    L"protocol public static struct subscript typealias var break case continue default defer do else fallthrough for "
    L"guard if in repeat return switch where while as catch false is nil rethrows super self Self throw throws true try "
    L"some any await async actor";
const wchar_t* kKwKotlin =
    L"as break class continue do else false for fun if in interface is null object package return super this throw true "
    L"try typealias typeof val var when while by catch constructor delegate dynamic field file finally get import init "
    L"param property receiver set setparam where abstract annotation companion const crossinline data enum expect "
    L"external final infix inline inner internal lateinit noinline open operator out override private protected public "
    L"reified sealed suspend tailrec vararg";
const wchar_t* kKwPhp =
    L"abstract and array as break callable case catch class clone const continue declare default do echo else elseif "
    L"empty enddeclare endfor endforeach endif endswitch endwhile extends final finally fn for foreach function global "
    L"goto if implements include include_once instanceof insteadof interface isset list match namespace new or print "
    L"private protected public readonly require require_once return static switch throw trait try unset use var while "
    L"xor yield true false null self parent";

const Lang kLangs[] = {
    {L".c .h .cc .cpp .cxx .hh .hpp .hxx .inl .m .mm .ino", L"//", L"/*", L"*/", L"\"'", false, true, false, 0, kKwC},
    {L".js .jsx .mjs .cjs .ts .tsx", L"//", L"/*", L"*/", L"\"'`", false, false, false, 0, kKwJs},
    {L".cs", L"//", L"/*", L"*/", L"\"'", false, true, false, 0, kKwCs},
    {L".java .groovy .scala", L"//", L"/*", L"*/", L"\"'", false, false, false, 0, kKwJava},
    {L".go", L"//", L"/*", L"*/", L"\"'`", false, false, false, 0, kKwGo},
    {L".rs", L"//", L"/*", L"*/", L"\"", false, false, false, 0, kKwRust},
    {L".swift", L"//", L"/*", L"*/", L"\"", false, false, false, 0, kKwSwift},
    {L".kt .kts", L"//", L"/*", L"*/", L"\"", false, false, false, 0, kKwKotlin},
    {L".php", L"// #", L"/*", L"*/", L"\"'", false, false, false, 0, kKwPhp},
    {L".dart", L"//", L"/*", L"*/", L"\"'", false, false, false, 0, kKwJs},
    {L".py .pyw .pyi", L"#", L"", L"", L"\"'", true, false, false, 0, kKwPy},
    {L".sh .bash .zsh .fish .bashrc .zshrc .profile", L"#", L"", L"", L"\"'", false, false, false, 0, kKwSh},
    {L".ps1 .psm1 .psd1", L"#", L"<#", L"#>", L"\"'", false, false, true, 0, kKwPs},
    {L".bat .cmd", L"rem ::", L"", L"", L"\"", false, false, true, 0, kKwBat},
    {L".lua", L"--", L"--[[", L"]]", L"\"'", false, false, false, 0, kKwLua},
    {L".rb .rake Gemfile Rakefile", L"#", L"=begin", L"=end", L"\"'", false, false, false, 0, kKwRuby},
    {L".pl .pm", L"#", L"", L"", L"\"'", false, false, false, 0, kKwSh},
    {L".sql", L"--", L"/*", L"*/", L"'\"", false, false, true, 0, kKwSql},
    {L".css .scss .less", L"//", L"/*", L"*/", L"\"'", false, false, false, 0, kKwCss},
    {L".html .htm .xml .xaml .svg .plist .csproj .vcxproj .props .targets .resx .xsl .xsd .wxs .aspx .vue .svelte", L"", L"<!--", L"-->", L"\"'", false, false, false, 1, L""},
    {L".json .jsonc .json5 .webmanifest", L"//", L"/*", L"*/", L"\"", false, false, false, 5, kKwJson},
    {L".yml .yaml", L"#", L"", L"", L"\"'", false, false, false, 2, kKwJson},
    {L".toml .ini .cfg .conf .properties .env .gitconfig .editorconfig .txt .desktop .service", L"# ;", L"", L"", L"\"'", false, false, false, 2, kKwJson},
    {L".md .markdown .mdx .mkd", L"", L"", L"", L"", false, false, false, 3, L""},
    {L".diff .patch", L"", L"", L"", L"", false, false, false, 4, L""},
    {L"Dockerfile .dockerfile", L"#", L"", L"", L"\"'", false, false, false, 0, kKwDocker},
    {L"Makefile makefile .mk .cmake CMakeLists.txt", L"#", L"", L"", L"\"'", false, false, false, 0, L"if else endif ifeq ifneq ifdef ifndef include define endef export unexport override cmake_minimum_required project add_executable add_library target_link_libraries set option message find_package"},
};

// The language for a file, or null.
const Lang* LangFor(const std::wstring& path) {
    const std::wstring ext = ExtOf(path.c_str(), path.size()), leaf = LeafOf(path);
    for (const Lang& l : kLangs) {
        const std::wstring list = l.exts;
        size_t p = 0;
        while (p < list.size()) {
            size_t sp = list.find(L' ', p);
            const std::wstring tok = list.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p);
            p = sp == std::wstring::npos ? list.size() : sp + 1;
            if (tok.empty()) continue;
            if (tok[0] == L'.' ? ext == tok : _wcsicmp(leaf.c_str(), tok.c_str()) == 0) return &l;
        }
    }
    return nullptr;
}

bool SxWord(wchar_t c) { return iswalnum(c) || c == L'_' || c == L'-' && false; }
bool SxIdent(wchar_t c) { return iswalnum(c) || c == L'_' || c == L'$' || c == L'?' || c == L'!'; }

struct SxKeywords {
    std::unordered_set<std::wstring> set;
    bool caseless = false;
    bool has(std::wstring w) const {
        if (caseless) w = Lower(w);
        return set.count(w) != 0;
    }
};
SxKeywords SxBuild(const Lang& l) {
    SxKeywords k;
    k.caseless = l.caseless;
    std::wstring s = l.keywords;
    size_t p = 0;
    while (p < s.size()) {
        size_t sp = s.find(L' ', p);
        std::wstring w = s.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p);
        p = sp == std::wstring::npos ? s.size() : sp + 1;
        if (!w.empty()) k.set.insert(l.caseless ? Lower(w) : w);
    }
    return k;
}

bool SxStarts(const std::wstring& s, size_t i, const wchar_t* what) {
    const size_t n = wcslen(what);
    return n && s.compare(i, n, what) == 0;
}

// One of the line-comment starters at i ("rem" needs a word boundary after it).
bool SxLineComment(const std::wstring& s, size_t i, const Lang& l, size_t& len) {
    std::wstring list = l.line1;
    size_t p = 0;
    while (p < list.size()) {
        size_t sp = list.find(L' ', p);
        std::wstring tok = list.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p);
        p = sp == std::wstring::npos ? list.size() : sp + 1;
        if (tok.empty()) continue;
        const bool word = iswalpha(tok[0]);
        if (word) {
            if (i + tok.size() <= s.size() && _wcsnicmp(s.c_str() + i, tok.c_str(), tok.size()) == 0 && (i + tok.size() == s.size() || !iswalnum(s[i + tok.size()])) &&
                (i == 0 || s[i - 1] == L'\n' || s[i - 1] == L' ' || s[i - 1] == L'\t' || s[i - 1] == L'&')) {
                len = tok.size();
                return true;
            }
        } else if (SxStarts(s, i, tok.c_str())) {
            len = tok.size();
            return true;
        }
    }
    return false;
}

void SxPush(std::vector<Span>& out, size_t start, size_t end, Tok cls) {
    if (end > start) out.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(end - start), cls});
}

// The whole text, once. Multi-line constructs (block comments, triple strings) carry across lines.
std::vector<Span> Highlight(const std::wstring& s, const Lang& l) {
    std::vector<Span> out;
    const SxKeywords kw = SxBuild(l);
    const size_t n = s.size();
    auto lineEnd = [&](size_t i) {
        size_t e = s.find(L'\n', i);
        return e == std::wstring::npos ? n : e;
    };
    if (l.family == 4) {  // diff: by line
        for (size_t i = 0; i < n;) {
            const size_t e = lineEnd(i);
            if (SxStarts(s, i, L"+++") || SxStarts(s, i, L"---") || SxStarts(s, i, L"diff ") || SxStarts(s, i, L"index ")) SxPush(out, i, e, Tok::Meta);
            else if (SxStarts(s, i, L"@@")) SxPush(out, i, e, Tok::Keyword);
            else if (s[i] == L'+') SxPush(out, i, e, Tok::Added);
            else if (s[i] == L'-') SxPush(out, i, e, Tok::Removed);
            i = e + 1;
        }
        return out;
    }
    if (l.family == 3) {  // markdown: headings, quotes, code spans and fenced code, bold markers
        bool fence = false;
        for (size_t i = 0; i < n;) {
            const size_t e = lineEnd(i);
            size_t j = i;
            while (j < e && (s[j] == L' ' || s[j] == L'\t')) ++j;
            if (SxStarts(s, j, L"```") || SxStarts(s, j, L"~~~")) {
                fence = !fence;
                SxPush(out, i, e, Tok::Meta);
            } else if (fence) {
                SxPush(out, i, e, Tok::String);
            } else if (j < e && s[j] == L'#') {
                SxPush(out, i, e, Tok::Heading);
            } else if (j < e && s[j] == L'>') {
                SxPush(out, i, e, Tok::Comment);
            } else {
                for (size_t k = j; k < e; ++k) {
                    if (s[k] == L'`') {
                        size_t c = s.find(L'`', k + 1);
                        if (c == std::wstring::npos || c > e) break;
                        SxPush(out, k, c + 1, Tok::String);
                        k = c;
                    } else if ((s[k] == L'*' || s[k] == L'_') && k + 1 < e && s[k + 1] == s[k]) {
                        size_t c = s.find(std::wstring(2, s[k]), k + 2);
                        if (c == std::wstring::npos || c > e) break;
                        SxPush(out, k, c + 2, Tok::Keyword);
                        k = c + 1;
                    } else if (s[k] == L'[') {
                        size_t c = s.find(L']', k);
                        if (c != std::wstring::npos && c < e && c + 1 < e && s[c + 1] == L'(') {
                            size_t p = s.find(L')', c);
                            if (p != std::wstring::npos && p < e) {
                                SxPush(out, k, c + 1, Tok::Attr);
                                SxPush(out, c + 1, p + 1, Tok::Comment);
                                k = p;
                            }
                        }
                    }
                }
                if (j < e && (s[j] == L'-' || s[j] == L'*' || s[j] == L'+') && j + 1 < e && s[j + 1] == L' ') SxPush(out, j, j + 1, Tok::Keyword);
            }
            i = e + 1;
        }
        return out;
    }
    const std::wstring quotes = l.quotes;
    bool lineStart = true, inTag = false;
    for (size_t i = 0; i < n;) {
        const wchar_t c = s[i];
        if (c == L'\n') {
            lineStart = true;
            ++i;
            continue;
        }
        // keys and sections (ini, yaml): at the start of a line
        if (l.family == 2 && lineStart && c != L' ' && c != L'\t') {
            const size_t e = lineEnd(i);
            if (c == L'[') {
                SxPush(out, i, e, Tok::Keyword);
                i = e;
                continue;
            }
            size_t len = 0;
            if (SxLineComment(s, i, l, len)) {
                SxPush(out, i, e, Tok::Comment);
                i = e;
                continue;
            }
            size_t sep = std::wstring::npos;
            for (size_t k = i; k < e; ++k)
                if (s[k] == L'=' || s[k] == L':') {
                    sep = k;
                    break;
                }
            if (sep != std::wstring::npos && sep > i) {
                SxPush(out, i, sep, Tok::Attr);
                i = sep + 1;
                lineStart = false;
                continue;
            }
        } else if (l.family == 2 && lineStart && (c == L' ' || c == L'\t')) {  // yaml: indented keys too
            size_t j = i;
            while (j < n && (s[j] == L' ' || s[j] == L'\t')) ++j;
            const size_t e = lineEnd(j);
            if (j < e && s[j] == L'-' && j + 1 < e && s[j + 1] == L' ') j += 2;
            size_t sep = std::wstring::npos;
            for (size_t k = j; k < e; ++k) {
                if (s[k] == L':' && (k + 1 == e || s[k + 1] == L' ')) {
                    sep = k;
                    break;
                }
                if (s[k] == L'#' || s[k] == L'"' || s[k] == L'\'') break;
            }
            if (sep != std::wstring::npos && sep > j) {
                SxPush(out, j, sep, Tok::Attr);
                i = sep + 1;
                lineStart = false;
                continue;
            }
            i = j;
            lineStart = false;
            continue;
        }
        if (c == L' ' || c == L'\t' || c == L'\r') {
            ++i;
            continue;
        }
        // directives
        if (l.preproc && lineStart && c == L'#') {
            size_t e = i + 1;
            while (e < n && (iswalpha(s[e]) || s[e] == L' ')) ++e;
            SxPush(out, i, e, Tok::Meta);
            i = e;
            lineStart = false;
            continue;
        }
        lineStart = false;
        // comments
        if (l.blockOpen[0] && SxStarts(s, i, l.blockOpen)) {
            size_t e = s.find(l.blockClose, i + wcslen(l.blockOpen));
            e = e == std::wstring::npos ? n : e + wcslen(l.blockClose);
            SxPush(out, i, e, Tok::Comment);
            i = e;
            continue;
        }
        size_t clen = 0;
        if (!inTag && SxLineComment(s, i, l, clen)) {
            const size_t e = lineEnd(i);
            SxPush(out, i, e, Tok::Comment);
            i = e;
            continue;
        }
        // strings
        if (quotes.find(c) != std::wstring::npos) {
            size_t e;
            if (l.triple && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
                const std::wstring t(3, c);
                e = s.find(t, i + 3);
                e = e == std::wstring::npos ? n : e + 3;
            } else {
                e = i + 1;
                while (e < n && s[e] != c && s[e] != L'\n') {
                    if (s[e] == L'\\' && e + 1 < n && l.family != 2) ++e;
                    ++e;
                }
                if (e < n && s[e] == c) ++e;
            }
            // json and yaml keys: a string followed by ':'
            Tok cls = Tok::String;
            if (l.family == 5 || l.family == 2) {
                size_t k = e;
                while (k < n && (s[k] == L' ' || s[k] == L'\t')) ++k;
                if (k < n && s[k] == L':') cls = Tok::Attr;
            }
            SxPush(out, i, e, cls);
            i = e;
            continue;
        }
        // markup: tags and attributes
        if (l.family == 1) {
            if (c == L'<' && i + 1 < n && (iswalpha(s[i + 1]) || s[i + 1] == L'/' || s[i + 1] == L'?' || s[i + 1] == L'!')) {
                size_t e = i + 1;
                if (s[e] == L'/' || s[e] == L'?' || s[e] == L'!') ++e;
                while (e < n && (iswalnum(s[e]) || s[e] == L'-' || s[e] == L':' || s[e] == L'.')) ++e;
                SxPush(out, i, e, Tok::Tag);
                inTag = true;
                i = e;
                continue;
            }
            if (inTag && (c == L'>' || (c == L'/' && i + 1 < n && s[i + 1] == L'>'))) {
                const size_t e = c == L'/' ? i + 2 : i + 1;
                SxPush(out, i, e, Tok::Tag);
                inTag = false;
                i = e;
                continue;
            }
            if (inTag && (iswalpha(c) || c == L'@' || c == L':')) {
                size_t e = i;
                while (e < n && (iswalnum(s[e]) || s[e] == L'-' || s[e] == L':' || s[e] == L'.' || s[e] == L'@')) ++e;
                SxPush(out, i, e, Tok::Attr);
                i = e;
                continue;
            }
            if (c == L'&') {  // entities
                size_t e = s.find(L';', i);
                if (e != std::wstring::npos && e - i < 10) {
                    SxPush(out, i, e + 1, Tok::Number);
                    i = e + 1;
                    continue;
                }
            }
            ++i;
            continue;
        }
        // numbers
        if (iswdigit(c) || (c == L'.' && i + 1 < n && iswdigit(s[i + 1]))) {
            size_t e = i + 1;
            while (e < n && (iswalnum(s[e]) || s[e] == L'.' || s[e] == L'_')) ++e;
            if (i == 0 || !SxIdent(s[i - 1])) SxPush(out, i, e, Tok::Number);
            i = e;
            continue;
        }
        // words: keywords
        if (iswalpha(c) || c == L'_' || c == L'$' || c == L'@') {
            size_t e = i + 1;
            while (e < n && SxIdent(s[e])) ++e;
            if (c == L'$' && l.exts[1] == L'p' && l.exts[2] == L's') SxPush(out, i, e, Tok::Attr);  // powershell variables
            else if (c == L'@' && l.family == 0) SxPush(out, i, e, Tok::Meta);                      // decorators and annotations
            else if (kw.has(s.substr(i, e - i))) SxPush(out, i, e, Tok::Keyword);
            i = e;
            continue;
        }
        ++i;
    }
    return out;
}

// The colors, by class (dark theme, in the spirit of the usual editors).
UINT32 TokColor(Tok t) {
    switch (t) {
        case Tok::Comment: return 0x6A9955;
        case Tok::String: return 0xCE9178;
        case Tok::Number: return 0xB5CEA8;
        case Tok::Keyword: return 0x569CD6;
        case Tok::Meta: return 0xC586C0;
        case Tok::Tag: return 0x4EC9B0;
        case Tok::Attr: return 0x9CDCFE;
        case Tok::Heading: return 0x569CD6;
        case Tok::Added: return 0x6A9955;
        case Tok::Removed: return 0xF48771;
        default: return 0xD4D4DA;
    }
}

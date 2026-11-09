// Preview (Space): right-hand column showing what's under the cursor.
// Images via WIC (scaled and rotated per EXIF, with the camera's data under photos), SVG (Direct2D), fonts (a sample
// in the typeface itself), Markdown rendered (markdown.inl), text/code (minified JSON indented), CSV as a table,
// folder contents (from the listing's memory), the Shell thumbnail (PDF, videos, Office...) and, for binaries
// Windows has nothing to show for, their bytes in hex. It all loads on a separate thread and only once the cursor
// stops: moving through the list doesn't slow down.
// Included from app.cpp after palette.inl.

constexpr UINT WM_APP_PREVIEW = WM_APP + 6;
constexpr UINT_PTR kTimerPreview = 6;

// A font loaded from memory gets a loader of its own, unregistered once the preview moves on: the file is never
// held open (it can be renamed or deleted while it shows) and nothing builds up.
struct PvFontHold {
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ~PvFontHold() {
        if (f5 && loader) f5->UnregisterFontFileLoader(loader.Get());
    }
};

struct PreviewResult {
    uint64_t gen = 0;
    std::wstring path;
    int kind = 0;  // 0 nothing, 1 image, 2 text, 3 not on this PC, 4 hex (a binary), 5 table (CSV), 6 Markdown, 7 font, 8 SVG
    ComPtr<IWICBitmap> bmp;
    UINT imgW = 0, imgH = 0;  // original size (already rotated)
    bool thumb = false;       // Shell thumbnail or icon (not enlarged)
    std::wstring text;
    bool truncated = false;
    bool indented = false;  // JSON that was all on one line, shown indented
    std::wstring typeName;
    std::vector<std::pair<std::wstring, std::wstring>> facts;  // photos: camera, exposure, when, where (EXIF)
    std::vector<uint8_t> bytes;                                // hex: the start of the file
    uint64_t fileSize = 0;
    std::vector<std::vector<std::wstring>> rows;  // table: the first one is the header
    int cols = 0;
    std::unique_ptr<MdDoc> md;  // Markdown, in blocks
    std::string svg;            // SVG, in UTF-8, its CSS made attributes (Direct2D reads only those)
    std::shared_ptr<PvFontHold> fontHold;               // declared before the formats: it goes after them
    std::vector<ComPtr<IDWriteTextFormat>> fontFmt;     // fonts: the name, the alphabet, then the sample sizes
    std::wstring fontName, fontFace;
    int fontFaces = 0;
};

struct PreviewState {
    std::wstring want;  // what's under the cursor
    std::wstring path;  // what was requested/is shown
    uint64_t gen = 0;
    std::unique_ptr<PreviewResult> res;
    ComPtr<ID2D1Bitmap> d2d;
    std::vector<std::pair<size_t, size_t>> lines;  // (start, length) in res->text
    std::vector<Span> spans;                       // syntax highlighting, sorted by start
    float scroll = 0;
    float scrollX = 0;  // tables: sideways (Shift+wheel)
    std::vector<float> colW;  // tables: each column's width
    std::vector<char> colNum;  // tables: columns of numbers, aligned to the right
    ComPtr<ID2D1SvgDocument> svg;  // made on the first draw, for this render target
    float svgW = 0, svgH = 0;      // its own size
    D2D1_SIZE_F svgVp{};
    bool svgTried = false;
    bool loading = false;
    bool isDir = false;  // the shown item is a folder
    D2D1_RECT_F box{};
} g_pv;

// Type names already seen, by extension: moving through the list shows them without waiting for the load.
std::unordered_map<std::wstring, std::wstring> g_pvTypes;
std::wstring PvExtKey(const std::wstring& path) { return Key(ExtOf(path.c_str(), path.size())); }

std::mutex g_pvm;
std::condition_variable g_pvcv;
std::wstring g_pvReq;
uint64_t g_pvReqGen = 0;
bool g_pvReqMd = true;  // Markdown rendered ([general] preview_markdown), as it was when asked

bool PvIsImage(const std::wstring& ext) {
    static const wchar_t* k[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".gif", L".tif", L".tiff",
                                 L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp", L".dds"};
    for (auto e : k)
        if (ext == e) return true;
    return false;
}
bool PvIsBinary(const std::wstring& ext) {
    static const wchar_t* k[] = {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".zip",
                                 L".7z", L".rar", L".exe", L".dll", L".msi", L".iso", L".mp4", L".mkv", L".avi",
                                 L".mov", L".wmv", L".webm", L".mp3", L".flac", L".wav", L".ogg", L".m4a", L".psd",
                                 L".ttf", L".otf", L".lnk", L".blend", L".fbx", L".stl", L".3mf"};
    for (auto e : k)
        if (ext == e) return true;
    return false;
}

bool PvIsTable(const std::wstring& ext) { return ext == L".csv" || ext == L".tsv"; }
bool PvIsMarkdown(const std::wstring& ext) { return ext == L".md" || ext == L".markdown" || ext == L".mdx" || ext == L".mkd"; }
bool PvIsFont(const std::wstring& ext) {
    return ext == L".ttf" || ext == L".otf" || ext == L".ttc" || ext == L".otc" || ext == L".woff" || ext == L".woff2";
}

// The whole of a file, up to max bytes (empty: it can't be read, or it is bigger).
std::vector<uint8_t> PvReadAll(const std::wstring& path, uint64_t max) {
    std::vector<uint8_t> data;
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= max) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(h);
    return data;
}

// --- fonts: the typeface's name in itself, its alphabet and a sentence at several sizes.
bool PvFont(const std::wstring& path, const std::wstring& ext, PreviewResult& r) {
    auto hold = std::make_shared<PvFontHold>();
    if (FAILED(g_dw.As(&hold->f5))) return false;
    std::vector<uint8_t> data = PvReadAll(path, 64ull << 20);
    if (data.empty()) return false;
    if (ext == L".woff" || ext == L".woff2") {  // packed for the web: unpacked in memory
        ComPtr<IDWriteFontFileStream> st;
        if (FAILED(hold->f5->UnpackFontFile(ext == L".woff" ? DWRITE_CONTAINER_TYPE_WOFF : DWRITE_CONTAINER_TYPE_WOFF2, data.data(),
                                            static_cast<UINT32>(data.size()), &st)))
            return false;
        UINT64 size = 0;
        const void* frag = nullptr;
        void* ctx = nullptr;
        if (FAILED(st->GetFileSize(&size)) || !size || size > (64ull << 20) || FAILED(st->ReadFileFragment(&frag, 0, size, &ctx))) return false;
        std::vector<uint8_t> raw(static_cast<const uint8_t*>(frag), static_cast<const uint8_t*>(frag) + size);
        st->ReleaseFileFragment(ctx);
        data = std::move(raw);
    }
    if (FAILED(hold->f5->CreateInMemoryFontFileLoader(&hold->loader))) return false;
    if (FAILED(hold->f5->RegisterFontFileLoader(hold->loader.Get()))) {
        hold->loader.Reset();
        return false;
    }
    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontSetBuilder1> sb;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> coll;
    ComPtr<IDWriteFontFamily> fam;
    ComPtr<IDWriteFont> font;
    if (FAILED(hold->loader->CreateInMemoryFontFileReference(hold->f5.Get(), data.data(), static_cast<UINT32>(data.size()), nullptr, &file)) ||
        FAILED(hold->f5->CreateFontSetBuilder(&sb)) || FAILED(sb->AddFontFile(file.Get())) || FAILED(sb->CreateFontSet(&set)) ||
        FAILED(hold->f5->CreateFontCollectionFromFontSet(set.Get(), &coll)) || !coll->GetFontFamilyCount() ||
        FAILED(coll->GetFontFamily(0, &fam)) ||
        FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))  // Regular, if it has one
        return false;
    auto name = [](IDWriteLocalizedStrings* s) {
        std::wstring out;
        UINT32 i = 0, len = 0;
        BOOL found = FALSE;
        if (!s) return out;
        if (FAILED(s->FindLocaleName(L"en-us", &i, &found)) || !found) i = 0;
        if (SUCCEEDED(s->GetStringLength(i, &len))) {
            out.resize(len + 1);
            s->GetString(i, out.data(), len + 1);
            out.resize(len);
        }
        return out;
    };
    ComPtr<IDWriteLocalizedStrings> family, face;
    fam->GetFamilyNames(&family);
    font->GetFaceNames(&face);
    r.fontName = name(family.Get());
    r.fontFace = name(face.Get());
    r.fontFaces = static_cast<int>(set->GetFontCount());
    for (float size : {28.f, 18.f, 12.f, 16.f, 22.f, 30.f, 40.f}) {
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(g_dw->CreateTextFormat(r.fontName.c_str(), coll.Get(), font->GetWeight(), font->GetStyle(), font->GetStretch(), size, L"en-us", &f))) {
            r.fontFmt.clear();
            return false;
        }
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        r.fontFmt.push_back(std::move(f));
    }
    r.fontHold = std::move(hold);
    r.kind = 7;
    return true;
}

// --- SVG. Direct2D draws its presentation attributes (fill="red") but no CSS, so style="..." and the <style> rules
// for tags, classes and ids become attributes, as a browser would apply them. No <text>: Direct2D leaves it out.
using SvgDecls = std::vector<std::pair<std::wstring, std::wstring>>;
SvgDecls SvgParseDecls(const std::wstring& d) {
    SvgDecls out;
    size_t p = 0;
    while (p < d.size()) {
        size_t e = d.find(L';', p);
        if (e == std::wstring::npos) e = d.size();
        const std::wstring one = d.substr(p, e - p);
        p = e + 1;
        const size_t colon = one.find(L':');
        if (colon == std::wstring::npos) continue;
        const std::wstring k = Lower(Trim(one.substr(0, colon)));
        std::wstring v = Trim(one.substr(colon + 1));
        const size_t imp = Lower(v).find(L"!important");
        if (imp != std::wstring::npos) v = Trim(v.substr(0, imp));
        if (!k.empty() && !v.empty()) out.push_back({k, v});
    }
    return out;
}
bool SvgPresentation(const std::wstring& k) {
    static const wchar_t* list[] = {L"fill", L"fill-opacity", L"fill-rule", L"stroke", L"stroke-width", L"stroke-opacity",
                                    L"stroke-linecap", L"stroke-linejoin", L"stroke-miterlimit", L"stroke-dasharray",
                                    L"stroke-dashoffset", L"opacity", L"display", L"visibility", L"stop-color", L"stop-opacity",
                                    L"clip-rule", L"clip-path", L"color", L"overflow"};
    for (const wchar_t* x : list)
        if (k == x) return true;
    return false;
}
std::wstring SvgInlineCss(const std::wstring& src) {
    const size_t npos = std::wstring::npos;
    if (src.find(L"style") == npos) return src;
    struct Rule {
        std::wstring tag, id;
        std::vector<std::wstring> classes;
        int spec = 0;
        SvgDecls decls;
    };
    std::vector<Rule> rules;
    for (size_t p = src.find(L"<style"); p != npos; p = src.find(L"<style", p + 1)) {
        const size_t gt = src.find(L'>', p);
        const size_t end = gt == npos ? npos : src.find(L"</style", gt);
        if (end == npos) break;
        std::wstring css = src.substr(gt + 1, end - gt - 1);
        for (const wchar_t* cut : {L"<![CDATA[", L"]]>"})
            for (size_t c = css.find(cut); c != npos; c = css.find(cut)) css.erase(c, wcslen(cut));
        for (size_t c = css.find(L"/*"); c != npos; c = css.find(L"/*", c)) {
            const size_t ce = css.find(L"*/", c + 2);
            css.erase(c, ce == npos ? npos : ce + 2 - c);
        }
        size_t a = 0;
        while (a < css.size()) {
            const size_t lb = css.find(L'{', a);
            if (lb == npos) break;
            const std::wstring sel = Trim(css.substr(a, lb - a));
            size_t rb = lb;
            int depth = 0;
            for (; rb < css.size(); ++rb) {  // braces inside counted: @media { a { } }
                if (css[rb] == L'{') ++depth;
                else if (css[rb] == L'}' && --depth == 0) break;
            }
            const SvgDecls decls = SvgParseDecls(css.substr(lb + 1, std::min(rb, css.size()) - lb - 1));
            a = rb + 1;
            if (sel.empty() || sel[0] == L'@') continue;
            for (size_t s0 = 0; s0 <= sel.size();) {
                size_t s1 = sel.find(L',', s0);
                if (s1 == npos) s1 = sel.size();
                const std::wstring one = Trim(sel.substr(s0, s1 - s0));
                s0 = s1 + 1;
                if (one.empty() || one.find_first_of(L" >+~:[*") != npos) continue;  // only tag, .class and #id
                Rule r;
                r.decls = decls;
                size_t k = 0;
                while (k < one.size() && one[k] != L'.' && one[k] != L'#') ++k;
                r.tag = one.substr(0, k);
                while (k < one.size()) {
                    size_t e = k + 1;
                    while (e < one.size() && one[e] != L'.' && one[e] != L'#') ++e;
                    if (one[k] == L'.') r.classes.push_back(one.substr(k + 1, e - k - 1));
                    else r.id = one.substr(k + 1, e - k - 1);
                    k = e;
                }
                r.spec = (r.id.empty() ? 0 : 100) + static_cast<int>(r.classes.size()) * 10 + (r.tag.empty() ? 0 : 1);
                rules.push_back(std::move(r));
            }
        }
    }
    std::stable_sort(rules.begin(), rules.end(), [](const Rule& x, const Rule& y) { return x.spec < y.spec; });
    std::wstring out;
    out.reserve(src.size() + src.size() / 4);
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        const size_t lt = src.find(L'<', i);
        if (lt == npos) {
            out.append(src, i, npos);
            break;
        }
        out.append(src, i, lt - i);
        auto copyThrough = [&](const wchar_t* mark) {
            const size_t e = src.find(mark, lt);
            const size_t stop = e == npos ? n : e + wcslen(mark);
            out.append(src, lt, stop - lt);
            i = stop;
        };
        if (src.compare(lt, 4, L"<!--") == 0) {
            copyThrough(L"-->");
            continue;
        }
        if (src.compare(lt, 9, L"<![CDATA[") == 0) {
            copyThrough(L"]]>");
            continue;
        }
        if (src.compare(lt, 6, L"<style") == 0) {  // its rules are applied below: it goes
            const size_t e = src.find(L"</style>", lt);
            i = e == npos ? n : e + 8;
            continue;
        }
        if (lt + 1 >= n || !iswalpha(src[lt + 1])) {
            out += L'<';
            i = lt + 1;
            continue;
        }
        // a start tag: its name and attributes, quotes respected
        size_t j = lt + 1;
        while (j < n && !iswspace(src[j]) && src[j] != L'/' && src[j] != L'>') ++j;
        const std::wstring name = src.substr(lt + 1, j - lt - 1);
        SvgDecls attrs;
        bool selfClose = false;
        while (j < n) {
            while (j < n && iswspace(src[j])) ++j;
            if (j >= n) break;
            if (src[j] == L'>') {
                ++j;
                break;
            }
            if (src[j] == L'/') {
                selfClose = true;
                ++j;
                continue;
            }
            size_t k = j;
            while (k < n && !iswspace(src[k]) && src[k] != L'=' && src[k] != L'>' && src[k] != L'/') ++k;
            if (k == j) {
                ++j;
                continue;
            }
            std::wstring an = src.substr(j, k - j), av;
            j = k;
            while (j < n && iswspace(src[j])) ++j;
            if (j < n && src[j] == L'=') {
                ++j;
                while (j < n && iswspace(src[j])) ++j;
                if (j < n && (src[j] == L'"' || src[j] == L'\'')) {
                    const size_t e = src.find(src[j], j + 1);
                    av = src.substr(j + 1, (e == npos ? n : e) - j - 1);
                    j = e == npos ? n : e + 1;
                } else {
                    size_t e = j;
                    while (e < n && !iswspace(src[e]) && src[e] != L'>') ++e;
                    av = src.substr(j, e - j);
                    j = e;
                }
            }
            selfClose = false;
            attrs.push_back({std::move(an), std::move(av)});
        }
        std::wstring cls = L" ", id;
        for (const auto& [k, v] : attrs) {
            if (k == L"class") cls = L" " + v + L" ";
            else if (k == L"id") id = v;
        }
        for (wchar_t& c : cls)
            if (iswspace(c)) c = L' ';
        const size_t colon = name.find(L':');
        const std::wstring local = colon == npos ? name : name.substr(colon + 1);
        SvgDecls decls;
        for (const Rule& r : rules) {
            if ((!r.tag.empty() && r.tag != local) || (!r.id.empty() && r.id != id)) continue;
            bool all = true;
            for (const std::wstring& c : r.classes)
                if (cls.find(L" " + c + L" ") == npos) all = false;
            if (all) decls.insert(decls.end(), r.decls.begin(), r.decls.end());
        }
        for (const auto& [k, v] : attrs)
            if (k == L"style") {
                const SvgDecls st = SvgParseDecls(v);
                decls.insert(decls.end(), st.begin(), st.end());
            }
        if (decls.empty()) {
            out.append(src, lt, j - lt);
            i = j;
            continue;
        }
        attrs.erase(std::remove_if(attrs.begin(), attrs.end(), [](const auto& a) { return a.first == L"style"; }), attrs.end());
        for (const auto& [k, v] : decls) {
            if (!SvgPresentation(k)) continue;
            auto it = std::find_if(attrs.begin(), attrs.end(), [&](const auto& a) { return a.first == k; });
            if (it != attrs.end()) it->second = v;
            else attrs.push_back({k, v});
        }
        out += L'<' + name;
        for (const auto& [k, v] : attrs) {
            std::wstring val = v;
            for (size_t q = val.find(L'"'); q != npos; q = val.find(L'"', q)) val.replace(q, 1, L"&quot;");
            out += L' ' + k + L"=\"" + val + L'"';
        }
        out += selfClose ? L"/>" : L">";
        i = j;
    }
    return out;
}

bool PvSvg(const std::wstring& path, PreviewResult& r) {
    const std::vector<uint8_t> data = PvReadAll(path, 16ull << 20);
    if (data.empty()) return false;
    std::wstring w;
    const bool utf16 = data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE;
    if (utf16) {
        w.assign(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
    } else {
        const size_t s = data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ? 3 : 0;
        const char* p = reinterpret_cast<const char*>(data.data() + s);
        const int n = MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(data.size() - s), nullptr, 0);
        w.resize(std::max(0, n));
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(data.size() - s), w.data(), n);
    }
    if (w.find(L"<svg") == std::wstring::npos) return false;
    std::wstring css = SvgInlineCss(w);
    if (utf16 && css.rfind(L"<?xml", 0) == 0) css.erase(0, css.find(L"?>") + 2);  // it said UTF-16; it goes as UTF-8
    const int m = WideCharToMultiByte(CP_UTF8, 0, css.data(), static_cast<int>(css.size()), nullptr, 0, nullptr, nullptr);
    r.svg.resize(std::max(0, m));
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, css.data(), static_cast<int>(css.size()), r.svg.data(), m, nullptr, nullptr);
    // the source, shown as text when Direct2D can't draw it
    const size_t keep = std::min<size_t>(w.size(), 256 * 1024);
    r.truncated = keep < w.size();
    for (size_t i = 0; i < keep; ++i)
        if (w[i] != L'\r') r.text += w[i];
    r.kind = 8;
    return true;
}
bool PvIsJson(const std::wstring& ext) { return ext == L".json" || ext == L".geojson" || ext == L".har" || ext == L".webmanifest"; }

// --- EXIF: what the camera wrote into a photo, for the lines under it. Tags by number, in JPEG (/app1/ifd) or in
// TIFF and HEIF (/ifd); dir is "", "/exif" or "/gps".
bool ExifGet(IWICMetadataQueryReader* q, const wchar_t* dir, int tag, PROPVARIANT& v) {
    for (const wchar_t* root : {L"/app1/ifd", L"/ifd"}) {
        const std::wstring p = std::wstring(root) + dir + L"/{ushort=" + std::to_wstring(tag) + L"}";
        PropVariantClear(&v);
        if (SUCCEEDED(q->GetMetadataByName(p.c_str(), &v))) return true;
    }
    return false;
}
std::wstring ExifText(IWICMetadataQueryReader* q, const wchar_t* dir, int tag) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring s;
    if (ExifGet(q, dir, tag, v)) {
        if (v.vt == VT_LPWSTR && v.pwszVal) {
            s = v.pwszVal;
        } else if (v.vt == VT_LPSTR && v.pszVal) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, v.pszVal, -1, nullptr, 0);
            if (n > 1) {
                s.resize(n - 1);
                MultiByteToWideChar(CP_UTF8, 0, v.pszVal, -1, s.data(), n);
            }
        }
    }
    PropVariantClear(&v);
    while (!s.empty() && (iswspace(s.back()) || !s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;
    return s.substr(a);
}
// A rational as WIC gives it: the numerator in the low half, the denominator in the high one.
double ExifRat(const ULARGE_INTEGER& u) { return u.HighPart ? static_cast<double>(u.LowPart) / u.HighPart : 0; }
double ExifNumber(IWICMetadataQueryReader* q, const wchar_t* dir, int tag) {
    PROPVARIANT v;
    PropVariantInit(&v);
    double d = 0;
    if (ExifGet(q, dir, tag, v)) {
        switch (v.vt) {
            case VT_UI8: d = ExifRat(v.uhVal); break;
            case VT_I8: d = v.hVal.HighPart ? static_cast<double>(static_cast<LONG>(v.hVal.LowPart)) / v.hVal.HighPart : 0; break;
            case VT_UI2: d = v.uiVal; break;
            case VT_UI4: d = v.ulVal; break;
            case VT_VECTOR | VT_UI2: d = v.caui.cElems ? v.caui.pElems[0] : 0; break;
            case VT_VECTOR | VT_UI8: d = v.cauh.cElems ? ExifRat(v.cauh.pElems[0]) : 0; break;
            default: break;
        }
    }
    PropVariantClear(&v);
    return d;
}
// A GPS coordinate (degrees, minutes, seconds) in degrees, negative to the south and west.
bool ExifCoord(IWICMetadataQueryReader* q, int tag, int refTag, double& out) {
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (ExifGet(q, L"/gps", tag, v) && v.vt == (VT_VECTOR | VT_UI8) && v.cauh.cElems >= 3) {
        out = ExifRat(v.cauh.pElems[0]) + ExifRat(v.cauh.pElems[1]) / 60 + ExifRat(v.cauh.pElems[2]) / 3600;
        ok = true;
    }
    PropVariantClear(&v);
    const std::wstring ref = ExifText(q, L"/gps", refTag);
    if (ok && !ref.empty() && (ref[0] == L'S' || ref[0] == L'W')) out = -out;
    return ok;
}
// A number with at most dec decimals, without trailing zeros: 1.8, 4, 0.5.
std::wstring ExifNum(double d, int dec) {
    wchar_t b[32];
    swprintf(b, 32, L"%.*f", dec, d);
    std::wstring s = b;
    if (s.find(L'.') != std::wstring::npos) {
        while (s.back() == L'0') s.pop_back();
        if (s.back() == L'.') s.pop_back();
    }
    return s;
}
void PvExif(IWICMetadataQueryReader* q, PreviewResult& r) {
    const std::wstring make = ExifText(q, L"", 271), model = ExifText(q, L"", 272);
    std::wstring camera = model.empty() ? make : model;
    if (!make.empty() && !model.empty()) {  // "Canon" + "Canon EOS R6" is just the model; "Apple" + "iPhone 13" both
        const std::wstring first = make.substr(0, make.find(L' '));
        if (_wcsnicmp(model.c_str(), first.c_str(), first.size()) != 0) camera = make + L" " + model;
    }
    if (!camera.empty()) r.facts.push_back({L"Camera", camera});
    const std::wstring lens = ExifText(q, L"/exif", 42036);
    if (!lens.empty()) r.facts.push_back({L"Lens", lens});
    std::wstring shot;
    auto add = [&](const std::wstring& s) { shot += (shot.empty() ? L"" : L"  ·  ") + s; };
    if (const double t = ExifNumber(q, L"/exif", 33434); t > 0) add(t < 0.25 ? L"1/" + ExifNum(std::round(1 / t), 0) + L" s" : ExifNum(t, 1) + L" s");
    if (const double f = ExifNumber(q, L"/exif", 33437); f > 0) add(L"f/" + ExifNum(f, 1));
    if (const double iso = ExifNumber(q, L"/exif", 34855); iso > 0) add(L"ISO " + ExifNum(iso, 0));
    double mm = ExifNumber(q, L"/exif", 41989);  // in 35 mm terms, which is how lenses are compared
    if (mm <= 0) mm = ExifNumber(q, L"/exif", 37386);
    if (mm > 0) add(ExifNum(mm, mm < 10 ? 1 : 0) + L" mm");
    if (!shot.empty()) r.facts.push_back({L"Exposure", shot});
    std::wstring when = ExifText(q, L"/exif", 36867);  // when it was taken; else when it was last saved
    if (when.empty()) when = ExifText(q, L"", 306);
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    if (swscanf_s(when.c_str(), L"%d:%d:%d %d:%d", &Y, &M, &D, &h, &m) == 5 && Y > 1900) {
        wchar_t b[40];
        swprintf(b, 40, L"%02d/%02d/%04d  %02d:%02d", D, M, Y, h, m);  // as FormatDate writes dates
        r.facts.push_back({L"Taken", b});
    }
    double lat = 0, lon = 0;
    if (ExifCoord(q, 2, 1, lat) && ExifCoord(q, 4, 3, lon) && (lat != 0 || lon != 0)) {
        wchar_t b[64];
        swprintf(b, 64, L"%.4f° %c, %.4f° %c", std::fabs(lat), lat < 0 ? L'S' : L'N', std::fabs(lon), lon < 0 ? L'W' : L'E');
        r.facts.push_back({L"Place", b});
    }
}

bool PvImage(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    GUID container{};
    UINT frames = 1;
    if (SUCCEEDED(dec->GetContainerFormat(&container)) && container == GUID_ContainerFormatIco &&
        SUCCEEDED(dec->GetFrameCount(&frames))) {
        for (UINT i = 1; i < frames; ++i) {  // an icon stores several sizes: the largest one looks best
            ComPtr<IWICBitmapFrameDecode> other;
            UINT ow = 0, oh = 0;
            if (SUCCEEDED(dec->GetFrame(i, &other)) && SUCCEEDED(other->GetSize(&ow, &oh)) && ow > w) {
                frame = other;
                w = ow;
                h = oh;
            }
        }
    }
    if (!w || !h) return false;
    // EXIF orientation (phone photos)
    USHORT orient = 1;
    ComPtr<IWICMetadataQueryReader> q;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&q))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v)) ||
            SUCCEEDED(q->GetMetadataByName(L"/ifd/{ushort=274}", &v))) {
            if (v.vt == VT_UI2) orient = v.uiVal;
        }
        PropVariantClear(&v);
        PvExif(q.Get(), r);
    }
    ComPtr<IWICBitmapSource> src = frame;
    const UINT maxSide = 2048;
    if (w > maxSide || h > maxSide) {
        double s = static_cast<double>(maxSide) / std::max(w, h);
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(src.Get(), std::max(1u, static_cast<UINT>(w * s)), std::max(1u, static_cast<UINT>(h * s)),
                                     WICBitmapInterpolationModeFant)))
            src = sc;
    }
    WICBitmapTransformOptions tr = WICBitmapTransformRotate0;
    switch (orient) {
        case 2: tr = WICBitmapTransformFlipHorizontal; break;
        case 3: tr = WICBitmapTransformRotate180; break;
        case 4: tr = WICBitmapTransformFlipVertical; break;
        case 5: tr = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
        case 6: tr = WICBitmapTransformRotate90; break;
        case 7: tr = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
        case 8: tr = WICBitmapTransformRotate270; break;
    }
    if (tr != WICBitmapTransformRotate0) {
        // the rotator needs the pixels in memory
        ComPtr<IWICBitmap> mem;
        ComPtr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(wic->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &mem)) &&
            SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(mem.Get(), tr)))
            src = rot;
        if (orient >= 5) std::swap(w, h);
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;
    if (FAILED(wic->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, &r.bmp))) return false;
    r.kind = 1;
    r.imgW = w;
    r.imgH = h;
    return true;
}

// The Shell's thumbnail or icon, through the thumbnail cache (thumbs.inl): instant the second time.
bool PvThumb(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r, bool* icon = nullptr) {
    UINT w = 0, h = 0;
    bool isIcon = false;
    if (!ThumbProduce(wic, path, 512, 0, r.bmp, w, h, isIcon)) return false;
    if (icon) *icon = isIcon;
    r.kind = 1;
    r.thumb = true;
    r.imgW = w;
    r.imgH = h;
    return true;
}

// 0: it can't be read; 1: text, in r.text; 2: a binary (null bytes near the start). raw keeps the tabs.
int PvText(const std::wstring& path, PreviewResult& r, bool raw = false) {
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    const DWORD kMax = 256 * 1024;
    std::vector<char> buf(kMax);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf.data(), kMax, &got, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!ok) return 0;
    buf.resize(got);
    r.truncated = static_cast<uint64_t>(size.QuadPart) > got;
    std::wstring w;
    if (got >= 2 && static_cast<uint8_t>(buf[0]) == 0xFF && static_cast<uint8_t>(buf[1]) == 0xFE) {
        w.assign(reinterpret_cast<const wchar_t*>(buf.data() + 2), (got - 2) / 2);
    } else {
        size_t start = got >= 3 && static_cast<uint8_t>(buf[0]) == 0xEF && static_cast<uint8_t>(buf[1]) == 0xBB &&
                               static_cast<uint8_t>(buf[2]) == 0xBF ? 3 : 0;
        // binary: there are null bytes near the start
        for (size_t i = start; i < std::min<size_t>(got, 8192); ++i)
            if (buf[i] == 0) return 2;
        size_t len = got - start;
        if (r.truncated) {  // cut at a line end so a character isn't split
            size_t nl = std::string_view(buf.data() + start, len).find_last_of('\n');
            if (nl != std::string_view::npos) {
                len = nl + 1;
            } else {  // all one line (minified): at least not in the middle of a UTF-8 character
                size_t p = len;
                while (p && len - p < 3 && (static_cast<uint8_t>(buf[start + p - 1]) & 0xC0) == 0x80) --p;
                if (p) {
                    const uint8_t lead = static_cast<uint8_t>(buf[start + p - 1]);
                    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    if (p - 1 + need > len) len = p - 1;
                }
            }
        }
        const char* p = buf.data() + start;
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, static_cast<int>(len), nullptr, 0);
        UINT cp = CP_UTF8;
        if (n <= 0 && len) {
            cp = CP_ACP;
            n = MultiByteToWideChar(cp, 0, p, static_cast<int>(len), nullptr, 0);
        }
        w.resize(std::max(0, n));
        if (n > 0) MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, p, static_cast<int>(len), w.data(), n);
    }
    // tabs to 4 spaces, no carriage returns
    r.text.reserve(w.size());
    size_t col = 0;
    for (wchar_t c : w) {
        if (c == L'\r') continue;
        if (c == L'\t' && !raw) {
            size_t sp = 4 - col % 4;
            r.text.append(sp, L' ');
            col += sp;
            continue;
        }
        r.text += c;
        col = c == L'\n' ? 0 : col + 1;
    }
    r.kind = 2;
    return 1;
}

// Minified JSON (all on one line, or nearly) indented two spaces a level. Anything with comments is left as it is.
bool JsonIndent(std::wstring& s) {
    size_t lines = 1;
    for (wchar_t c : s) lines += c == L'\n';
    if (!(lines <= 2 && s.size() > 100) && s.size() / lines <= 300) return false;
    std::wstring out;
    out.reserve(s.size() + s.size() / 2);
    int depth = 0;
    auto newline = [&] {
        out += L'\n';
        out.append(static_cast<size_t>(depth) * 2, L' ');
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'"') {
            size_t j = i + 1;
            while (j < s.size() && s[j] != L'"') j += s[j] == L'\\' ? 2 : 1;
            j = std::min(j, s.size() - 1);
            out.append(s, i, j - i + 1);
            i = j;
        } else if (c == L'{' || c == L'[') {
            size_t j = i + 1;
            while (j < s.size() && iswspace(s[j])) ++j;
            out += c;
            if (j < s.size() && (s[j] == L'}' || s[j] == L']')) {  // empty: {} and [] stay together
                out += s[j];
                i = j;
            } else {
                ++depth;
                newline();
            }
        } else if (c == L'}' || c == L']') {
            if (--depth < 0) return false;
            newline();
            out += c;
        } else if (c == L',') {
            out += c;
            newline();
        } else if (c == L':') {
            out += L": ";
        } else if (c == L'/') {
            return false;
        } else if (!iswspace(c)) {
            out += c;
        }
    }
    s = std::move(out);
    return true;
}

// CSV or TSV (in r.text, tabs kept) into rows, quotes as in RFC 4180. The separator is whichever of , ; tab |
// the first line has most of. False when there is only one column: then it stays text.
bool PvTable(const std::wstring& ext, PreviewResult& r) {
    const std::wstring& s = r.text;
    wchar_t sep = L'\t';
    if (ext != L".tsv") {
        const size_t eol = std::min(s.find(L'\n'), s.size());
        int best = 0;
        for (wchar_t c : {L',', L';', L'\t', L'|'}) {
            int n = 0;
            bool q = false;
            for (size_t i = 0; i < eol; ++i) {
                if (s[i] == L'"') q = !q;
                else if (!q && s[i] == c) ++n;
            }
            if (n > best) {
                best = n;
                sep = c;
            }
        }
        if (!best) return false;
    }
    std::vector<std::wstring> row;
    std::wstring cell;
    bool q = false;
    size_t cols = 0;
    auto endCell = [&] {
        if (row.size() < 200) row.push_back(std::move(cell));
        cell.clear();
    };
    auto endRow = [&] {
        endCell();
        if (row.size() > 1 || !row[0].empty()) {
            cols = std::max(cols, row.size());
            r.rows.push_back(std::move(row));
        }
        row.clear();
    };
    for (size_t i = 0; i < s.size() && r.rows.size() < 20000; ++i) {
        const wchar_t c = s[i];
        if (q) {
            if (c == L'"' && i + 1 < s.size() && s[i + 1] == L'"') {
                cell += L'"';
                ++i;
            } else if (c == L'"') {
                q = false;
            } else if (cell.size() < 500) {
                cell += c == L'\n' || c == L'\t' ? L' ' : c;  // a cell is one line
            }
        } else if (c == L'"' && cell.empty()) {
            q = true;
        } else if (c == sep) {
            endCell();
        } else if (c == L'\n') {
            endRow();
        } else if (cell.size() < 500) {
            cell += c == L'\t' ? L' ' : c;
        }
    }
    if (!cell.empty() || !row.empty()) endRow();
    if (cols < 2) {
        r.rows.clear();
        return false;
    }
    if (r.rows.size() >= 20000) r.truncated = true;
    r.cols = static_cast<int>(cols);
    r.text.clear();
    r.text.shrink_to_fit();
    r.kind = 5;
    return true;
}

// The start of a binary, for the hex view.
bool PvHex(const std::wstring& path, PreviewResult& r) {
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD kMax = 64 * 1024;
    r.bytes.resize(kMax);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, r.bytes.data(), kMax, &got, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!ok) return false;
    r.bytes.resize(got);
    r.fileSize = static_cast<uint64_t>(size.QuadPart);
    r.truncated = r.fileSize > got;
    r.kind = 4;
    return true;
}

void PreviewThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    uint64_t seen = 0;
    for (;;) {
        std::wstring path;
        uint64_t gen;
        bool md;
        {
            std::unique_lock lk(g_pvm);
            g_pvcv.wait(lk, [&] { return g_pvReqGen != seen; });
            path = g_pvReq;
            gen = seen = g_pvReqGen;
            md = g_pvReqMd;
        }
        auto r = std::make_unique<PreviewResult>();
        r->gen = gen;
        r->path = path;
        SHFILEINFOW sfi{};
        const bool online = OnlineOnlyPath(path);
        if (SHGetFileInfoW(path.c_str(), online ? FILE_ATTRIBUTE_NORMAL : 0, &sfi, sizeof(sfi), SHGFI_TYPENAME | (online ? SHGFI_USEFILEATTRIBUTES : 0)))
            r->typeName = sfi.szTypeName;
        std::wstring ext = ExtOf(path.c_str(), path.size());
        if (online) {  // a thumbnail Windows already has, at most: reading it would download it
            if (!wic || !PvThumb(wic.Get(), path, *r)) r->kind = 3;
        } else {
            bool done = wic && PvIsImage(ext) && PvImage(wic.Get(), path, *r);
            if (!done && PvIsFont(ext)) done = PvFont(path, ext, *r);
            if (!done && ext == L".svg") done = PvSvg(path, *r);
            int text = 0;
            if (!done && !PvIsBinary(ext)) {
                const bool table = PvIsTable(ext);
                text = PvText(path, *r, table);
                done = text == 1;
                if (done && table) {
                    PvTable(ext, *r);
                } else if (done && PvIsJson(ext)) {
                    r->indented = JsonIndent(r->text);
                } else if (done && md && PvIsMarkdown(ext)) {
                    r->md = std::make_unique<MdDoc>();
                    r->md->blocks = MdParse(r->text);
                    r->text.clear();
                    r->kind = 6;
                }
            }
            bool icon = false;
            if (!done && wic) done = PvThumb(wic.Get(), path, *r, &icon);
            if (text == 2 && (!done || icon)) {  // a binary Windows has nothing better for than its icon: its bytes
                r->kind = 0;
                r->bmp.Reset();
                r->thumb = false;
                PvHex(path, *r);
            }
        }
        PreviewResult* raw = r.release();
        if (!PostMessageW(g_hwnd, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }
}

// What's under the active tab's cursor ("" = nothing).
std::wstring PreviewTarget() {
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) return L"";
    return RowPath(t, t.sel);
}

// Called a little while after the cursor stops moving.
void PreviewLoad() {
    if (!g_preview || g_pv.want == g_pv.path) return;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(PreviewThread).detach(); });
    g_pv.path = g_pv.want;
    g_pv.res.reset();
    g_pv.d2d.Reset();
    g_pv.lines.clear();
    g_pv.scroll = 0;
    g_pv.scrollX = 0;
    g_pv.svg.Reset();
    g_pv.svgTried = false;
    g_pv.loading = false;
    if (g_pv.path.empty()) {
        Invalidate();
        return;
    }
    DWORD a = GetFileAttributesW(ToExtendedPath(g_pv.path).c_str());
    g_pv.isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    if (g_pv.isDir) {
        if (!g_cache.count(Key(g_pv.path))) Enqueue(g_pv.path, JobKind::Revalidate);
        Invalidate();
        return;
    }
    g_pv.loading = true;
    {
        std::lock_guard lk(g_pvm);
        g_pvReq = g_pv.path;
        g_pvReqMd = g_cfg.previewMarkdown;
        g_pv.gen = ++g_pvReqGen;
    }
    g_pvcv.notify_one();
    Invalidate();
}

// A text result's lines and highlighting, for drawing.
void PvTextLines(PreviewResult& r) {
    g_pv.lines.clear();
    g_pv.spans.clear();
    // tabs become four spaces: the mono layout and the wrap arithmetic then agree
    for (size_t p = r.text.find(L'\t'); p != std::wstring::npos; p = r.text.find(L'\t', p + 4)) r.text.replace(p, 1, L"    ");
    if (const Lang* l = LangFor(r.path)) g_pv.spans = Highlight(r.text, *l);
    size_t start = 0;
    const std::wstring& s = r.text;
    while (start <= s.size() && g_pv.lines.size() < 20000) {
        size_t nl = s.find(L'\n', start);
        size_t end = nl == std::wstring::npos ? s.size() : nl;
        g_pv.lines.push_back({start, end - start});
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
}

void PreviewApply(PreviewResult* raw) {
    std::unique_ptr<PreviewResult> r(raw);
    if (r->gen != g_pv.gen || r->path != g_pv.path) return;
    g_pv.loading = false;
    g_pv.d2d.Reset();
    g_pv.lines.clear();
    g_pv.spans.clear();
    g_pv.colW.clear();
    g_pv.colNum.clear();
    if (r->kind == 5) {
        // each column as wide as its longest cell among the first rows (within limits); numbers to the right
        const int nc = r->cols;
        std::vector<const std::wstring*> longest(nc, nullptr);
        std::vector<int> filled(nc, 0);
        g_pv.colNum.assign(nc, 1);
        const size_t sample = std::min<size_t>(r->rows.size(), 400);
        for (size_t i = 0; i < sample; ++i)
            for (int c = 0; c < nc && c < static_cast<int>(r->rows[i].size()); ++c) {
                const std::wstring& s = r->rows[i][c];
                if (!longest[c] || s.size() > longest[c]->size()) longest[c] = &s;
                if (i == 0 || s.empty()) continue;
                ++filled[c];
                bool digit = false, other = false;
                for (wchar_t ch : s) {
                    if (iswdigit(ch)) digit = true;
                    else if (!wcschr(L" .,-+%$€eE", ch)) other = true;
                }
                if (other || !digit) g_pv.colNum[c] = 0;
            }
        for (int c = 0; c < nc; ++c) {
            if (!filled[c]) g_pv.colNum[c] = 0;
            const float w = longest[c] ? MeasureW(*longest[c], g_fmtNameBold.Get()) : 0;
            g_pv.colW.push_back(std::clamp(std::ceil(w) + 18, 44.f, 280.f));
        }
    }
    g_pv.svg.Reset();
    g_pv.svgTried = false;
    if (r->kind == 2) PvTextLines(*r);
    if (!r->typeName.empty()) g_pvTypes[PvExtKey(r->path)] = r->typeName;
    g_pv.res = std::move(r);
    Invalidate();
}

void PreviewToggle() {
    g_preview = !g_preview;
    g_pv.want.clear();
    g_pv.path = L"\x01";  // forces a load
    for (auto& tp : g_tabs) ClampScroll(*tp);
    LayoutEdit();
    LayoutRename();
    Invalidate();
    TouchSession();
}

// steps: notches of the wheel, up or left positive. Sideways only moves tables.
void PreviewWheel(float steps, bool sideways = false) {
    if (sideways && !(g_pv.res && g_pv.res->kind == 5)) return;
    float& s = sideways ? g_pv.scrollX : g_pv.scroll;
    s = std::max(0.f, s - steps * 3 * 16.f);
    Invalidate();
}

// The width of one character in the monospaced preview font.
float MonoCharW() {
    static float w = 0;
    static IDWriteTextFormat* forFmt = nullptr;
    if (forFmt != g_fmtMono.Get()) {
        w = std::max(1.f, MeasureW(L"MMMMMMMMMM", g_fmtMono.Get()) / 10);
        forFmt = g_fmtMono.Get();
    }
    return w;
}

// A light checkerboard, behind what can be transparent (SVG), for this render target (released with it).
ComPtr<ID2D1BitmapBrush> g_pvChecker;
ID2D1BitmapBrush* CheckerBrush() {
    if (!g_pvChecker) {
        uint32_t px[16 * 16];
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) px[y * 16 + x] = (x / 8 + y / 8) % 2 ? 0xFFE4E4E8 : 0xFFFFFFFF;
        ComPtr<ID2D1Bitmap> bmp;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(16, 16), px, 16 * 4, props, &bmp)))
            g_rt->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), &g_pvChecker);
    }
    return g_pvChecker.Get();
}

// The SVG made a Direct2D document, on its first draw. False: Direct2D can't draw it (old Windows, or not valid).
bool PvSvgOpen() {
    g_pv.svgTried = true;
    ComPtr<ID2D1DeviceContext5> dc;
    if (!g_pv.res || g_pv.res->svg.empty() || FAILED(g_rt.As(&dc))) return false;
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(g_pv.res->svg.data()), static_cast<UINT>(g_pv.res->svg.size())));
    ComPtr<ID2D1SvgDocument> doc;
    if (!st || FAILED(dc->CreateSvgDocument(st.Get(), D2D1::SizeF(100, 100), &doc)) || !doc) return false;
    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    if (!root) return false;
    // its size: width and height, or the viewBox's; then it is made to fill whatever it is given
    float w = 0, h = 0;
    D2D1_SVG_LENGTH len{};
    if (SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &len, sizeof(len))) && len.units == D2D1_SVG_LENGTH_UNITS_NUMBER) w = len.value;
    if (SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &len, sizeof(len))) && len.units == D2D1_SVG_LENGTH_UNITS_NUMBER) h = len.value;
    D2D1_SVG_VIEWBOX vb{};
    const bool hasVb = SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) && vb.width > 0 && vb.height > 0;
    if (!hasVb) {
        vb = {0, 0, w > 0 ? w : 300.f, h > 0 ? h : 150.f};
        root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb));
    }
    if (w <= 0 && h > 0) w = h * vb.width / vb.height;
    else if (h <= 0 && w > 0) h = w * vb.height / vb.width;
    else if (w <= 0) {
        w = vb.width;
        h = vb.height;
    }
    const D2D1_SVG_LENGTH full{100.f, D2D1_SVG_LENGTH_UNITS_PERCENTAGE};
    root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &full, sizeof(full));
    root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &full, sizeof(full));
    g_pv.svg = doc;
    g_pv.svgW = w;
    g_pv.svgH = h;
    g_pv.svgVp = {};
    return true;
}

// A "label ...... value" line in the info block.
void PvInfoLine(const wchar_t* label, const std::wstring& value, float x0, float x1, float& y) {
    Color(cFaint);
    Text(label, wcslen(label), g_fmtName.Get(), x0, y, x0 + 90, y + 20);
    Color(0xC9C9D1);
    Text(value, g_fmtRight.Get(), x0 + 90, y, x1, y + 20);
    y += 20;
}

void DrawPreview(float W, float H) {
    if (!g_preview) return;
    const float pw = PreviewW(), x0 = W - pw, y0 = kTabH + ToolH(), y1 = H - kStatus;
    Fill(D2D1::RectF(x0, y0, W, y1), cSide);
    // left edge: drag to change the width (double click: back to the default)
    Fill(D2D1::RectF(x0, y0, x0 + 1, y1), g_pvDrag || IsHover(HitKind::PreviewEdge, 0) ? cDim : cBorder);
    g_hits.push_back({D2D1::RectF(x0 - 3, y0, x0 + 4, y1), HitKind::PreviewEdge, 0});
    // Markdown rendered or as source changed (palette, settings): the same item again
    static bool mdShown = g_cfg.previewMarkdown;
    if (mdShown != g_cfg.previewMarkdown) {
        mdShown = g_cfg.previewMarkdown;
        g_pv.want.clear();
        g_pv.path = L"\x01";
    }
    // request the new item once the cursor stops
    std::wstring want = PreviewTarget();
    if (want != g_pv.want) {
        g_pv.want = want;
        SetTimer(g_hwnd, kTimerPreview, g_pv.path.empty() || g_pv.path == L"\x01" ? 1 : 60, nullptr);
    }
    const float pad = 14, L = x0 + pad, R = W - pad;
    Tab& t = T();
    if (t.sel < 0 || t.sel >= Count()) {
        Color(cFaint);
        Text(L"Nothing selected", g_fmtCenter.Get(), x0, y0, W, y0 + 80);
        return;
    }
    // Info (bottom): straight from the cursor row, without waiting for the content to load. Every value has a
    // fixed slot and the block a fixed height, so a value that arrives late doesn't move anything.
    const std::wstring rowPath = RowPath(t, t.sel);
    const bool rowMatches = Key(rowPath) == Key(g_pv.path);
    const Entry& e = EntryAt(t, t.sel);
    wchar_t buf[64];
    std::wstring modified;
    if (e.mtime) {
        FormatDate(e.mtime, buf, 64);
        modified = buf;
    }
    constexpr int kInfoRows = 5;
    std::pair<const wchar_t*, std::wstring> info[kInfoRows];
    if (e.isDir()) {
        std::wstring contains, size;
        auto list = g_cache.find(Key(rowPath));
        if (list != g_cache.end() && !list->second->error)
            contains = Thousands(static_cast<int>(list->second->entries.size())) + L" items";
        DirSize ds;
        if (DirSizeOf(Key(rowPath), ds) && (ds.done || ds.bytes)) {
            FormatSize(ds.bytes, buf, 64);
            size = std::wstring(buf) + (ds.done ? L"" : L"…") + L"   ·   " + Thousands(static_cast<int>(ds.files)) + L" files";
        }
        info[0] = {L"Type", IsServerPath(rowPath) ? L"Computer" : IsSharePath(rowPath) ? L"Shared folder" : L"Folder"};
        info[1] = {L"Modified", modified};
        info[2] = {L"Contains", contains};
        info[3] = {L"Size", size};
    } else {
        std::wstring type = rowMatches && g_pv.res ? g_pv.res->typeName : L"";
        if (type.empty()) {
            auto known = g_pvTypes.find(PvExtKey(rowPath));
            if (known != g_pvTypes.end()) type = known->second;
        }
        FormatSize(e.size, buf, 64);
        info[0] = {L"Type", type};
        info[1] = {L"Size", std::wstring(buf) + L"  (" + Thousands(static_cast<int>(std::min<uint64_t>(e.size, INT_MAX))) + L" bytes)"};
        info[2] = {IsBin(t.path) ? L"Deleted" : L"Modified", modified};
        if (IsBin(t.path)) info[3] = {L"From", ParentOf(BinItemOf(RowName(t, t.sel).c_str()).original)};
        else if (rowMatches && g_pv.res && g_pv.res->kind == 1 && !g_pv.res->thumb)
            info[3] = {L"Dimensions", std::to_wstring(g_pv.res->imgW) + L" × " + std::to_wstring(g_pv.res->imgH)};
        else if (rowMatches && g_pv.res && g_pv.res->kind == 5)
            info[3] = {L"Table", Thousands(static_cast<int>(g_pv.res->rows.size()) - 1) + (g_pv.res->truncated ? L"+" : L"") + L" rows  ·  " +
                                     Thousands(g_pv.res->cols) + L" columns"};
        else if (rowMatches && g_pv.res && g_pv.res->indented)
            info[3] = {L"Shown", L"Indented for reading"};
        else if (rowMatches && g_pv.res && g_pv.res->kind == 6)
            info[3] = {L"Shown", L"Rendered"};
        else if (rowMatches && g_pv.res && g_pv.res->kind == 7)
            info[3] = {L"Font", g_pv.res->fontName + L"  ·  " + g_pv.res->fontFace +
                                    (g_pv.res->fontFaces > 1 ? L"  ·  " + Thousands(g_pv.res->fontFaces) + L" faces" : L"")};
        else if (rowMatches && g_pv.res && g_pv.res->kind == 8 && g_pv.svg)
            info[3] = {L"Dimensions", ExifNum(g_pv.svgW, 1) + L" × " + ExifNum(g_pv.svgH, 1)};
        else if (OnlineOnly(e.attrs)) info[3] = {L"Storage", L"Online only"};
        else if (e.attrs & FILE_ATTRIBUTE_PINNED) info[3] = {L"Storage", L"Always on this PC"};
    }
    if (const auto* tags = TagsOf(rowPath)) {
        std::wstring all;
        for (const std::wstring& x : *tags) all += (all.empty() ? L"" : L", ") + x;
        info[4] = {L"Tags", all};
    }
    const float infoH = 34 + kInfoRows * 20 + 10;
    const float top = y0 + pad, bot = std::max(top + 60, y1 - infoH - 6);
    const D2D1_RECT_F box = D2D1::RectF(L, top, R, bot);
    g_pv.box = box;
    // content: what has loaded, which for a moment can still be the previous item
    g_rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    auto cached = g_pv.isDir ? g_cache.find(Key(g_pv.path)) : g_cache.end();
    if (g_pv.path.empty() || g_pv.path == L"\x01") {
        // nothing requested yet (it takes a frame)
    } else if (g_pv.isDir) {
        if (cached == g_cache.end()) {
            auto note = g_folderNotes.find(Key(g_pv.path));
            const bool why = note != g_folderNotes.end();
            Color(why ? cDim : cFaint);
            Text(why ? FolderWhyLong(note->second) : L"Loading…", g_fmtCenter.Get(), L, top, R, top + 60);
        } else {
            auto l = Prepared(cached->second);
            const float rh = 22;
            int first = static_cast<int>(g_pv.scroll / rh);
            if (l->entries.empty()) {
                Color(cFaint);
                Text(L"Empty folder", g_fmtCenter.Get(), L, top, R, top + 60);
            }
            for (int i = std::max(0, first); i < static_cast<int>(l->entries.size()); ++i) {
                float y = top + i * rh - g_pv.scroll;
                if (y > bot) break;
                const Entry& ce = l->entries[i];
                const wchar_t* nm = l->name(ce);
                DrawIcon(Join(l->path, nm), &ce, nm, ce.nameLen, false, L + 2, y + (rh - kIcon) / 2, 1.f);
                Color(0xC9C9D1);
                Text(nm, ce.nameLen, g_fmtName.Get(), L + 26, y, R, y + rh);
            }
        }
    } else if (g_pv.res && g_pv.res->kind == 1 && g_pv.res->bmp) {
        // a photo's EXIF lines go under it, and the image fits in what is left
        const auto& facts = g_pv.res->facts;
        const float factsH = facts.empty() ? 0.f : facts.size() * 20.f + 12;
        const D2D1_RECT_F ibox = D2D1::RectF(box.left, box.top, box.right, std::max(box.top + 40, box.bottom - factsH));
        if (!g_pv.d2d) g_rt->CreateBitmapFromWicBitmap(g_pv.res->bmp.Get(), nullptr, &g_pv.d2d);
        if (g_pv.d2d) {
            D2D1_SIZE_U px = g_pv.d2d->GetPixelSize();
            float scale = 96.f / g_dpi;  // pixels -> DIP
            float iw = px.width * scale, ih = px.height * scale;
            float bw = ibox.right - ibox.left, bh = ibox.bottom - ibox.top;
            float k = std::min(bw / iw, bh / ih);
            if (g_pv.res->thumb || k > 1) k = std::min(k, 1.f);  // don't enlarge
            float dw = iw * k, dh = ih * k;
            const float snap = g_dpi / 96.f;  // whole pixels: at its real size it isn't resampled
            float dx = std::round((ibox.left + (bw - dw) / 2) * snap) / snap;
            float dy = std::round((ibox.top + (bh - dh) / 2) * snap) / snap;
            g_rt->DrawBitmap(g_pv.d2d.Get(), D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        float fy = ibox.bottom + 12;
        for (const auto& [label, value] : facts) PvInfoLine(label.c_str(), value, L, R, fy);
    } else if (g_pv.res && g_pv.res->kind == 3) {
        const float mid = (top + bot) / 2;
        Color(cFaint);
        Glyph(gCloud, g_fmtIcon.Get(), L, mid - 44, R, mid - 18);
        Color(cDim);
        Text(L"Online only", g_fmtCenter.Get(), L, mid - 18, R, mid + 2);
        Color(cFaint);
        Text(L"Open it to download it", g_fmtCenter.Get(), L, mid + 2, R, mid + 22);
    } else if (g_pv.res && g_pv.res->kind == 6 && g_pv.res->md) {
        MdDoc& d = *g_pv.res->md;
        const float mr = R - 4;  // room for the text's last letter in italics
        if (d.width != mr - L || d.fonts != g_fmtName.Get()) MdLayout(d, mr - L);
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, d.height - (bot - top)));
        if (d.blocks.empty()) {
            Color(cFaint);
            Text(L"Empty file", g_fmtCenter.Get(), L, top, R, top + 60);
        }
        MdDraw(d, L, mr, top - g_pv.scroll, top, bot);
    } else if (g_pv.res && g_pv.res->kind == 7 && g_pv.res->fontFmt.size() >= 7) {
        // a font: its name in itself, the alphabet, then a sentence from small to big
        struct FontLine {
            int fmt;
            const wchar_t* text;
            const wchar_t* size;
        };
        static const FontLine lines[] = {
            {0, nullptr, nullptr},
            {1, L"ABCDEFGHIJKLMNOPQRSTUVWXYZ", nullptr},
            {1, L"abcdefghijklmnopqrstuvwxyz", nullptr},
            {1, L"0123456789 .,:;!?&@#%$€()", nullptr},
            {2, L"The quick brown fox jumps over the lazy dog", L"12"},
            {3, L"The quick brown fox jumps over the lazy dog", L"16"},
            {4, L"The quick brown fox jumps over the lazy dog", L"22"},
            {5, L"The quick brown fox jumps over the lazy dog", L"30"},
            {6, L"The quick brown fox jumps over the lazy dog", L"40"},
        };
        float y = top + 2 - g_pv.scroll;
        for (size_t li = 0; li < std::size(lines); ++li) {
            const FontLine& fl = lines[li];
            const std::wstring text = fl.text ? fl.text : g_pv.res->fontName;
            const float x = L + (fl.size ? 24.f : 0.f);
            ComPtr<IDWriteTextLayout> lay;
            if (FAILED(g_dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), g_pv.res->fontFmt[fl.fmt].Get(),
                                              std::max(20.f, R - x), 10000.f, &lay)))
                continue;
            DWRITE_TEXT_METRICS m{};
            lay->GetMetrics(&m);
            if (y + m.height >= top && y <= bot) {
                if (fl.size) {
                    const float first = m.height / std::max<UINT32>(1, m.lineCount);
                    Color(cFaint);
                    Text(fl.size, wcslen(fl.size), g_fmtName.Get(), L, y, x, y + first);
                }
                Color(li == 0 ? cText : 0xD4D4DA);
                g_rt->DrawTextLayout(D2D1::Point2F(x, y), lay.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            y += m.height + (li == 0 ? 6.f : 4.f);
            if (li == 0 || li == 3) {
                HLine(L, R, y + 4);
                y += 14;
            }
        }
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, y + g_pv.scroll - bot));
    } else if (g_pv.res && g_pv.res->kind == 8) {
        // SVG: fit to the box (it is vector: as big as it fits), on a light checkerboard
        if (!g_pv.svg && !g_pv.svgTried && !PvSvgOpen()) {  // Direct2D can't draw it: its source, as text
            g_pv.res->kind = 2;
            PvTextLines(*g_pv.res);
            Invalidate();
        }
        ComPtr<ID2D1DeviceContext5> dc;
        if (g_pv.svg && g_pv.svgW > 0 && g_pv.svgH > 0 && SUCCEEDED(g_rt.As(&dc))) {
            const float bw = box.right - box.left, bh = box.bottom - box.top;
            const float k = std::min(bw / g_pv.svgW, bh / g_pv.svgH);
            const float dw = std::max(1.f, std::floor(g_pv.svgW * k)), dh = std::max(1.f, std::floor(g_pv.svgH * k));
            const float dx = std::round(box.left + (bw - dw) / 2), dy = std::round(box.top + (bh - dh) / 2);
            if (g_pv.svgVp.width != dw || g_pv.svgVp.height != dh) {
                g_pv.svgVp = D2D1::SizeF(dw, dh);
                g_pv.svg->SetViewportSize(g_pv.svgVp);
            }
            if (ID2D1BitmapBrush* chk = CheckerBrush()) {
                chk->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy));
                g_rt->FillRectangle(D2D1::RectF(dx, dy, dx + dw, dy + dh), chk);
            }
            D2D1_MATRIX_3X2_F old;
            g_rt->GetTransform(&old);
            g_rt->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * old);
            dc->DrawSvgDocument(g_pv.svg.Get());
            g_rt->SetTransform(old);
        }
    } else if (g_pv.res && g_pv.res->kind == 4) {
        // hex: offset, the bytes (zeros faint) and the same as characters; 16, 8 or 4 a line, whatever fits
        const std::vector<uint8_t>& b = g_pv.res->bytes;
        const float lh = 16, cw = MonoCharW();
        const size_t n = b.size();
        int od = 4;  // digits of the offsets
        while (od < 8 && n > (1ull << (od * 4))) ++od;
        auto charsFor = [&](int bpl) { return od + 2 + bpl * 3 + (bpl == 16 ? 1 : 0) + 1 + bpl; };
        int bpl = 16;
        while (bpl > 4 && charsFor(bpl) * cw > R - L) bpl /= 2;
        const int lines = static_cast<int>((n + bpl - 1) / bpl);
        const float total = (lines + (g_pv.res->truncated ? 2 : 0)) * lh;
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, total - (bot - top)));
        if (!n) {
            Color(cFaint);
            Text(L"Empty file", g_fmtCenter.Get(), L, top, R, top + 60);
        }
        std::vector<std::pair<UINT32, UINT32>> faint;  // (start, length) in the line
        for (int i = std::max(0, static_cast<int>(g_pv.scroll / lh)); i < lines; ++i) {
            const float y = top + i * lh - g_pv.scroll;
            if (y > bot) break;
            const size_t off = static_cast<size_t>(i) * bpl;
            wchar_t line[96];
            int k = swprintf(line, 96, L"%0*llX  ", od, static_cast<unsigned long long>(off));
            faint.clear();
            for (int j = 0; j < bpl; ++j) {
                if (bpl == 16 && j == 8) line[k++] = L' ';
                if (off + j < n) {
                    if (!b[off + j]) faint.push_back({static_cast<UINT32>(k), 2});
                    k += swprintf(line + k, 96 - k, L"%02X ", b[off + j]);
                } else {
                    line[k++] = L' ';
                    line[k++] = L' ';
                    line[k++] = L' ';
                }
            }
            line[k++] = L' ';
            const int textAt = k;
            for (int j = 0; j < bpl && off + j < n; ++j) {
                const uint8_t c = b[off + j];
                const bool shows = c >= 0x20 && c < 0x7F;
                if (!shows) faint.push_back({static_cast<UINT32>(k), 1});
                line[k++] = shows ? static_cast<wchar_t>(c) : L'·';
            }
            ComPtr<IDWriteTextLayout> lay;
            if (SUCCEEDED(g_dw->CreateTextLayout(line, static_cast<UINT32>(k), g_fmtMono.Get(), 4000, lh, &lay))) {
                lay->SetDrawingEffect(RunBrush(cDim), DWRITE_TEXT_RANGE{static_cast<UINT32>(textAt), static_cast<UINT32>(k - textAt)});
                lay->SetDrawingEffect(RunBrush(cFaint), DWRITE_TEXT_RANGE{0, static_cast<UINT32>(od)});
                for (auto [at, len] : faint) lay->SetDrawingEffect(RunBrush(cFaint), DWRITE_TEXT_RANGE{at, len});
                Color(0xD4D4DA);
                g_rt->DrawTextLayout(D2D1::Point2F(L, y), lay.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (g_pv.res->truncated) {
            const float y = top + (lines + 1) * lh - g_pv.scroll;
            wchar_t a[32], all[32];
            FormatSize(n, a, 32);
            FormatSize(g_pv.res->fileSize, all, 32);
            Color(cFaint);
            Text(std::wstring(L"The first ") + a + L" of " + all, g_fmtName.Get(), L, y, R, y + lh);
        }
    } else if (g_pv.res && g_pv.res->kind == 5) {
        // a table: the header stays on top, row numbers on the left, Shift+wheel moves it sideways
        const auto& rows = g_pv.res->rows;
        const int nr = static_cast<int>(rows.size()), nc = static_cast<int>(g_pv.colW.size());
        const float rh = 22;
        const float tl = L + std::to_wstring(std::max(1, nr - 1)).size() * 7.f + 12;  // where the cells start
        float tw = 0;
        for (float w : g_pv.colW) tw += w;
        g_pv.scrollX = std::clamp(g_pv.scrollX, 0.f, std::max(0.f, tw - (R - tl)));
        const float dataTop = top + rh, total = (nr - 1) * rh;
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, total - (bot - dataTop)));
        const float tableBot = std::min(bot, dataTop + total - g_pv.scroll);
        auto cells = [&](const std::vector<std::wstring>& row, float y, bool head) {
            g_rt->PushAxisAlignedClip(D2D1::RectF(tl, y, R, y + rh), D2D1_ANTIALIAS_MODE_ALIASED);
            float x = tl - g_pv.scrollX;
            for (int c = 0; c < nc && x < R; x += g_pv.colW[c], ++c) {
                if (x + g_pv.colW[c] <= tl || c >= static_cast<int>(row.size()) || row[c].empty()) continue;
                const float l = x + 8, r = x + g_pv.colW[c] - 8;
                if (head) {
                    const float at = g_pv.colNum[c] ? std::max(l, r - MeasureW(row[c], g_fmtNameBold.Get())) : l;
                    Color(cText);
                    Text(row[c], g_fmtNameBold.Get(), at, y, r, y + rh);
                } else {
                    Color(0xC9C9D1);
                    Text(row[c], g_pv.colNum[c] ? g_fmtRight.Get() : g_fmtName.Get(), l, y, r, y + rh);
                }
            }
            g_rt->PopAxisAlignedClip();
        };
        g_rt->PushAxisAlignedClip(D2D1::RectF(L, dataTop, R, bot), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = std::max(1, 1 + static_cast<int>(g_pv.scroll / rh)); i < nr; ++i) {
            const float y = dataTop + (i - 1) * rh - g_pv.scroll;
            if (y > bot) break;
            if (i % 2 == 0) Fill(D2D1::RectF(L, y, R, y + rh), 0xFFFFFF, 0.025f);
            wchar_t num[16];
            swprintf(num, 16, L"%d", i);
            Color(cFaint);
            Text(num, wcslen(num), g_fmtRight.Get(), L, y, tl - 8, y + rh);
            cells(rows[i], y, false);
        }
        g_rt->PopAxisAlignedClip();
        cells(rows[0], top, true);
        HLine(L, R, dataTop - 1);
        float x = tl - g_pv.scrollX;
        for (int c = 0; c < nc && x < R; ++c) {  // the lines between columns
            x += g_pv.colW[c];
            if (x > tl && x < R) Fill(D2D1::RectF(x, top, x + 1, tableBot), cBorder);
        }
        if (tw > R - tl) {  // how far sideways it is
            const float track = R - tl, len = std::max(24.f, track * track / tw);
            const float at = tl + (track - len) * (g_pv.scrollX / (tw - track));
            Round(D2D1::RectF(at, bot - 4, at + len, bot - 1), 1.5f, 0xFFFFFF, 0.2f);
        }
    } else if (g_pv.res && g_pv.res->kind == 2) {
        const float lh = 16;
        const std::wstring& s = g_pv.res->text;
        const int nl = static_cast<int>(g_pv.lines.size());
        const int digits = static_cast<int>(std::to_wstring(std::max(1, nl)).size());
        const float gutter = digits * 7.5f + 10;
        // wrapping: the mono font makes a line's rows len / columns, so the scroll stays in pixels
        const float charW = MonoCharW();
        const bool wrap = g_cfg.previewWrap;
        const int cols = std::max(8, static_cast<int>((R - 6 - L - gutter) / charW));
        const float layW = wrap ? cols * charW + 0.5f : 20000.f;
        auto rowsOf = [&](int i) { return wrap ? std::max(1, (static_cast<int>(std::min<size_t>(g_pv.lines[i].second, 4000)) + cols - 1) / cols) : 1; };
        float total = 0;
        for (int i = 0; i < nl; ++i) total += rowsOf(i) * lh;
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, total - (bot - top)));
        if (s.empty()) {
            Color(cFaint);
            Text(L"Empty file", g_fmtCenter.Get(), L, top, R, top + 60);
        }
        // one brush per token class, for this render target
        static ComPtr<ID2D1SolidColorBrush> tokBrush[11];
        static ID2D1RenderTarget* tokRt = nullptr;
        if (tokRt != g_rt.Get()) {
            for (auto& b : tokBrush) b.Reset();
            tokRt = g_rt.Get();
        }
        auto brushFor = [&](Tok t) -> ID2D1SolidColorBrush* {
            auto& b = tokBrush[static_cast<int>(t)];
            if (!b) g_rt->CreateSolidColorBrush(D2D1::ColorF(TokColor(t)), &b);
            return b.Get();
        };
        float y = top - g_pv.scroll;
        int i = 0;
        for (; i < nl && y + rowsOf(i) * lh <= top; ++i) y += rowsOf(i) * lh;  // skip what is above
        for (; i < nl; ++i) {
            const float rows = static_cast<float>(rowsOf(i));
            if (y > bot) break;
            wchar_t num[16];
            swprintf(num, 16, L"%d", i + 1);
            Color(cFaint);
            Text(num, wcslen(num), g_fmtMonoR.Get(), L, y, L + gutter - 10, y + lh);
            auto [off, len] = g_pv.lines[i];
            len = std::min<size_t>(len, 4000);
            ComPtr<IDWriteTextLayout> lay;
            if (len && SUCCEEDED(g_dw->CreateTextLayout(s.c_str() + off, static_cast<UINT32>(len), wrap ? g_fmtMonoWrap.Get() : g_fmtMono.Get(), layW, rows * lh, &lay))) {
                // the spans touching this line (they are sorted by start; one may begin before it)
                auto it = std::lower_bound(g_pv.spans.begin(), g_pv.spans.end(), off, [](const Span& sp, size_t o) { return sp.start + sp.len <= o; });
                for (; it != g_pv.spans.end() && it->start < off + len; ++it) {
                    const size_t a = std::max<size_t>(it->start, off), b = std::min<size_t>(it->start + it->len, off + len);
                    if (b <= a) continue;
                    const DWRITE_TEXT_RANGE range{static_cast<UINT32>(a - off), static_cast<UINT32>(b - a)};
                    lay->SetDrawingEffect(brushFor(it->cls), range);
                    if (it->cls == Tok::Heading) lay->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
                }
                Color(0xD4D4DA);
                g_rt->DrawTextLayout(D2D1::Point2F(L + gutter, y), lay.Get(), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            y += rows * lh;
        }
    } else if (g_pv.loading) {
        // no flicker: "loading" isn't shown (it almost always takes less than a frame)
    } else {
        DrawIcon(g_pv.path, nullptr, nullptr, 0, false, (L + R) / 2 - kIcon / 2, top + 30, 1.f);
    }
    g_rt->PopAxisAlignedClip();
    // info
    float y = bot + 10;
    HLine(L, R, y - 4);
    const std::wstring name = RowName(t, t.sel);
    Color(cText);
    Text(name, g_fmtNameBold.Get(), L, y, R, y + 26);
    y += 30;
    for (auto& [label, value] : info) {
        if (value.empty()) {
            y += 20;
        } else if (wcscmp(label, L"Tags") == 0) {  // the tags as labels, not text
            Color(cFaint);
            Text(label, wcslen(label), g_fmtName.Get(), L, y, L + 90, y + 20);
            float xe;
            DrawTagLabels(rowPath, L + 90, y + 10, R, 1.f, xe);
            y += 20;
        } else {
            PvInfoLine(label, value, L, R, y);
        }
    }
}

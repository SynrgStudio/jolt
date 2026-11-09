// Preview (Space): right-hand column showing what's under the cursor.
// Images via WIC (scaled and rotated per EXIF, with the camera's data under photos), SVG (Direct2D), fonts (a sample
// in the typeface itself), Markdown rendered (markdown.inl), text/code (minified JSON indented), CSV as a table,
// what an archive holds (zip read here, 7z, rar, tar and the like through Windows' tar.exe), songs and videos that
// play (media.inl), folder contents (from the listing's memory), the Shell thumbnail (PDF, Office...) and, for
// binaries Windows has nothing to show for, their bytes in hex. It all loads on a separate thread and only once the cursor
// stops: moving through the list doesn't slow down.
// Included from app.cpp after palette.inl.

constexpr UINT WM_APP_PREVIEW = WM_APP + 6;
constexpr UINT_PTR kTimerPreview = 6;
constexpr UINT_PTR kTimerHold = 18;  // the preview's key held long enough: the floating preview

// The floating preview: the preview's key (Space) held shows a big preview over the list and letting go hides it;
// a tap still opens or closes the panel. Which one it was is known on letting go, or once the key is held
// [general] preview_hold milliseconds (150 by default, 50 at least).
bool g_pvFloat = false;
UINT g_pvHoldKey = 0;  // the preview's key, while it is down

// A font loaded from memory gets a loader of its own, unregistered once the preview moves on: the file is never
// held open (it can be renamed or deleted while it shows) and nothing builds up.
struct PvFontHold {
    ComPtr<IDWriteFactory5> f5;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ~PvFontHold() {
        if (f5 && loader) f5->UnregisterFontFileLoader(loader.Get());
    }
};

// One thing inside an archive, in tree order.
struct ArcItem {
    std::wstring path;  // "folder/file.txt", folders without the slash
    bool dir = false;
    uint64_t size = 0;
    std::wstring name;
    int depth = 0;
};

struct PreviewResult {
    uint64_t gen = 0;
    std::wstring path;
    int kind = 0;  // 0 nothing, 1 image, 2 text, 3 not on this PC, 4 hex (a binary), 5 table (CSV), 6 Markdown, 7 font, 8 SVG,
                   // 9 a song or a video, 10 an archive's contents
    ComPtr<IWICBitmap> bmp;
    UINT imgW = 0, imgH = 0;  // original size (already rotated)
    bool thumb = false;       // Shell thumbnail or icon (not enlarged)
    bool alpha = false;       // some of it is see-through: a checkerboard goes behind
    bool full = false;        // decoded at its whole size (asked for when zooming in past the first decode)
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
    int media = 0;  // 1 a song, 2 a video (bmp: its cover or a frame, if Windows has one)
    std::wstring mTitle, mArtist, mAlbum;
    uint64_t mDur = 0;  // 100 ns units
    std::vector<ArcItem> arc;
    int arcFiles = 0, arcDirs = 0;
    uint64_t arcBytes = 0;
    bool arcLocked = false;  // some of it is encrypted
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
    // zooming, panning and turning the picture (an image or the SVG); back to fitting with each new item
    float zoom = 0;            // screen DIPs per picture DIP; 0: as it fits
    float panX = 0, panY = 0;  // the picture's center, from the box's
    int rot = 0;               // quarter turns to the right
    bool fullAsked = false;
    bool panning = false;
    float panFromX = 0, panFromY = 0, panX0 = 0, panY0 = 0;
    D2D1_RECT_F imgBox{};      // where the picture goes (under it, a photo's EXIF lines)
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
UINT g_pvReqFull = 0;   // an image again, at this many pixels a side at most (0: the usual 2048)

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

// --- archives: what they hold, as a tree.
// 1: a zip (read here), 2: something Windows' tar.exe (libarchive) reads; 0: neither.
int PvArchiveKind(const std::wstring& path, const std::wstring& ext) {
    static const wchar_t* zips[] = {L".zip", L".jar", L".war", L".ear", L".apk", L".aab", L".ipa", L".nupkg", L".vsix", L".whl",
                                    L".xpi", L".appx", L".msix", L".appxbundle", L".msixbundle", L".cbz", L".epub", L".kmz"};
    static const wchar_t* tars[] = {L".7z", L".rar", L".tar", L".tgz", L".tbz", L".tbz2", L".txz", L".tzst", L".cab", L".iso", L".cpio",
                                    L".xar", L".lha", L".lzh", L".cbr", L".cb7"};
    for (const wchar_t* z : zips)
        if (ext == z) return 1;
    for (const wchar_t* t : tars)
        if (ext == t) return 2;
    const std::wstring low = Lower(path);
    for (const wchar_t* s : {L".tar.gz", L".tar.bz2", L".tar.xz", L".tar.zst", L".tar.lz", L".tar.lzma"})
        if (low.size() > wcslen(s) && low.compare(low.size() - wcslen(s), wcslen(s), s) == 0) return 2;
    return 0;
}

// A zip's central directory: every name and its unpacked size (zip64 too). Data before the zip (self-extractors) is fine.
bool PvZipList(const std::wstring& path, std::vector<std::pair<std::wstring, uint64_t>>& out, bool& locked) {
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](uint64_t at, void* buf, DWORD n) {
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, n, &got, &o) && got == n;
    };
    auto u16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; };
    auto u32 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; };
    auto u64 = [&](const uint8_t* p) { return static_cast<uint64_t>(u32(p)) | static_cast<uint64_t>(u32(p + 4)) << 32; };
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    const uint64_t size = static_cast<uint64_t>(sz.QuadPart);
    bool ok = false;
    do {
        if (size < 22) break;
        const DWORD tail = static_cast<DWORD>(std::min<uint64_t>(size, 65535 + 22));  // the end record and its comment
        std::vector<uint8_t> t(tail);
        if (!readAt(size - tail, t.data(), tail)) break;
        size_t e = std::wstring::npos;
        for (size_t k = tail - 22 + 1; k-- > 0;)
            if (u32(&t[k]) == 0x06054b50) {
                e = k;
                break;
            }
        if (e == std::wstring::npos) break;
        uint64_t cdSize = u32(&t[e + 12]), end = size - tail + e;
        if ((u16(&t[e + 10]) == 0xFFFF || cdSize == 0xFFFFFFFF || u32(&t[e + 16]) == 0xFFFFFFFF) && e >= 20 && u32(&t[e - 20]) == 0x07064b50) {
            const uint64_t at = u64(&t[e - 20 + 8]);
            uint8_t z[56];
            if (readAt(at, z, 56) && u32(z) == 0x06064b50) {
                cdSize = u64(z + 40);
                end = at;
            }
        }
        if (cdSize > end || cdSize > (64ull << 20)) break;
        std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
        if (cdSize && !readAt(end - cdSize, cd.data(), static_cast<DWORD>(cdSize))) break;
        size_t p = 0;
        while (p + 46 <= cd.size() && u32(&cd[p]) == 0x02014b50 && out.size() < 20000) {
            const uint32_t flags = u16(&cd[p + 8]);
            uint64_t usize = u32(&cd[p + 24]);
            const size_t nlen = u16(&cd[p + 28]), xlen = u16(&cd[p + 30]), clen = u16(&cd[p + 32]);
            if (p + 46 + nlen + xlen > cd.size()) break;
            if (usize == 0xFFFFFFFF)  // zip64: the size is in the extra field
                for (size_t x = p + 46 + nlen; x + 12 <= p + 46 + nlen + xlen;) {
                    const uint32_t id = u16(&cd[x]), len = u16(&cd[x + 2]);
                    if (id == 1 && len >= 8) {
                        usize = u64(&cd[x + 4]);
                        break;
                    }
                    x += 4 + len;
                }
            // names: UTF-8 when flagged (and often when not), else the old DOS code page
            const char* name = reinterpret_cast<const char*>(&cd[p + 46]);
            UINT cp = CP_UTF8;
            int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, name, static_cast<int>(nlen), nullptr, 0);
            if (n <= 0 && !(flags & 0x800)) {
                cp = 437;
                n = MultiByteToWideChar(cp, 0, name, static_cast<int>(nlen), nullptr, 0);
            }
            std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
            if (n > 0) MultiByteToWideChar(cp, 0, name, static_cast<int>(nlen), w.data(), n);
            if (flags & 1) locked = true;
            out.push_back({std::move(w), usize});
            p += 46 + nlen + xlen + clen;
        }
        ok = true;
    } while (false);
    CloseHandle(h);
    return ok;
}

// The rest through Windows' tar.exe, which reads 7z, rar, tar (and gz, bz2, xz, zstd), cab, iso...: "tar -tvf".
bool PvTarList(const std::wstring& path, std::vector<std::pair<std::wstring, uint64_t>>& out) {
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    const std::wstring exe = std::wstring(sys) + L"\\tar.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring cmd = L"\"" + exe + L"\" -tvf \"" + path + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        return false;
    }
    std::string text;
    const ULONGLONG deadline = GetTickCount64() + 5000;  // a huge archive doesn't hold the preview up
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // closed and empty: tar is done
        if (avail) {
            char buf[65536];
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
            text.append(buf, got);
            if (text.size() > (8u << 20)) break;
            continue;
        }
        WaitForSingleObject(pi.hProcess, 10);
        if (GetTickCount64() > deadline) break;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    if (text.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    const UINT cp = n > 0 ? CP_UTF8 : CP_ACP;
    if (n <= 0) n = MultiByteToWideChar(cp, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring all(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, text.data(), static_cast<int>(text.size()), all.data(), n);
    // "drwxr-xr-x  0 user group  1234 Nov  8 23:40 folder/name": mode, links, owner, group, size, date (3), the name
    for (size_t p = 0; p < all.size() && out.size() < 20000;) {
        size_t nl = all.find(L'\n', p);
        if (nl == std::wstring::npos) nl = all.size();
        std::wstring line = all.substr(p, nl - p);
        p = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t q = 0;
        std::wstring field[8];
        bool whole = true;
        for (std::wstring& f : field) {
            while (q < line.size() && line[q] == L' ') ++q;
            const size_t s = q;
            while (q < line.size() && line[q] != L' ') ++q;
            f = line.substr(s, q - s);
            if (f.empty()) whole = false;
        }
        if (!whole || field[0].size() < 10 || !iswdigit(field[4][0])) continue;
        while (q < line.size() && line[q] == L' ') ++q;
        std::wstring name = line.substr(std::min(q, line.size()));
        if (field[0][0] == L'l') name = name.substr(0, name.find(L" -> "));
        if (field[0][0] == L'h') name = name.substr(0, name.find(L" link to "));
        if (name.empty()) continue;
        if (field[0][0] == L'd' && name.back() != L'/') name += L'/';
        out.push_back({std::move(name), _wcstoui64(field[4].c_str(), nullptr, 10)});
    }
    return !out.empty() || code == 0;
}

bool PvArchive(const std::wstring& path, int how, PreviewResult& r) {
    std::vector<std::pair<std::wstring, uint64_t>> raw;  // inner paths ("folder/" for folders), unpacked sizes
    if (!(how == 1 ? PvZipList(path, raw, r.arcLocked) : PvTarList(path, raw))) return false;
    std::unordered_map<std::wstring, size_t> seen;  // lowercase path -> its place in r.arc
    auto add = [&](const std::wstring& p, bool dir, uint64_t size) {
        for (size_t slash = p.find(L'/'); slash != std::wstring::npos; slash = p.find(L'/', slash + 1)) {  // its folders, even unlisted
            const std::wstring d = p.substr(0, slash), k = Lower(d);
            if (!d.empty() && !seen.count(k)) {
                seen[k] = r.arc.size();
                r.arc.push_back({d, true});
            }
        }
        if (p.empty()) return;
        auto it = seen.find(Lower(p));
        if (it != seen.end()) return;
        seen[Lower(p)] = r.arc.size();
        r.arc.push_back({p, dir, size});
    };
    for (auto& [p0, size] : raw) {
        std::wstring p = p0;
        for (wchar_t& c : p)
            if (c == L'\\') c = L'/';
        while (p.rfind(L"./", 0) == 0) p.erase(0, 2);
        while (!p.empty() && p[0] == L'/') p.erase(0, 1);
        const bool dir = !p.empty() && p.back() == L'/';
        while (!p.empty() && p.back() == L'/') p.pop_back();
        add(p, dir, size);
    }
    for (ArcItem& a : r.arc) {
        a.depth = static_cast<int>(std::count(a.path.begin(), a.path.end(), L'/'));
        a.name = a.path.substr(a.path.rfind(L'/') + 1);
        if (a.dir) ++r.arcDirs;
        else {
            ++r.arcFiles;
            r.arcBytes += a.size;
        }
    }
    // tree order: each folder before what it holds, folders before files, by name
    std::sort(r.arc.begin(), r.arc.end(), [](const ArcItem& a, const ArcItem& b) {
        size_t i = 0, j = 0;
        for (;;) {
            const size_t ie = a.path.find(L'/', i), je = b.path.find(L'/', j);
            const size_t ae = ie == std::wstring::npos ? a.path.size() : ie, be = je == std::wstring::npos ? b.path.size() : je;
            const bool aDir = ie != std::wstring::npos || a.dir, bDir = je != std::wstring::npos || b.dir;
            const int c = CompareStringOrdinal(a.path.c_str() + i, static_cast<int>(ae - i), b.path.c_str() + j, static_cast<int>(be - j), TRUE);
            if (c != CSTR_EQUAL) return aDir != bDir ? aDir : c == CSTR_LESS_THAN;
            if (ie == std::wstring::npos || je == std::wstring::npos) return ie == std::wstring::npos && je != std::wstring::npos;
            i = ie + 1;
            j = je + 1;
        }
    });
    r.kind = 10;
    return true;
}

// --- songs and videos: the cover or a frame Windows has, title, artist, album, length. They play in media.inl.
bool PvThumb(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r, bool* icon);  // below
int PvMediaKind(const std::wstring& ext) {  // 1 a song, 2 a video, by the registry's PerceivedType; any thread
    PERCEIVED t = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG f = 0;
    if (ext.empty() || FAILED(AssocGetPerceivedType(ext.c_str(), &t, &f, nullptr))) return 0;
    return t == PERCEIVED_TYPE_AUDIO ? 1 : t == PERCEIVED_TYPE_VIDEO ? 2 : 0;
}
std::wstring PvPropText(IPropertyStore* ps, const PROPERTYKEY& k) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(ps->GetValue(k, &v))) {
        if (v.vt == VT_LPWSTR && v.pwszVal) s = v.pwszVal;
        else if (v.vt == (VT_VECTOR | VT_LPWSTR))
            for (ULONG i = 0; i < v.calpwstr.cElems; ++i) s += (i ? L", " : L"") + std::wstring(v.calpwstr.pElems[i] ? v.calpwstr.pElems[i] : L"");
    }
    PropVariantClear(&v);
    return s;
}
bool PvMedia(IWICImagingFactory* wic, const std::wstring& path, int media, PreviewResult& r) {
    bool icon = true;
    if (wic && PvThumb(wic, path, r, &icon) && icon) r.bmp.Reset();  // only a real picture: a cover, a frame
    r.thumb = true;
    r.imgW = r.imgH = 0;  // the thumbnail's size isn't the video's
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps)))) {
        static const PROPERTYKEY kTitle = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
        static const GUID kMusic = {0x56A3372E, 0xCE9C, 0x11D2, {0x9F, 0x0E, 0x00, 0x60, 0x97, 0xC6, 0x86, 0xF6}};
        r.mTitle = PvPropText(ps.Get(), kTitle);
        r.mArtist = PvPropText(ps.Get(), PROPERTYKEY{kMusic, 2});
        r.mAlbum = PvPropText(ps.Get(), PROPERTYKEY{kMusic, 4});
        r.mDur = PropNumber(ps.Get(), kPkDuration);
        if (media == 2) {
            r.imgW = static_cast<UINT>(PropNumber(ps.Get(), kPkVideoW));
            r.imgH = static_cast<UINT>(PropNumber(ps.Get(), kPkVideoH));
        }
    }
    r.media = media;
    r.kind = 9;
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

bool PvImage(IWICImagingFactory* wic, const std::wstring& path, PreviewResult& r, UINT maxSide = 2048) {
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
    const double kMaxPixels = 48e6;  // the whole of a huge one is still too much memory
    if (w > maxSide || h > maxSide || static_cast<double>(w) * h > kMaxPixels) {
        const double s = std::min(static_cast<double>(maxSide) / std::max(w, h), std::sqrt(kMaxPixels / (static_cast<double>(w) * h)));
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
    // see-through: only in formats that can be, and only if some pixel is
    WICPixelFormatGUID pf{};
    ComPtr<IWICComponentInfo> ci;
    ComPtr<IWICPixelFormatInfo2> pfi;
    BOOL canBe = FALSE;
    if (SUCCEEDED(frame->GetPixelFormat(&pf)) && SUCCEEDED(wic->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci.As(&pfi))) pfi->SupportsTransparency(&canBe);
    if (canBe) {
        UINT bw = 0, bh = 0, size = 0, stride = 0;
        r.bmp->GetSize(&bw, &bh);
        const WICRect all{0, 0, static_cast<INT>(bw), static_cast<INT>(bh)};
        ComPtr<IWICBitmapLock> lock;
        BYTE* data = nullptr;
        if (SUCCEEDED(r.bmp->Lock(&all, WICBitmapLockRead, &lock)) && SUCCEEDED(lock->GetDataPointer(&size, &data)) && SUCCEEDED(lock->GetStride(&stride)))
            for (UINT y = 0; y < bh && !r.alpha; ++y)
                for (UINT x = 0; x < bw; ++x)
                    if (data[y * stride + x * 4 + 3] != 255) {
                        r.alpha = true;
                        break;
                    }
    }
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
        UINT full;
        {
            std::unique_lock lk(g_pvm);
            g_pvcv.wait(lk, [&] { return g_pvReqGen != seen; });
            path = g_pvReq;
            gen = seen = g_pvReqGen;
            md = g_pvReqMd;
            full = g_pvReqFull;
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
            bool done = wic && PvIsImage(ext) && PvImage(wic.Get(), path, *r, full ? full : 2048);
            r->full = full != 0;
            if (full) done = true;  // only the image: if it fails, what shows stays
            if (!done && PvIsFont(ext)) done = PvFont(path, ext, *r);
            if (!done && ext == L".svg") done = PvSvg(path, *r);
            if (const int arc = done ? 0 : PvArchiveKind(path, ext)) done = PvArchive(path, arc, *r);
            if (const int media = done ? 0 : PvMediaKind(ext)) done = PvMedia(wic.Get(), path, media, *r);
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
    if (!(g_preview || g_pvFloat) || g_pv.want == g_pv.path) return;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(PreviewThread).detach(); });
    g_pv.path = g_pv.want;
    if (g_media.path != g_pv.path) MediaClose();
    g_pv.res.reset();
    g_pv.d2d.Reset();
    g_pv.lines.clear();
    g_pv.scroll = 0;
    g_pv.scrollX = 0;
    g_pv.svg.Reset();
    g_pv.svgTried = false;
    g_pv.zoom = g_pv.panX = g_pv.panY = 0;
    g_pv.rot = 0;
    g_pv.fullAsked = g_pv.panning = false;
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
        g_pvReqFull = 0;
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

// Zoomed in past what was decoded: the same image again at its whole size (what shows stays meanwhile).
void PreviewLoadFull() {
    if (g_pv.fullAsked || !g_pv.res || g_pv.res->kind != 1 || g_pv.res->thumb || g_pv.res->full) return;
    g_pv.fullAsked = true;
    {
        std::lock_guard lk(g_pvm);
        g_pvReq = g_pv.path;
        g_pvReqFull = std::min<UINT>(8192, g_rt->GetMaximumBitmapSize());
        g_pv.gen = ++g_pvReqGen;
    }
    g_pvcv.notify_one();
}

void PreviewApply(PreviewResult* raw) {
    std::unique_ptr<PreviewResult> r(raw);
    if (r->gen != g_pv.gen || r->path != g_pv.path) return;
    if (r->full && r->kind != 1) return;  // the whole size failed: the first decode stays
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
    if (!g_preview) MediaClose();
    g_pv.want.clear();
    g_pv.path = L"\x01";  // forces a load
    for (auto& tp : g_tabs) ClampScroll(*tp);
    LayoutEdit();
    LayoutRename();
    Invalidate();
    TouchSession();
}

void PreviewHoldStart(UINT vk) {
    g_pvHoldKey = vk;
    SetTimer(g_hwnd, kTimerHold, static_cast<UINT>(g_cfg.previewHold), nullptr);
}
void PreviewFloatShow() {  // still held: up it comes
    if (!g_pvHoldKey || g_pvFloat) return;
    g_pvFloat = true;
    if (!g_preview) {  // what is under the cursor, now
        g_pv.want.clear();
        g_pv.path = L"\x01";
    }
    Invalidate();
}
// Letting go: the floating one goes, or it was a tap and the panel opens or closes. cancel: the keyboard went
// elsewhere (no letting go will come), nothing is toggled.
void PreviewHoldEnd(bool cancel) {
    if (!g_pvHoldKey) return;
    KillTimer(g_hwnd, kTimerHold);
    g_pvHoldKey = 0;
    if (g_pvFloat) {
        g_pvFloat = false;
        if (!g_preview) MediaClose();
        Invalidate();
    } else if (!cancel) {
        PreviewToggle();
    }
}

// Play or pause what the preview shows (a song or a video).
void PreviewPlay() {
    if (g_pv.res && g_pv.res->kind == 9 && Key(g_pv.res->path) == Key(g_pv.path)) MediaPlayPause(g_pv.res->path, g_pv.res->media == 2);
}

// An icon font at another size (the play button, the note over a song without a cover).
IDWriteTextFormat* PvIconFmt(float size) {
    static std::unordered_map<int, ComPtr<IDWriteTextFormat>> fmts;
    auto& f = fmts[static_cast<int>(size)];
    if (!f) f = MakeFormat(FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", size, DWRITE_TEXT_ALIGNMENT_CENTER,
                           DWRITE_FONT_WEIGHT_NORMAL, false);
    return f.Get();
}

// The preview's bitmap (an image, a cover, a frame) fitted into r, centered; enlarge: bigger than it is.
void PvDrawFit(const D2D1_RECT_F& r, bool enlarge) {
    if (!g_pv.d2d && g_pv.res && g_pv.res->bmp) g_rt->CreateBitmapFromWicBitmap(g_pv.res->bmp.Get(), nullptr, &g_pv.d2d);
    if (!g_pv.d2d) return;
    const D2D1_SIZE_U px = g_pv.d2d->GetPixelSize();
    const float iw = px.width * 96.f / g_dpi, ih = px.height * 96.f / g_dpi, bw = r.right - r.left, bh = r.bottom - r.top;
    float k = std::min(bw / iw, bh / ih);
    if (!enlarge) k = std::min(k, 1.f);
    const float dw = iw * k, dh = ih * k, x = std::round(r.left + (bw - dw) / 2), y = std::round(r.top + (bh - dh) / 2);
    g_rt->DrawBitmap(g_pv.d2d.Get(), D2D1::RectF(x, y, x + dw, y + dh), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// steps: notches of the wheel, up or left positive. Sideways only moves tables.
bool PvZoomable();
void PreviewWheel(float steps, bool sideways = false) {
    if (PvZoomable() && g_pv.zoom > 0) {  // a zoomed picture: the wheel moves it
        (sideways ? g_pv.panX : g_pv.panY) += steps * 48;
        Invalidate();
        return;
    }
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
bool g_pvCheckerDark = false;  // what the brush was made for ([general] preview_checker)
ID2D1BitmapBrush* CheckerBrush() {
    if (g_pvChecker && g_pvCheckerDark != g_cfg.checkerDark) g_pvChecker.Reset();
    if (!g_pvChecker) {
        g_pvCheckerDark = g_cfg.checkerDark;
        const uint32_t a = g_cfg.checkerDark ? 0xFF2C2C32 : 0xFFFFFFFF, b = g_cfg.checkerDark ? 0xFF3A3A41 : 0xFFE4E4E8;
        uint32_t px[16 * 16];
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) px[y * 16 + x] = (x / 8 + y / 8) % 2 ? b : a;
        ComPtr<ID2D1Bitmap> bmp;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(16, 16), px, 16 * 4, props, &bmp)))
            g_rt->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), &g_pvChecker);
    }
    return g_pvChecker.Get();
}

bool SvgNormalize(ID2D1SvgDocument* doc, float& w, float& h);

// The SVG made a Direct2D document, on its first draw. False: Direct2D can't draw it (old Windows, or not valid).
bool PvSvgOpen() {
    g_pv.svgTried = true;
    ComPtr<ID2D1DeviceContext5> dc;
    if (!g_pv.res || g_pv.res->svg.empty() || FAILED(g_rt.As(&dc))) return false;
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(g_pv.res->svg.data()), static_cast<UINT>(g_pv.res->svg.size())));
    ComPtr<ID2D1SvgDocument> doc;
    if (!st || FAILED(dc->CreateSvgDocument(st.Get(), D2D1::SizeF(100, 100), &doc)) || !doc) return false;
    float w = 0, h = 0;
    if (!SvgNormalize(doc.Get(), w, h)) return false;
    g_pv.svg = doc;
    g_pv.svgW = w;
    g_pv.svgH = h;
    g_pv.svgVp = {};
    return true;
}

// An SVG document's own size (width and height, or its viewBox's), then made to fill whatever viewport it gets.
bool SvgNormalize(ID2D1SvgDocument* doc, float& w, float& h) {
    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    if (!root) return false;
    w = h = 0;
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
    return w > 0 && h > 0;
}

// --- the picture (an image, or the SVG): zoomed with Ctrl+wheel around the mouse, moved by dragging or with the
// wheel once zoomed, a double click between fitting and its real size, turned a quarter at a time. All of it only
// in the preview: the file isn't touched.
bool PvZoomable() {
    return g_pv.res && ((g_pv.res->kind == 1 && !g_pv.res->thumb && g_pv.res->bmp && g_pv.res->imgW && g_pv.res->imgH) ||
                        (g_pv.res->kind == 8 && g_pv.svg && g_pv.svgW > 0 && g_pv.svgH > 0));
}
struct PvView {
    float baseW, baseH;  // unturned, at scale 1: an image's pixels at 1:1, an SVG's own units
    float k, fitK;       // the scale now, and the one that fits
    float cx, cy;        // its center on screen
    float w, h;          // on screen, turned
};
PvView PvViewOf(const D2D1_RECT_F& box) {
    PvView v{};
    const bool svg = g_pv.res->kind == 8;
    v.baseW = svg ? g_pv.svgW : g_pv.res->imgW * 96.f / g_dpi;
    v.baseH = svg ? g_pv.svgH : g_pv.res->imgH * 96.f / g_dpi;
    const bool turned = g_pv.rot % 2 != 0;
    const float tw = turned ? v.baseH : v.baseW, th = turned ? v.baseW : v.baseH;
    const float bw = box.right - box.left, bh = box.bottom - box.top;
    v.fitK = std::max(0.0001f, std::min(bw / tw, bh / th));
    if (!svg) v.fitK = std::min(v.fitK, 1.f);  // an image isn't enlarged to fit; an SVG is
    v.k = g_pv.zoom > 0 ? g_pv.zoom : v.fitK;
    v.w = tw * v.k;
    v.h = th * v.k;
    const float mx = std::max(0.f, (v.w - bw) / 2), my = std::max(0.f, (v.h - bh) / 2);  // it doesn't leave the box
    g_pv.panX = std::clamp(g_pv.panX, -mx, mx);
    g_pv.panY = std::clamp(g_pv.panY, -my, my);
    const float s = g_dpi / 96.f;  // its corner on a whole pixel: at 1:1 nothing is resampled
    const float left = std::round(((box.left + box.right) / 2 + g_pv.panX - v.w / 2) * s) / s;
    const float top = std::round(((box.top + box.bottom) / 2 + g_pv.panY - v.h / 2) * s) / s;
    v.cx = left + v.w / 2;
    v.cy = top + v.h / 2;
    return v;
}

// Zooms by f around (x, y); back to fitting when it gets there.
void PreviewZoomBy(float f, float x, float y) {
    if (!PvZoomable()) return;
    const D2D1_RECT_F box = g_pv.imgBox;
    const PvView v = PvViewOf(box);
    float most = 40;
    if (g_pv.res->kind == 8) most = std::max(v.fitK, std::min(most, 16384 / std::max(v.baseW, v.baseH)));  // an SVG's canvas has a limit
    const float k1 = std::clamp(v.k * f, v.fitK, std::max(v.fitK, most));
    if (k1 <= v.fitK * 1.0001f) {
        g_pv.zoom = g_pv.panX = g_pv.panY = 0;
    } else {
        const float ux = (x - v.cx) / v.k, uy = (y - v.cy) / v.k;  // the point under the mouse stays under it
        g_pv.panX = x - ux * k1 - (box.left + box.right) / 2;
        g_pv.panY = y - uy * k1 - (box.top + box.bottom) / 2;
        g_pv.zoom = k1;
    }
    Invalidate();
}
// Ctrl+wheel: true when it was over the picture.
bool PreviewZoomAt(float x, float y, float steps) {
    const D2D1_RECT_F& b = g_pv.imgBox;
    if (!g_preview || !PvZoomable() || x < b.left || x > b.right || y < b.top || y > b.bottom) return false;
    PreviewZoomBy(std::pow(1.25f, steps), x, y);
    return true;
}
// Fitting, or its real size there (twice the size if it already fits at its real size).
void PreviewZoomToggle(float x, float y) {
    if (!PvZoomable()) return;
    if (g_pv.zoom > 0) {
        g_pv.zoom = g_pv.panX = g_pv.panY = 0;
        Invalidate();
        return;
    }
    const PvView v = PvViewOf(g_pv.imgBox);
    PreviewZoomBy((v.fitK < 0.999f ? 1.f : v.k * 2) / v.k, x, y);
}
void PreviewZoomStep(float steps) {  // the palette: around the middle
    PreviewZoomBy(std::pow(1.25f, steps), (g_pv.imgBox.left + g_pv.imgBox.right) / 2, (g_pv.imgBox.top + g_pv.imgBox.bottom) / 2);
}
void PreviewRotate(int quarters) {
    if (!PvZoomable()) return;
    g_pv.rot = (g_pv.rot + quarters + 4) % 4;
    g_pv.zoom = g_pv.panX = g_pv.panY = 0;
    Invalidate();
}
bool PreviewPannable() {
    if (!PvZoomable()) return false;
    const PvView v = PvViewOf(g_pv.imgBox);
    return v.w > g_pv.imgBox.right - g_pv.imgBox.left + 0.5f || v.h > g_pv.imgBox.bottom - g_pv.imgBox.top + 0.5f;
}
void PreviewPanStart(float x, float y) {
    g_pv.panning = true;
    g_pv.panFromX = x;
    g_pv.panFromY = y;
    g_pv.panX0 = g_pv.panX;
    g_pv.panY0 = g_pv.panY;
}
void PreviewPanMove(float x, float y) {
    g_pv.panX = g_pv.panX0 + x - g_pv.panFromX;
    g_pv.panY = g_pv.panY0 + y - g_pv.panFromY;
    Invalidate();
}
void PreviewToolClick(int idx) {
    if (idx == 0) PreviewRotate(-1);
    else if (idx == 1) PreviewRotate(1);
    else PreviewZoomToggle((g_pv.imgBox.left + g_pv.imgBox.right) / 2, (g_pv.imgBox.top + g_pv.imgBox.bottom) / 2);
}

// The picture as zoomed, moved and turned, clipped to box; on a checkerboard if it can be see-through.
void PvDrawPicture(const D2D1_RECT_F& box) {
    g_pv.imgBox = box;
    const PvView v = PvViewOf(box);
    const bool svg = g_pv.res->kind == 8;
    g_rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    if (svg || g_pv.res->alpha)
        if (ID2D1BitmapBrush* chk = CheckerBrush()) {
            const D2D1_RECT_F r = D2D1::RectF(v.cx - v.w / 2, v.cy - v.h / 2, v.cx + v.w / 2, v.cy + v.h / 2);
            chk->SetTransform(D2D1::Matrix3x2F::Translation(r.left, r.top));  // the squares move with it
            g_rt->FillRectangle(r, chk);
        }
    D2D1_MATRIX_3X2_F old;
    g_rt->GetTransform(&old);
    const D2D1_MATRIX_3X2_F turn = D2D1::Matrix3x2F::Rotation(90.f * g_pv.rot) * D2D1::Matrix3x2F::Translation(v.cx, v.cy) * old;
    const float hw = v.baseW * v.k / 2, hh = v.baseH * v.k / 2;
    if (svg) {
        ComPtr<ID2D1DeviceContext5> dc;
        if (SUCCEEDED(g_rt.As(&dc))) {
            const D2D1_SIZE_F vp = D2D1::SizeF(std::max(1.f, hw * 2), std::max(1.f, hh * 2));
            if (g_pv.svgVp.width != vp.width || g_pv.svgVp.height != vp.height) {
                g_pv.svgVp = vp;
                g_pv.svg->SetViewportSize(vp);
            }
            g_rt->SetTransform(D2D1::Matrix3x2F::Translation(-hw, -hh) * turn);
            dc->DrawSvgDocument(g_pv.svg.Get());
        }
    } else if (g_pv.d2d) {
        const D2D1_SIZE_U px = g_pv.d2d->GetPixelSize();
        const float perPx = v.k * g_pv.res->imgW / std::max(1u, px.width);  // screen pixels per pixel of the bitmap
        g_rt->SetTransform(turn);
        const D2D1_RECT_F dst = D2D1::RectF(-hw, -hh, hw, hh);
        ComPtr<ID2D1DeviceContext> dc;
        if (perPx < 1 && SUCCEEDED(g_rt.As(&dc)))  // made smaller: high-quality cubic, no shimmering lines
            dc->DrawBitmap(g_pv.d2d.Get(), &dst, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        else
            g_rt->DrawBitmap(g_pv.d2d.Get(), dst, 1.f,
                             perPx >= 3 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);  // close up: the pixels
        if (perPx > 1.2f && px.width < g_pv.res->imgW) PreviewLoadFull();  // closer than it was decoded
    }
    g_rt->SetTransform(old);
    g_rt->PopAxisAlignedClip();
    g_hits.push_back({box, HitKind::PreviewImage, 0});
    // over it, while the mouse is: turn left, turn right, and the zoom (a click: fit / real size)
    if (!(IsHover(HitKind::PreviewImage, 0) || g_hover.kind == HitKind::PreviewTool || g_pv.panning)) return;
    wchar_t pct[16];
    swprintf(pct, 16, L"%.0f%%", v.k * 100);
    const float bw = 28, lw = std::max(48.f, MeasureW(pct, g_fmtName.Get()) + 18), top = box.top + 8, right = box.right - 8;
    const D2D1_RECT_F bar = D2D1::RectF(right - bw * 2 - lw - 8, top, right, top + 30);
    Round(bar, 8, 0x101014, 0.85f);
    for (int i = 0; i < 3; ++i) {
        const float l = bar.left + 4 + i * bw;
        const D2D1_RECT_F b = D2D1::RectF(l, top + 3, i < 2 ? l + bw : right - 4, top + 27);
        if (IsHover(HitKind::PreviewTool, i)) Round(b, 6, 0xFFFFFF, 0.12f);
        Color(cText);
        if (i == 2) {
            Text(pct, wcslen(pct), g_fmtCenter.Get(), b.left, b.top, b.right, b.bottom);
        } else {
            D2D1_MATRIX_3X2_F was;
            g_rt->GetTransform(&was);
            if (i == 0)  // to the left: the same arrow, mirrored
                g_rt->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F((b.left + b.right) / 2, 0)) * was);
            Glyph(L'\xE7AD', g_fmtIcon.Get(), b.left, b.top, b.right, b.bottom);  // Rotate
            g_rt->SetTransform(was);
        }
        g_hits.push_back({b, HitKind::PreviewTool, i});
    }
}

// --- copying the picture to the clipboard (the palette's "preview copy image", or a right click on the preview).
// An image is read again at its whole size, turned as it shows; a thumbnail or a cover as it is; an SVG drawn at
// 1024 px on its long side (or its own size, if bigger); a playing video, the frame on screen. Opaque pictures go
// as a 24-bit DIB, which every program reads; see-through ones as a DIBV5 with alpha and as PNG.
constexpr UINT WM_APP_CLIPIMG = WM_APP + 17;
struct ClipImage {
    HGLOBAL dib = nullptr, png = nullptr;
    bool v5 = false;  // dib is a BITMAPV5HEADER with alpha (else a 24-bit BITMAPINFOHEADER)
    UINT w = 0, h = 0;
    ~ClipImage() {
        if (dib) GlobalFree(dib);
        if (png) GlobalFree(png);
    }
};
void TestClipImage(const ClipImage& c);  // testmode.inl: tests don't touch the real clipboard

ClipImage* ClipImageFrom(IWICImagingFactory* wic, IWICBitmapSource* src) {
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    ComPtr<IWICFormatConverter> conv;
    if (!w || !h || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return nullptr;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;
    bool alpha = false;
    for (size_t i = 3; i < px.size() && !alpha; i += 4) alpha = px[i] != 255;
    auto c = std::make_unique<ClipImage>();
    c->w = w;
    c->h = h;
    c->v5 = alpha;
    if (alpha) {
        c->dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + px.size());
        auto* p = c->dib ? static_cast<uint8_t*>(GlobalLock(c->dib)) : nullptr;
        if (!p) return nullptr;
        BITMAPV5HEADER hd{};
        hd.bV5Size = sizeof(hd);
        hd.bV5Width = static_cast<LONG>(w);
        hd.bV5Height = static_cast<LONG>(h);  // bottom-up, which every reader takes
        hd.bV5Planes = 1;
        hd.bV5BitCount = 32;
        hd.bV5Compression = BI_BITFIELDS;
        hd.bV5SizeImage = static_cast<DWORD>(px.size());
        hd.bV5RedMask = 0x00FF0000;
        hd.bV5GreenMask = 0x0000FF00;
        hd.bV5BlueMask = 0x000000FF;
        hd.bV5AlphaMask = 0xFF000000;
        hd.bV5CSType = LCS_sRGB;
        hd.bV5Intent = LCS_GM_IMAGES;
        memcpy(p, &hd, sizeof(hd));
        for (UINT y = 0; y < h; ++y) memcpy(p + sizeof(hd) + static_cast<size_t>(y) * w * 4, px.data() + static_cast<size_t>(h - 1 - y) * w * 4, w * 4);
        GlobalUnlock(c->dib);
        // and as PNG, what most programs keep the transparency from
        ComPtr<IStream> st;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fr;
        WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, FALSE, &st))) {
            if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) &&
                SUCCEEDED(fr->SetPixelFormat(&f)) && f == GUID_WICPixelFormat32bppBGRA &&
                SUCCEEDED(fr->WritePixels(h, w * 4, static_cast<UINT>(px.size()), px.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
                GetHGlobalFromStream(st.Get(), &c->png);
            else if (HGLOBAL g = nullptr; SUCCEEDED(GetHGlobalFromStream(st.Get(), &g)))
                GlobalFree(g);
        }
    } else {
        const UINT stride = (w * 3 + 3) & ~3u;
        c->dib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + static_cast<size_t>(stride) * h);
        auto* p = c->dib ? static_cast<uint8_t*>(GlobalLock(c->dib)) : nullptr;
        if (!p) return nullptr;
        BITMAPINFOHEADER hd{};
        hd.biSize = sizeof(hd);
        hd.biWidth = static_cast<LONG>(w);
        hd.biHeight = static_cast<LONG>(h);
        hd.biPlanes = 1;
        hd.biBitCount = 24;
        hd.biCompression = BI_RGB;
        hd.biSizeImage = stride * h;
        memcpy(p, &hd, sizeof(hd));
        for (UINT y = 0; y < h; ++y) {
            uint8_t* d = p + sizeof(hd) + static_cast<size_t>(h - 1 - y) * stride;
            const uint8_t* s = px.data() + static_cast<size_t>(y) * w * 4;
            for (UINT x = 0; x < w; ++x) {
                d[x * 3] = s[x * 4];
                d[x * 3 + 1] = s[x * 4 + 1];
                d[x * 3 + 2] = s[x * 4 + 2];
            }
        }
        GlobalUnlock(c->dib);
    }
    return c.release();
}

// An SVG drawn into a bitmap, 1024 px on its long side or its own size if bigger (4096 at most).
ComPtr<IWICBitmap> SvgToBitmap(IWICImagingFactory* wic, const std::string& svg) {
    ComPtr<ID2D1Factory> f;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf()))) return nullptr;
    auto target = [&](UINT w, UINT h, ComPtr<IWICBitmap>& bmp, ComPtr<ID2D1RenderTarget>& rt, ComPtr<ID2D1DeviceContext5>& dc) {
        return SUCCEEDED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
               SUCCEEDED(f->CreateWicBitmapRenderTarget(bmp.Get(),
                                                        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                                                        &rt)) &&
               SUCCEEDED(rt.As(&dc));
    };
    auto open = [&](ID2D1DeviceContext5* dc, ComPtr<ID2D1SvgDocument>& doc, float& w, float& h) {
        ComPtr<IStream> st;
        st.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(svg.data()), static_cast<UINT>(svg.size())));
        return st && SUCCEEDED(dc->CreateSvgDocument(st.Get(), D2D1::SizeF(100, 100), &doc)) && doc && SvgNormalize(doc.Get(), w, h);
    };
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1DeviceContext5> dc;
    ComPtr<ID2D1SvgDocument> doc;
    float w = 0, h = 0;
    if (!target(1, 1, bmp, rt, dc) || !open(dc.Get(), doc, w, h)) return nullptr;  // its size first
    const float side = std::max(w, h), s = std::clamp(1024.f / side, std::min(1.f, 4096.f / side), 4096.f / side);
    const UINT W = std::max(1u, static_cast<UINT>(std::lround(w * s))), H = std::max(1u, static_cast<UINT>(std::lround(h * s)));
    if (!target(W, H, bmp, rt, dc) || !open(dc.Get(), doc, w, h)) return nullptr;  // a document belongs to its target
    doc->SetViewportSize(D2D1::SizeF(static_cast<float>(W), static_cast<float>(H)));
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0.f));
    dc->DrawSvgDocument(doc.Get());
    return SUCCEEDED(rt->EndDraw()) ? bmp : nullptr;
}

void PreviewClipDone(ClipImage* raw) {
    std::unique_ptr<ClipImage> c(raw);
    bool ok = false;
    if (c->dib && g_test) {
        TestClipImage(*c);
        ok = true;
    } else if (c->dib && OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(c->v5 ? CF_DIBV5 : CF_DIB, c->dib)) {  // the clipboard's now
            c->dib = nullptr;
            ok = true;
        }
        if (ok && c->png && SetClipboardData(RegisterClipboardFormatW(L"PNG"), c->png)) c->png = nullptr;
        CloseClipboard();
    }
    S.message = ok ? L"Image copied: " + std::to_wstring(c->w) + L" × " + std::to_wstring(c->h) + (c->v5 ? L", with its transparency" : L"")
                   : std::wstring(L"The image couldn't be copied");
    Invalidate();
}

bool PvHasPicture() {
    const PreviewResult* r = g_pv.res.get();
    return r && (((r->kind == 1 || r->kind == 9) && r->bmp) || (r->kind == 8 && !r->svg.empty()) ||
                 (r->kind == 9 && MediaFor(r->path) && g_media.shown && g_media.frame));
}

void PreviewCopyImage() {
    if (!(g_preview || g_pvFloat) || !PvHasPicture()) {
        S.message = L"The preview isn't showing a picture to copy";
        Invalidate();
        return;
    }
    const PreviewResult& r = *g_pv.res;
    if (r.kind == 9 && MediaFor(r.path) && g_media.shown && g_media.frame) {  // a video playing: the frame on screen
        ClipImage* c = ClipImageFrom(g_wic.Get(), g_media.frame.Get());
        PreviewClipDone(c ? c : new ClipImage);
        return;
    }
    struct Job {
        std::wstring path;
        int kind;
        bool thumb;
        int rot;
        std::string svg;
    };
    Job j{r.path, r.kind, r.kind == 9 || r.thumb, PvZoomable() ? g_pv.rot : 0, r.kind == 8 ? r.svg : std::string()};
    S.message = L"Copying the image…";
    Invalidate();
    std::thread([j] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ClipImage* c = nullptr;
        {
            ComPtr<IWICImagingFactory> wic;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
                ComPtr<IWICBitmap> bmp;
                PreviewResult tmp;
                if (j.kind == 8) bmp = SvgToBitmap(wic.Get(), j.svg);
                else if (j.thumb ? PvThumb(wic.Get(), j.path, tmp, nullptr) : PvImage(wic.Get(), j.path, tmp, 16384)) bmp = tmp.bmp;
                if (bmp) {
                    ComPtr<IWICBitmapSource> src = bmp;
                    static const WICBitmapTransformOptions turn[] = {WICBitmapTransformRotate0, WICBitmapTransformRotate90, WICBitmapTransformRotate180,
                                                                     WICBitmapTransformRotate270};
                    ComPtr<IWICBitmapFlipRotator> fr;
                    if (j.rot && SUCCEEDED(wic->CreateBitmapFlipRotator(&fr)) && SUCCEEDED(fr->Initialize(bmp.Get(), turn[j.rot & 3]))) src = fr;
                    c = ClipImageFrom(wic.Get(), src.Get());
                }
            }
        }
        CoUninitialize();
        if (!c) c = new ClipImage;  // empty: it failed
        if (!PostMessageW(g_hwnd, WM_APP_CLIPIMG, 0, reinterpret_cast<LPARAM>(c))) delete c;
    }).detach();
}

// A right click on what the preview shows: copy the picture, and turn or zoom it.
bool PreviewMenuAt(float x, float y, POINT screenPt) {
    const D2D1_RECT_F& b = g_pv.box;
    if (!(g_preview || g_pvFloat) || !PvHasPicture() || x < b.left || x > b.right || y < b.top || y > b.bottom) return false;
    std::vector<MenuItem> items;
    auto add = [&](const wchar_t* text, UINT id) {
        MenuItem m;
        m.text = text;
        m.id = id;
        items.push_back(std::move(m));
    };
    add(L"Copy image", 1);
    if (PvZoomable()) {
        MenuItem sep;
        sep.sep = true;
        items.push_back(std::move(sep));
        add(L"Turn left", 2);
        add(L"Turn right", 3);
        add(g_pv.zoom > 0 ? L"Fit" : L"Real size", 4);
    }
    const RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(std::move(items), anchor, [](UINT id) {
        if (id == 1) PreviewCopyImage();
        else if (id == 2) PreviewRotate(-1);
        else if (id == 3) PreviewRotate(1);
        else if (id == 4) PreviewToolClick(2);
    });
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

// What the preview shows, in [x0, W] x [y0, y1]: the content above, the info below. The panel and the floating
// card both draw through here (one of them per frame).
void DrawPreviewBody(float x0, float y0, float W, float y1) {
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
        else if (rowMatches && g_pv.res && g_pv.res->kind == 9 && g_pv.res->mDur)
            info[3] = {L"Length", FormatDuration(g_pv.res->mDur) +
                                      (g_pv.res->imgW ? L"  ·  " + std::to_wstring(g_pv.res->imgW) + L" × " + std::to_wstring(g_pv.res->imgH) : L"")};
        else if (rowMatches && g_pv.res && g_pv.res->kind == 10) {
            wchar_t sz[32];
            FormatSize(g_pv.res->arcBytes, sz, 32);
            info[3] = {L"Contains", Thousands(g_pv.res->arcFiles) + L" files  ·  " + Thousands(g_pv.res->arcDirs) + L" folders  ·  " + sz +
                                        (g_pv.res->arcLocked ? L"  ·  encrypted" : L"")};
        }
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
        if (g_pv.d2d && PvZoomable()) {
            PvDrawPicture(ibox);
        } else if (g_pv.d2d) {  // a thumbnail or an icon: as it is, not enlarged
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
    } else if (g_pv.res && g_pv.res->kind == 9) {
        // a song or a video: the picture, then play / pause, the time and the bar
        const PreviewResult& m = *g_pv.res;
        const float ctlY = bot - 30;
        const bool mine = MediaFor(m.path);
        if (m.media == 2) {
            const D2D1_RECT_F area = D2D1::RectF(L, top, R, ctlY - 12);
            const bool frame = mine && MediaDrawVideo(area);
            if (!frame && m.bmp) PvDrawFit(area, true);
            if (!frame && !m.bmp) {
                Color(cFaint);
                Text(std::wstring(1, L'\xE714'), PvIconFmt(48), area.left, area.top, area.right, area.bottom);  // Video
            }
            if (!mine || !g_media.shown) {  // the big play button, until the video shows
                const float cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
                Color(0x000000, IsHover(HitKind::PreviewPlay, 0) ? 0.7f : 0.55f);
                g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 30, 30), g_brush.Get());
                Color(0xFFFFFF);
                Text(std::wstring(1, L'\xE768'), PvIconFmt(22), cx - 28, cy - 30, cx + 32, cy + 30);
            }
            g_hits.push_back({area, HitKind::PreviewPlay, 0});
        } else {
            const float side = std::max(40.f, std::min({R - L, 220.f, ctlY - top - 74}));
            const float at = top + std::max(4.f, std::round((ctlY - 12 - top - side - 58) / 2));  // cover and names, centered
            const D2D1_RECT_F art = D2D1::RectF(std::round((L + R - side) / 2), at, std::round((L + R + side) / 2), at + side);
            if (m.bmp) {
                PvDrawFit(art, true);
            } else {
                Round(art, 12, 0x24242B);
                Color(cDim);
                Text(std::wstring(1, L'\xEC4F'), PvIconFmt(std::round(side * 0.35f)), art.left, art.top, art.right, art.bottom);  // MusicNote
            }
            g_hits.push_back({art, HitKind::PreviewPlay, 0});
            const std::wstring title = !m.mTitle.empty() ? m.mTitle : LeafOf(m.path).substr(0, LeafOf(m.path).rfind(L'.'));
            std::wstring by = m.mArtist;
            if (!m.mAlbum.empty()) by += (by.empty() ? L"" : L"  ·  ") + m.mAlbum;
            const float tw = MeasureW(title, g_fmtNameBold.Get());  // centered under the cover, cut with … when long
            Color(cText);
            Text(title, g_fmtNameBold.Get(), std::max(L, std::round((L + R - tw) / 2)), art.bottom + 12, R, art.bottom + 34);
            Color(cDim);
            Text(by, g_fmtCenter.Get(), L, art.bottom + 34, R, art.bottom + 54);
        }
        MediaControls(m.path, L, R, ctlY, m.mDur / 1e7);
    } else if (g_pv.res && g_pv.res->kind == 10) {
        // an archive: its tree, folders first, each file's size
        const auto& items = g_pv.res->arc;
        const float rh = 22;
        g_pv.scroll = std::min(g_pv.scroll, std::max(0.f, items.size() * rh - (bot - top)));
        if (items.empty()) {
            Color(cFaint);
            Text(L"Empty archive", g_fmtCenter.Get(), L, top, R, top + 60);
        }
        for (int i = std::max(0, static_cast<int>(g_pv.scroll / rh)); i < static_cast<int>(items.size()); ++i) {
            const float y = top + i * rh - g_pv.scroll;
            if (y > bot) break;
            const ArcItem& it = items[i];
            const float x = L + 2 + std::min(it.depth, 12) * 14.f;
            const int idx = it.dir ? g_iconFolder : IconForExt(it.name.c_str(), it.name.size());
            if (ID2D1Bitmap* bmp = idx >= 0 ? IconBitmap(idx) : nullptr) {
                g_rt->DrawBitmap(bmp, D2D1::RectF(x, y + (rh - kIcon) / 2, x + kIcon, y + (rh + kIcon) / 2), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                Color(it.dir ? 0xE6B450 : 0x9AA3B2);
                Glyph(it.dir ? gFolder : gFile, g_fmtIcon.Get(), x - 2, y, x + kIcon + 2, y + rh);
            }
            wchar_t sz[32] = L"";
            if (!it.dir) FormatSize(it.size, sz, 32);
            const float sw = it.dir ? 0.f : 64.f;
            Color(0xC9C9D1);
            Text(it.name, g_fmtName.Get(), x + 24, y, R - sw, y + rh);
            if (!it.dir) {
                Color(cFaint);
                Text(sz, wcslen(sz), g_fmtRight.Get(), R - sw, y, R, y + rh);
            }
        }
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
        if (PvZoomable()) PvDrawPicture(box);
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

void DrawPreview(float W, float H) {
    if (!g_preview) return;
    const float pw = PreviewW(), x0 = W - pw, y0 = kTabH + ToolH(), y1 = H - kStatus;
    Fill(D2D1::RectF(x0, y0, W, y1), cSide);
    // left edge: drag to change the width (double click: back to the default)
    Fill(D2D1::RectF(x0, y0, x0 + 1, y1), g_pvDrag || IsHover(HitKind::PreviewEdge, 0) ? cDim : cBorder);
    g_hits.push_back({D2D1::RectF(x0 - 3, y0, x0 + 4, y1), HitKind::PreviewEdge, 0});
    if (!g_pvFloat) DrawPreviewBody(x0, y0, W, y1);  // while it floats, the content is up there
}

// The floating preview: a big card over the list, the rest dimmed. It shows at once, without an animation.
void DrawPreviewFloat(float W, float H) {
    if (!g_pvFloat) return;
    const float top = kTabH, room = H - top;
    const float cw = std::clamp(W * 0.72f, std::min(W - 32, 480.f), std::max(200.f, W - 48));
    const float ch = std::clamp(room * 0.86f, std::min(room - 24, 360.f), std::max(160.f, room - 32));
    const float cx = W / 2, cy = top + room / 2;
    const D2D1_RECT_F card = D2D1::RectF(std::round(cx - cw / 2), std::round(cy - ch / 2), std::round(cx + cw / 2), std::round(cy + ch / 2));
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.42f);
    for (int i = 1; i <= 4; ++i)  // a soft shadow
        Round(D2D1::RectF(card.left - i * 3, card.top - i * 2, card.right + i * 3, card.bottom + i * 4), 12.f + i * 3, 0x000000, 0.07f);
    Round(card, 12, cSide);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left + 0.5f, card.top + 0.5f, card.right - 0.5f, card.bottom - 0.5f), 12, 12), g_brush.Get(), 1);
    DrawPreviewBody(card.left + 6, card.top + 6, card.right - 6, card.bottom - 4);
}

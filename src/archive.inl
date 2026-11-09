// Compressing and extracting. Zip with what Windows already has (.NET's ZipFile, through PowerShell) or with 7-Zip
// when it is installed (faster, and 7z too); 7z, rar, tar and the like are extracted with 7-Zip, or else with Windows'
// own tar.exe. One job at a time, on a thread, its progress in the status bar; the palette's "stop compressing or
// extracting" stops it. What it makes is selected when done, and Ctrl+Z sends it to the Recycle Bin.
// "Extract here" puts an archive holding one folder (or one file) right here, and anything else in a folder named
// after it, so nothing spills into the folder; "extract to a folder" always makes that folder.
// Included from app.cpp after preview.inl.

constexpr UINT WM_APP_ARC = WM_APP + 18;  // wp 0: progress (lp: percent); wp 1: done (lp: ArcDone*)

// 7-Zip's console program (or NanaZip's), if installed; empty if not.
const std::wstring& SevenZip() {
    static const std::wstring exe = [] {
        if (GetEnvironmentVariableW(L"JOLT_NO_7ZIP", nullptr, 0)) return std::wstring();  // to try the way without it
        auto ok = [](const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
        for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
            for (const wchar_t* v : {L"Path64", L"Path"}) {
                wchar_t buf[MAX_PATH];
                DWORD size = sizeof(buf);
                if (RegGetValueW(root, L"SOFTWARE\\7-Zip", v, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
                    std::wstring p = buf;
                    if (!p.empty() && p.back() != L'\\') p += L'\\';
                    if (ok(p + L"7z.exe")) return p + L"7z.exe";
                }
            }
        for (const wchar_t* env : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"}) {
            wchar_t buf[MAX_PATH];
            if (GetEnvironmentVariableW(env, buf, MAX_PATH) && ok(std::wstring(buf) + L"\\7-Zip\\7z.exe")) return std::wstring(buf) + L"\\7-Zip\\7z.exe";
        }
        wchar_t la[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH) && ok(std::wstring(la) + L"\\Microsoft\\WindowsApps\\NanaZipC.exe"))
            return std::wstring(la) + L"\\Microsoft\\WindowsApps\\NanaZipC.exe";
        return std::wstring();
    }();
    return exe;
}

struct ArcJob {
    bool extract = false;
    std::wstring folder;               // where it happens
    std::vector<std::wstring> items;   // compress: what goes in, relative to folder
    std::wstring dest;                 // compress: the archive to make
    bool sevenZ = false;               // compress: 7z instead of zip
    std::vector<std::wstring> archives;  // extract: each archive
    bool toFolder = false;               // extract: always into a folder of its own
};
struct ArcDone {
    bool ok = false, stopped = false;
    std::wstring message, folder, select;
    std::vector<std::wstring> created;  // for Ctrl+Z
};
struct {
    bool busy = false;
    std::wstring label;  // "Compressing photos.zip"
    std::atomic<bool> stop{false};
} g_arc;

std::wstring ArcSys(const wchar_t* rel) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + rel;
}
std::wstring ArcQuote(const std::wstring& s) { return L"\"" + s + L"\""; }
std::wstring PsQuote(const std::wstring& s) {  // a PowerShell string: 'it''s'
    std::wstring o = L"'";
    for (wchar_t c : s) o += c == L'\'' ? std::wstring(L"''") : std::wstring(1, c);
    return o + L"'";
}
bool WriteUtf8(const std::wstring& file, const std::wstring& text, bool bom) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), s.data(), n, nullptr, nullptr);
    std::ofstream f(file, std::ios::binary);
    if (bom) f.write("\xEF\xBB\xBF", 3);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return f.good();
}
std::wstring ArcTemp(const wchar_t* ext) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    static std::atomic<int> n{0};
    return std::wstring(dir) + L"jolt-arc-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n) + ext;
}

// Runs a program hidden, its output read for "NN%" (sent as progress); stops it when asked. tail: the end of what it
// said, for an error. False if it couldn't start.
// report: its percent shows (and "archive stop" stops it); keepAll: all it says is kept (a listing).
bool ArcRun(const std::wstring& exe, std::wstring cmd, const std::wstring& dir, DWORD& code, std::wstring& tail, bool report = true, bool keepAll = false) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);  // no questions
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        return false;
    }
    std::string text;
    int last = -1;
    for (;;) {
        if (report && g_arc.stop) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // closed and empty: it's done
        if (!avail) {
            WaitForSingleObject(pi.hProcess, 20);
            continue;
        }
        char buf[8192];
        DWORD got = 0;
        if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
        text.append(buf, got);
        // the last "NN%" said
        for (size_t p = text.rfind('%'); p != std::string::npos && p > 0; p = 0) {
            size_t s = p;
            while (s > 0 && isdigit(static_cast<unsigned char>(text[s - 1]))) --s;
            if (s < p) {
                const int pct = std::clamp(atoi(text.c_str() + s), 0, 100);
                if (pct != last && report) PostMessageW(g_hwnd, WM_APP_ARC, 0, last = pct);
            }
        }
        if (!keepAll && text.size() > 16384) text.erase(0, text.size() - 4096);
        if (text.size() > (64u << 20)) break;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    const UINT cp = n > 0 ? CP_UTF8 : CP_OEMCP;
    if (n <= 0) n = MultiByteToWideChar(cp, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    tail.assign(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, text.data(), static_cast<int>(text.size()), tail.data(), n);
    return true;
}

// What went wrong, in one line, from what the program said: its first line naming an error, else its last one.
std::wstring ArcError(const std::wstring& tail) {
    std::wstring last, err;
    for (size_t p = 0; p < tail.size();) {
        size_t nl = tail.find_first_of(L"\r\n\b", p);
        if (nl == std::wstring::npos) nl = tail.size();
        std::wstring line = Trim(tail.substr(p, nl - p));
        p = nl + 1;
        if (line.empty() || line.back() == L'%' || line.find(L"% ") != std::wstring::npos) continue;
        if (err.empty() && (line.find(L"ERROR") != std::wstring::npos || line.find(L"Exception") != std::wstring::npos ||
                            line.find(L"Error") != std::wstring::npos || line.find(L"Wrong password") != std::wstring::npos))
            err = line;
        last = line;
    }
    std::wstring s = err.empty() ? last : err;
    if (s.size() > 140) s = s.substr(0, 139) + L"…";
    return s;
}

// The names at the top of an archive (the first part of each path), to decide where it goes.
std::vector<std::wstring> ArcTops(const std::wstring& archive) {
    std::vector<std::pair<std::wstring, uint64_t>> raw;
    bool locked = false;
    const std::wstring ext = ExtOf(archive.c_str(), archive.size());
    if (PvArchiveKind(archive, ext) == 1) {
        PvZipList(archive, raw, locked);
    } else if (!SevenZip().empty()) {
        DWORD code = 0;
        std::wstring out;
        if (ArcRun(SevenZip(), ArcQuote(SevenZip()) + L" l -ba -slt -sccUTF-8 -- " + ArcQuote(archive), L"", code, out, false, true))
            for (size_t p = out.find(L"\nPath = "); p != std::wstring::npos; p = out.find(L"\nPath = ", p + 1)) {
                const size_t e = out.find_first_of(L"\r\n", p + 8);
                raw.push_back({out.substr(p + 8, (e == std::wstring::npos ? out.size() : e) - p - 8), 0});
            }
    } else {
        PvTarList(archive, raw);
    }
    std::vector<std::wstring> tops;
    for (auto& [p0, size] : raw) {
        std::wstring p = p0;
        for (wchar_t& c : p)
            if (c == L'\\') c = L'/';
        while (p.rfind(L"./", 0) == 0) p.erase(0, 2);
        while (!p.empty() && p[0] == L'/') p.erase(0, 1);
        const std::wstring top = p.substr(0, p.find(L'/'));
        if (top.empty()) continue;
        bool seen = false;
        for (const std::wstring& t : tops) seen = seen || _wcsicmp(t.c_str(), top.c_str()) == 0;
        if (!seen) tops.push_back(top);
        if (tops.size() > 1) break;
    }
    return tops;
}

// An archive's name without its extension (two for .tar.gz and the like).
std::wstring ArcBase(const std::wstring& archive) {
    std::wstring leaf = LeafOf(archive);
    const size_t dot = leaf.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) leaf = leaf.substr(0, dot);
    const size_t tar = Lower(leaf).rfind(L".tar");
    if (tar != std::wstring::npos && tar > 0 && tar + 4 == leaf.size()) leaf = leaf.substr(0, tar);
    return leaf;
}

void ArcCompressRun(const ArcJob& j, ArcDone& d) {
    const std::wstring list = ArcTemp(L".txt");
    std::wstring names;
    for (const std::wstring& n : j.items) names += n + L"\n";
    DWORD code = 1;
    std::wstring tail, script;
    bool ran = false;
    if (!SevenZip().empty()) {
        WriteUtf8(list, names, false);
        ran = ArcRun(SevenZip(), ArcQuote(SevenZip()) + L" a -t" + (j.sevenZ ? L"7z" : L"zip") + L" -bsp1 -bso0 -bse1 -y -scsUTF-8 -sccUTF-8 " +
                                     ArcQuote(j.dest) + L" @" + ArcQuote(list),
                     j.folder, code, tail);
    } else {  // .NET's ZipFile: every folder walked, UTF-8 names, a line per entry for the progress
        WriteUtf8(list, names, false);
        script = ArcTemp(L".ps1");
        WriteUtf8(script,
                  L"$ErrorActionPreference = 'Stop'\n"
                  L"Add-Type -AssemblyName System.IO.Compression\nAdd-Type -AssemblyName System.IO.Compression.FileSystem\n"
                  L"$base = " + PsQuote(j.folder) + L"\n$dest = " + PsQuote(j.dest) + L"\n"
                  L"$names = [System.IO.File]::ReadAllLines(" + PsQuote(list) + L", [System.Text.Encoding]::UTF8)\n"
                  L"$cut = $base.TrimEnd('\\').Length + 1\n"
                  L"$all = New-Object System.Collections.Generic.List[string[]]\n"
                  L"foreach ($n in $names) {\n"
                  L"  if ($n -eq '') { continue }\n"
                  L"  $p = [System.IO.Path]::Combine($base, $n)\n"
                  L"  if ([System.IO.Directory]::Exists($p)) {\n"
                  L"    $all.Add([string[]]@($p, ($n.Replace('\\', '/') + '/')))\n"
                  L"    foreach ($e in [System.IO.Directory]::EnumerateFileSystemEntries($p, '*', 'AllDirectories')) {\n"
                  L"      $rel = $e.Substring($cut).Replace('\\', '/')\n"
                  L"      if ([System.IO.Directory]::Exists($e)) { $all.Add([string[]]@($e, ($rel + '/'))) } else { $all.Add([string[]]@($e, $rel)) }\n"
                  L"    }\n"
                  L"  } else { $all.Add([string[]]@($p, $n.Replace('\\', '/'))) }\n"
                  L"}\n"
                  L"$zip = [System.IO.Compression.ZipFile]::Open($dest, 'Create', [System.Text.Encoding]::UTF8)\n"
                  L"try {\n"
                  L"  $i = 0\n"
                  L"  foreach ($f in $all) {\n"
                  L"    $i++\n"
                  L"    if ($f[1].EndsWith('/')) { [void]$zip.CreateEntry($f[1]) }\n"
                  L"    else { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f[0], $f[1], 'Optimal') }\n"
                  L"    [Console]::Out.WriteLine(('{0}%' -f [int](100 * $i / $all.Count)))\n"
                  L"  }\n"
                  L"} finally { $zip.Dispose() }\n",
                  true);
        const std::wstring ps = ArcSys(L"\\WindowsPowerShell\\v1.0\\powershell.exe");
        ran = ArcRun(ps, ArcQuote(ps) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + ArcQuote(script), j.folder, code, tail);
    }
    DeleteFileW(list.c_str());
    if (!script.empty()) DeleteFileW(script.c_str());
    d.folder = j.folder;
    const std::wstring name = LeafOf(j.dest);
    if (g_arc.stop) {
        DeleteFileW(ToExtendedPath(j.dest).c_str());  // half an archive is no use
        d.stopped = true;
        d.message = L"Compressing " + name + L" stopped";
    } else if (!ran || code > (SevenZip().empty() ? 0u : 1u) || !PathExists(j.dest)) {  // 7-Zip: 1 is a warning (a file in use was left out)
        DeleteFileW(ToExtendedPath(j.dest).c_str());
        d.message = L"Couldn't compress into " + name + (ran ? L": " + ArcError(tail) : std::wstring(L": the compressor didn't start"));
    } else {
        ULONGLONG stamp = 0, size = 0;
        PathStamp(j.dest, stamp, size);
        wchar_t sz[32];
        FormatSize(size, sz, 32);
        d.ok = true;
        d.select = j.dest;
        d.created.push_back(j.dest);
        d.message = L"Compressed into " + name + L" (" + sz + L")" + (code == 1 && !SevenZip().empty() ? L", but something was left out: " + ArcError(tail) : L"");
    }
}

void ArcExtractRun(const ArcJob& j, ArcDone& d) {
    d.folder = j.folder;
    int done = 0;
    for (const std::wstring& archive : j.archives) {
        if (g_arc.stop) break;
        // where it goes: one thing at its top that isn't here yet goes right here, anything else in its own folder
        const std::vector<std::wstring> tops = j.toFolder ? std::vector<std::wstring>() : ArcTops(archive);
        std::wstring target = j.folder, result;
        if (tops.size() == 1 && !PathExists(Join(j.folder, tops[0].c_str()))) {
            result = Join(j.folder, tops[0].c_str());
        } else {
            target = result = Join(j.folder, FreeName(j.folder, ArcBase(archive), L"").c_str());
            if (!CreateDirectoryW(ToExtendedPath(target).c_str(), nullptr)) {
                d.message = L"Couldn't make the folder " + LeafOf(target);
                break;
            }
        }
        const std::wstring ext = ExtOf(archive.c_str(), archive.size());
        DWORD code = 1;
        std::wstring tail, script;
        bool ran = false;
        if (!SevenZip().empty()) {
            ran = ArcRun(SevenZip(), ArcQuote(SevenZip()) + L" x -bsp1 -bso0 -bse1 -y -aou -sccUTF-8 -o" + ArcQuote(target) + L" -- " + ArcQuote(archive), j.folder,
                         code, tail);
        } else if (PvArchiveKind(archive, ext) == 1) {  // a zip with .NET; nothing written outside the target
            script = ArcTemp(L".ps1");
            WriteUtf8(script,
                      L"$ErrorActionPreference = 'Stop'\nAdd-Type -AssemblyName System.IO.Compression.FileSystem\n"
                      L"$dest = [System.IO.Path]::GetFullPath(" + PsQuote(target) + L")\nif (-not $dest.EndsWith('\\')) { $dest += '\\' }\n"
                      L"$zip = [System.IO.Compression.ZipFile]::OpenRead(" + PsQuote(archive) + L")\n"
                      L"try {\n"
                      L"  $n = [Math]::Max(1, $zip.Entries.Count); $i = 0\n"
                      L"  foreach ($e in $zip.Entries) {\n"
                      L"    $i++\n"
                      L"    $t = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($dest, $e.FullName))\n"
                      L"    if ($t.StartsWith($dest, [System.StringComparison]::OrdinalIgnoreCase)) {\n"
                      L"      if ($e.FullName.EndsWith('/') -or $e.FullName.EndsWith('\\')) { [void][System.IO.Directory]::CreateDirectory($t) }\n"
                      L"      else { [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($t)); "
                      L"[System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $t, $false) }\n"
                      L"    }\n"
                      L"    [Console]::Out.WriteLine(('{0}%' -f [int](100 * $i / $n)))\n"
                      L"  }\n"
                      L"} finally { $zip.Dispose() }\n",
                      true);
            const std::wstring ps = ArcSys(L"\\WindowsPowerShell\\v1.0\\powershell.exe");
            ran = ArcRun(ps, ArcQuote(ps) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + ArcQuote(script), j.folder, code, tail);
        } else {  // Windows' tar.exe (libarchive): 7z, rar, tar...; it refuses paths that would leave the target
            const std::wstring tar = ArcSys(L"\\tar.exe");
            ran = ArcRun(tar, ArcQuote(tar) + L" -xf " + ArcQuote(archive) + L" -C " + ArcQuote(target), j.folder, code, tail);
        }
        if (!script.empty()) DeleteFileW(script.c_str());
        if (g_arc.stop) {
            d.stopped = true;
            d.message = L"Extracting " + LeafOf(archive) + L" stopped; what came out so far is in " + LeafOf(result);
            d.created.push_back(result);
            break;
        }
        if (!ran || code > (SevenZip().empty() ? 0u : 1u)) {  // 7-Zip: 1 is a warning; the others: any is an error
            const std::wstring why = ran ? ArcError(tail) : std::wstring(L"the extractor didn't start");
            const bool tar = SevenZip().empty() && PvArchiveKind(archive, ext) != 1;
            d.message = L"Couldn't extract " + LeafOf(archive) + L": " +
                        (tar ? std::wstring(L"Windows' tar.exe failed (it can't write some names); with 7-Zip installed it works")
                             : why.empty() ? std::wstring(L"it may be damaged or need a password") : why);
            if (target != j.folder) RemoveDirectoryW(ToExtendedPath(target).c_str());  // only if it stayed empty
            if (PathExists(result)) d.created.push_back(result);
            break;
        }
        ++done;
        d.created.push_back(result);
        d.select = result;
    }
    if (d.message.empty()) {
        d.ok = done > 0;
        d.message = done == 1 ? L"Extracted into " + LeafOf(d.select) : L"Extracted " + std::to_wstring(done) + L" archives";
    }
}

void ArcStart(ArcJob j, std::wstring label) {
    g_arc.busy = true;
    g_arc.stop = false;
    g_arc.label = std::move(label);
    S.message = g_arc.label + L"…";
    Invalidate();
    std::thread([j = std::move(j)] {
        auto* d = new ArcDone;
        if (j.extract) ArcExtractRun(j, *d);
        else ArcCompressRun(j, *d);
        if (!PostMessageW(g_hwnd, WM_APP_ARC, 1, reinterpret_cast<LPARAM>(d))) delete d;
    }).detach();
}

void ArcMessage(WPARAM wp, LPARAM lp) {
    if (wp == 0) {
        if (g_arc.busy && !g_arc.stop) {
            S.message = g_arc.label + L"… " + std::to_wstring(static_cast<int>(lp)) + L"%";
            Invalidate();
        }
        return;
    }
    std::unique_ptr<ArcDone> d(reinterpret_cast<ArcDone*>(lp));
    g_arc.busy = false;
    for (size_t i = 0; i < d->created.size(); ++i) {  // one Ctrl+Z for all of it
        g_undoMerge = i > 0;
        UndoRecordCreate(d->created[i]);
    }
    g_undoMerge = false;
    if (!d->folder.empty()) Enqueue(d->folder, JobKind::Revalidate);
    if (!d->select.empty() && Key(T().path) == Key(d->folder)) T().pendingSelect = d->select;
    S.message = d->message;
    Invalidate();
}

bool ArcRefuses() {
    if (!g_arc.busy) return false;
    S.message = g_arc.label + L" isn't done yet (Ctrl+K → stop compressing or extracting)";
    Invalidate();
    return true;
}

// The selection, relative to the tab's folder; false (with a note) when it can't be compressed from here.
bool ArcSelection(std::vector<std::wstring>& rel) {
    Tab& t = T();
    if (IsVirtual(t.path) || !t.list || t.list->error) {
        S.message = L"Compressing works on the items of a folder";
        Invalidate();
        return false;
    }
    const std::wstring base = t.path.back() == L'\\' ? t.path : t.path + L'\\';
    for (const std::wstring& f : SelectedFullPaths(t))
        if (f.size() > base.size() && _wcsnicmp(f.c_str(), base.c_str(), base.size()) == 0) rel.push_back(f.substr(base.size()));
    if (rel.empty()) {
        S.message = L"Select what to compress";
        Invalidate();
    }
    return !rel.empty();
}

void ArchiveCompress(bool sevenZ) {
    if (ArcRefuses()) return;
    Tab& t = T();
    std::vector<std::wstring> rel;
    if (!ArcSelection(rel) || TestRefuses(t.path)) return;
    if (sevenZ && SevenZip().empty()) {
        S.message = L"7z needs 7-Zip installed; ZIP works without it";
        Invalidate();
        return;
    }
    // its name: the item's (a file without its extension), or the folder's for several
    std::wstring base = LeafOf(t.path);
    if (rel.size() == 1) {
        base = LeafOf(rel[0]);
        const size_t dot = base.rfind(L'.');
        if (!PathIsDir(Join(t.path, rel[0].c_str())) && dot != std::wstring::npos && dot > 0) base = base.substr(0, dot);
    }
    if (base.empty() || base.back() == L':' || base.back() == L'\\') base = L"Archive";
    ArcJob j;
    j.folder = t.path;
    j.items = std::move(rel);
    j.sevenZ = sevenZ;
    j.dest = Join(t.path, FreeName(t.path, base, sevenZ ? L".7z" : L".zip").c_str());
    const std::wstring label = L"Compressing " + LeafOf(j.dest);
    ArcStart(std::move(j), label);
}

std::vector<std::wstring> ArcSelectedArchives() {
    std::vector<std::wstring> out;
    for (const std::wstring& f : SelectedFullPaths(T()))
        if (!PathIsDir(f) && PvArchiveKind(f, ExtOf(f.c_str(), f.size()))) out.push_back(f);
    return out;
}

void ArchiveExtract(bool toFolder) {
    if (ArcRefuses()) return;
    std::vector<std::wstring> archives = ArcSelectedArchives();
    if (archives.empty()) {
        S.message = L"Select a zip, 7z, rar or other archive to extract";
        Invalidate();
        return;
    }
    const std::wstring folder = ParentOf(archives[0]);
    if (TestRefuses(folder)) return;
    ArcJob j;
    j.extract = true;
    j.folder = folder;
    j.toFolder = toFolder;
    for (const std::wstring& a : archives)
        if (Key(ParentOf(a)) == Key(folder)) j.archives.push_back(a);  // those next to the first (search results can mix folders)
    const std::wstring label = L"Extracting " + (j.archives.size() == 1 ? LeafOf(j.archives[0]) : std::to_wstring(j.archives.size()) + L" archives");
    ArcStart(std::move(j), label);
}

void ArchiveStop() {
    if (!g_arc.busy) {
        S.message = L"Nothing is being compressed or extracted";
    } else {
        g_arc.stop = true;
        S.message = L"Stopping…";
    }
    Invalidate();
}

// The context menu's entries: compress, and extract when archives are selected.
std::vector<MenuItem> ArchiveMenuItems(UINT firstId) {
    std::vector<MenuItem> items;
    auto add = [&](const std::wstring& text, UINT id, const wchar_t* key) {
        MenuItem m;
        m.text = text;
        m.id = firstId + id;
        m.key = key;
        items.push_back(std::move(m));
    };
    const std::vector<std::wstring> archives = ArcSelectedArchives();
    if (archives.size() == 1 && !ArcInside(archives[0])) add(L"Browse inside", 4, L"jolt:browse_archive");
    if (!archives.empty()) {
        add(L"Extract here", 2, L"jolt:extract");
        add(archives.size() == 1 ? L"Extract to \"" + ArcBase(archives[0]) + L"\\\"" : std::wstring(L"Extract each to its own folder"), 3, L"jolt:extract_to");
    }
    add(L"Compress to ZIP", 0, L"jolt:compress");
    if (!SevenZip().empty()) add(L"Compress to 7z", 1, L"jolt:compress_7z");
    return items;
}
void ArchiveMenuPick(UINT what) {
    if (what == 4) {
        const std::vector<std::wstring> archives = ArcSelectedArchives();
        if (!archives.empty()) Navigate(T(), archives[0], true);
        return;
    }
    if (what == 0) ArchiveCompress(false);
    else if (what == 1) ArchiveCompress(true);
    else ArchiveExtract(what == 3);
}

// ---------------------------------------------------------------- browsing an archive as a folder
// "C:\x\photos.zip\2024\day 1" lists what is in that folder of the archive. It is read-only: what is opened,
// previewed, copied or extracted from it is taken out to a temporary folder first (%TEMP%\Jolt-archives), and
// whatever would change it is refused with a note. Enter goes into zip, 7z, rar, tar and cab ([general]
// browse_archives); jar, apk, epub, iso and the like still open with their program, and "Browse inside" in their
// right-click menu goes in.

// The archives Enter goes into.
bool ArcBrowsableExt(const std::wstring& path) {
    const std::wstring ext = ExtOf(path.c_str(), path.size());
    static const wchar_t* k[] = {L".zip", L".7z", L".rar", L".tar", L".tgz", L".tbz", L".tbz2", L".txz", L".tzst", L".cab"};
    for (const wchar_t* e : k)
        if (ext == e) return true;
    const std::wstring low = Lower(path);
    for (const wchar_t* s : {L".tar.gz", L".tar.bz2", L".tar.xz", L".tar.zst"})
        if (low.size() > wcslen(s) && low.compare(low.size() - wcslen(s), wcslen(s), s) == 0) return true;
    return false;
}

// A path through an archive: "C:\a\b.zip\x\y" is the archive "C:\a\b.zip" and "x\y" in it ("" for its top).
// Only components with an archive's extension are looked at on disk, and only if they are files.
bool ArcSplit(const std::wstring& path, std::wstring& archive, std::wstring& inner) {
    if (path.size() < 4 || IsVirtual(path)) return false;
    for (size_t p = 0; p < path.size();) {
        size_t e = path.find(L'\\', p);
        if (e == std::wstring::npos) e = path.size();
        const std::wstring prefix = path.substr(0, e);
        if (PvArchiveKind(prefix, ExtOf(prefix.c_str(), prefix.size()))) {
            const DWORD a = GetFileAttributesW(ToExtendedPath(prefix).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                archive = prefix;
                inner = e < path.size() ? path.substr(e + 1) : std::wstring();
                while (!inner.empty() && inner.back() == L'\\') inner.pop_back();
                return true;
            }
        }
        p = e + 1;
    }
    return false;
}
// A folder that is an archive or inside one (the archive itself included).
bool InArchive(const std::wstring& path) {
    thread_local std::wstring last;
    thread_local bool lastIn = false;
    if (path != last) {
        std::wstring a, i;
        last = path;
        lastIn = ArcSplit(path, a, i);
    }
    return lastIn;
}
// An item inside an archive (not the archive itself).
bool ArcInside(const std::wstring& path) {
    std::wstring a, i;
    return ArcSplit(path, a, i) && !i.empty();
}

// What an archive holds, read once and kept while the archive doesn't change.
struct ArcTree {
    ULONGLONG stamp = 0, size = 0;
    bool ok = false;
    struct Item {
        std::wstring path;  // "folder/file.txt", folders without the slash; every folder is there, even unlisted ones
        std::wstring raw;   // as the archive names it (for taking it out); empty for unlisted folders
        bool dir = false;
        uint64_t size = 0;
        int64_t mtime = 0;
    };
    std::vector<Item> items;
    std::unordered_map<std::wstring, size_t> index;  // lowercase path -> item
    const Item* Find(std::wstring inner) const {
        for (wchar_t& c : inner)
            if (c == L'\\') c = L'/';
        auto it = index.find(Lower(inner));
        return it == index.end() ? nullptr : &items[it->second];
    }
};
std::mutex g_arcTreeM;
std::unordered_map<std::wstring, std::shared_ptr<const ArcTree>> g_arcTrees;  // Key(archive)

int64_t ArcLocalTime(int Y, int M, int D, int h, int m, int s) {  // a local date and time as UTC FILETIME
    SYSTEMTIME lt{static_cast<WORD>(Y), static_cast<WORD>(M), 0, static_cast<WORD>(D), static_cast<WORD>(h), static_cast<WORD>(m), static_cast<WORD>(s), 0};
    SYSTEMTIME ut;
    FILETIME ft;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut) || !SystemTimeToFileTime(&ut, &ft)) return 0;
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::shared_ptr<const ArcTree> ArcTreeOf(const std::wstring& archive) {
    ULONGLONG stamp = 0, size = 0;
    if (!PathStamp(archive, stamp, size)) return nullptr;
    const std::wstring k = Key(archive);
    {
        std::lock_guard lk(g_arcTreeM);
        auto it = g_arcTrees.find(k);
        if (it != g_arcTrees.end() && it->second->stamp == stamp && it->second->size == size) return it->second;
    }
    auto t = std::make_shared<ArcTree>();
    t->stamp = stamp;
    t->size = size;
    struct Raw {
        std::wstring path;
        uint64_t size;
        int64_t mtime;
        bool dir;
    };
    std::vector<Raw> raw;
    const int64_t own = static_cast<int64_t>(stamp);
    if (PvArchiveKind(archive, ExtOf(archive.c_str(), archive.size())) == 1) {
        std::vector<std::pair<std::wstring, uint64_t>> z;
        std::vector<int64_t> times;
        bool locked = false;
        if ((t->ok = PvZipList(archive, z, locked, &times)))
            for (size_t i = 0; i < z.size(); ++i)
                raw.push_back({z[i].first, z[i].second, i < times.size() && times[i] ? times[i] : own, !z[i].first.empty() && z[i].first.back() == L'/'});
    } else if (!SevenZip().empty()) {
        DWORD code = 1;
        std::wstring out;
        if (ArcRun(SevenZip(), ArcQuote(SevenZip()) + L" l -ba -slt -sccUTF-8 " + ArcQuote(archive), L"", code, out, false, true) && code == 0) {
            t->ok = true;
            Raw cur{L"", 0, own, false};
            auto flush = [&] {
                if (!cur.path.empty()) raw.push_back(cur);
                cur = Raw{L"", 0, own, false};
            };
            for (size_t p = 0; p < out.size();) {
                size_t e = out.find_first_of(L"\r\n", p);
                if (e == std::wstring::npos) e = out.size();
                const std::wstring line = out.substr(p, e - p);
                p = e + 1;
                if (line.rfind(L"Path = ", 0) == 0) {
                    flush();
                    cur.path = line.substr(7);
                } else if (line.rfind(L"Size = ", 0) == 0) {
                    cur.size = _wcstoui64(line.c_str() + 7, nullptr, 10);
                } else if (line == L"Folder = +" || (line.rfind(L"Attributes = ", 0) == 0 && line.size() > 13 && line[13] == L'D')) {
                    cur.dir = true;
                } else if (line.rfind(L"Modified = ", 0) == 0) {
                    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
                    if (swscanf_s(line.c_str() + 11, L"%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) cur.mtime = ArcLocalTime(Y, M, D, h, m, s);
                }
            }
            flush();
        }
    } else {
        std::vector<std::pair<std::wstring, uint64_t>> l;
        if ((t->ok = PvTarList(archive, l)))
            for (auto& [p, s] : l) raw.push_back({p, s, own, !p.empty() && p.back() == L'/'});
    }
    // every path the same way, every folder there (even the ones only implied by what is in them)
    auto add = [&](const std::wstring& p, bool dir, uint64_t s, int64_t mtime, const std::wstring& rawName) {
        for (size_t slash = p.find(L'/'); slash != std::wstring::npos; slash = p.find(L'/', slash + 1)) {
            const std::wstring d = p.substr(0, slash), dk = Lower(d);
            if (!d.empty() && !t->index.count(dk)) {
                t->index[dk] = t->items.size();
                t->items.push_back({d, L"", true, 0, mtime});
            }
        }
        if (p.empty()) return;
        const std::wstring pk = Lower(p);
        auto it = t->index.find(pk);
        if (it != t->index.end()) {  // a folder seen before its own entry: now it has its name in the archive
            if (t->items[it->second].raw.empty()) t->items[it->second].raw = rawName;
            return;
        }
        t->index[pk] = t->items.size();
        t->items.push_back({p, rawName, dir, s, mtime});
    };
    for (const Raw& r : raw) {
        std::wstring p = r.path;
        for (wchar_t& c : p)
            if (c == L'\\') c = L'/';
        while (p.rfind(L"./", 0) == 0) p.erase(0, 2);
        while (!p.empty() && p[0] == L'/') p.erase(0, 1);
        const bool dir = r.dir || (!p.empty() && p.back() == L'/');
        while (!p.empty() && p.back() == L'/') p.pop_back();
        std::wstring rawName = r.path;
        while (!rawName.empty() && (rawName.back() == L'/' || rawName.back() == L'\\')) rawName.pop_back();
        add(p, dir, r.size, r.mtime, rawName);
    }
    std::lock_guard lk(g_arcTreeM);
    if (g_arcTrees.size() > 32) g_arcTrees.clear();
    g_arcTrees[k] = t;
    return t;
}

// A folder inside an archive, as a listing (from the worker). False when the path doesn't go through an archive.
bool ArcEnumerate(const std::wstring& path, Listing& out) {
    std::wstring archive, inner;
    if (!ArcSplit(path, archive, inner)) return false;
    out.path = path;
    const std::shared_ptr<const ArcTree> tree = ArcTreeOf(archive);
    if (!tree || !tree->ok) {
        out.error = ERROR_BAD_FORMAT;
        return true;
    }
    std::wstring prefix = inner;
    for (wchar_t& c : prefix)
        if (c == L'\\') c = L'/';
    if (!prefix.empty()) {
        const ArcTree::Item* self = tree->Find(prefix);
        if (!self || !self->dir) {
            out.error = ERROR_PATH_NOT_FOUND;
            return true;
        }
        prefix += L'/';
    }
    for (const ArcTree::Item& it : tree->items) {
        if (it.path.size() <= prefix.size() || _wcsnicmp(it.path.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
        if (it.path.find(L'/', prefix.size()) != std::wstring::npos) continue;  // deeper down
        const wchar_t* name = it.path.c_str() + prefix.size();
        ListingAdd(out, name, wcslen(name), it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, it.size, it.mtime);
    }
    return true;
}

// Is this item inside an archive a folder?
bool ArcIsDir(const std::wstring& path) {
    std::wstring archive, inner;
    if (!ArcSplit(path, archive, inner)) return false;
    if (inner.empty()) return true;
    const std::shared_ptr<const ArcTree> tree = ArcTreeOf(archive);
    const ArcTree::Item* it = tree ? tree->Find(inner) : nullptr;
    return it && it->dir;
}

// Takes items out of an archive into dir, each keeping its path inside the archive (a folder with all it holds).
std::mutex g_arcTakeM;
bool ArcExtractSome(const std::wstring& archive, const std::vector<std::wstring>& names, const std::wstring& dir) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    DWORD code = 1;
    std::wstring tail, list, script;
    bool ran = false;
    if (!SevenZip().empty()) {
        list = ArcTemp(L".txt");
        std::wstring all;
        for (const std::wstring& n : names) all += n + L"\n";
        WriteUtf8(list, all, false);
        ran = ArcRun(SevenZip(), ArcQuote(SevenZip()) + L" x -y -bso0 -bse1 -aoa -scsUTF-8 -sccUTF-8 -o" + ArcQuote(dir) + L" " + ArcQuote(archive) + L" @" + ArcQuote(list),
                     L"", code, tail, false) &&
              code <= 1;
    } else if (PvArchiveKind(archive, ExtOf(archive.c_str(), archive.size())) == 1) {
        script = ArcTemp(L".ps1");
        std::wstring arr;
        for (const std::wstring& n : names) {
            std::wstring s = n;
            for (wchar_t& c : s)
                if (c == L'\\') c = L'/';
            arr += (arr.empty() ? L"" : L", ") + PsQuote(s);
        }
        WriteUtf8(script,
                  L"$ErrorActionPreference = 'Stop'\nAdd-Type -AssemblyName System.IO.Compression.FileSystem\n"
                  L"$dest = [System.IO.Path]::GetFullPath(" + PsQuote(dir) + L")\nif (-not $dest.EndsWith('\\')) { $dest += '\\' }\n"
                  L"$want = @(" + arr + L")\n"
                  L"$zip = [System.IO.Compression.ZipFile]::OpenRead(" + PsQuote(archive) + L")\n"
                  L"try {\n"
                  L"  foreach ($e in $zip.Entries) {\n"
                  L"    $n = $e.FullName.Replace('\\', '/').TrimEnd('/')\n"
                  L"    $hit = $false\n"
                  L"    foreach ($w in $want) { if ($n -eq $w -or $n.StartsWith($w + '/', [System.StringComparison]::OrdinalIgnoreCase)) { $hit = $true } }\n"
                  L"    if (-not $hit) { continue }\n"
                  L"    $t = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($dest, $e.FullName))\n"
                  L"    if (-not $t.StartsWith($dest, [System.StringComparison]::OrdinalIgnoreCase)) { continue }\n"
                  L"    if ($e.FullName.EndsWith('/') -or $e.FullName.EndsWith('\\')) { [void][System.IO.Directory]::CreateDirectory($t) }\n"
                  L"    else { [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($t)); "
                  L"[System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $t, $true) }\n"
                  L"  }\n"
                  L"} finally { $zip.Dispose() }\n",
                  true);
        const std::wstring ps = ArcSys(L"\\WindowsPowerShell\\v1.0\\powershell.exe");
        ran = ArcRun(ps, ArcQuote(ps) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + ArcQuote(script), L"", code, tail, false) &&
              code == 0;
    } else {
        const std::wstring tar = ArcSys(L"\\tar.exe");
        std::wstring cmd = ArcQuote(tar) + L" -xf " + ArcQuote(archive) + L" -C " + ArcQuote(dir);
        for (const std::wstring& n : names) cmd += L" " + ArcQuote(n);
        ran = ArcRun(tar, cmd, L"", code, tail, false) && code == 0;
    }
    if (!list.empty()) DeleteFileW(list.c_str());
    if (!script.empty()) DeleteFileW(script.c_str());
    return ran;
}

// Where things taken out of an archive go: one folder per archive (as it is now), so the second time is instant.
std::wstring ArcTempRoot() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"Jolt-archives";
}
std::wstring ArcCacheDir(const std::wstring& archive, ULONGLONG stamp) {
    wchar_t h[24];
    swprintf(h, 24, L"%016llx", static_cast<unsigned long long>(std::hash<std::wstring>{}(Key(archive) + L"|" + std::to_wstring(stamp))));
    return ArcTempRoot() + L"\\" + h;
}

// A file inside an archive, taken out (once) to the archive's temporary folder; out is where it is. False if it is a
// folder, bigger than most, or couldn't be taken out.
bool ArcTakeOut(const std::wstring& path, std::wstring& out, uint64_t most) {
    std::wstring archive, inner;
    if (!ArcSplit(path, archive, inner) || inner.empty()) return false;
    const std::shared_ptr<const ArcTree> tree = ArcTreeOf(archive);
    const ArcTree::Item* it = tree ? tree->Find(inner) : nullptr;
    if (!it || it->dir || it->size > most) return false;
    const std::wstring dir = ArcCacheDir(archive, tree->stamp);
    out = dir + L"\\" + inner;
    std::lock_guard lk(g_arcTakeM);
    ULONGLONG st = 0, sz = 0;
    if (PathStamp(out, st, sz) && sz == it->size) return true;  // taken out before
    ArcExtractSome(archive, {it->raw.empty() ? inner : it->raw}, dir);
    return PathStamp(out, st, sz);
}

// The window's side of taking things out: a note, files for the clipboard, or files to move somewhere.
void TestLog(const std::wstring& s);  // testmode.inl
struct ArcOut {
    std::wstring message;
    std::vector<std::wstring> files;  // what was taken out (each item at the top of what was asked)
    int then = 0;                     // 0: just the note; 1: on the clipboard; 2: moved into dest; 3: gone into (an archive)
    std::wstring dest;
};
constexpr UINT WM_APP_ARCOUT = WM_APP + 19;

void ArcOutDone(ArcOut* raw) {
    std::unique_ptr<ArcOut> o(raw);
    if (o->then == 1 && !o->files.empty() && g_test) {  // tests leave the real clipboard alone
        for (const std::wstring& f : o->files) TestLog(L"to the clipboard: " + f);
    } else if (o->then == 1 && !o->files.empty()) {
        if (ClipboardSetFiles(o->files, false)) {
            g_ignoreClipUpdate = true;
            g_cutKeys.clear();
        }
    } else if (o->then == 2 && !o->files.empty()) {
        if (FileOp(FileOpKind::Move, o->files, o->dest)) {
            if (o->files.size() == 1 && Key(T().path) == Key(o->dest)) T().pendingSelect = LeafOf(o->files[0]);
            for (auto& tp : g_tabs)
                if (Key(tp->path) == Key(o->dest)) Enqueue(o->dest, JobKind::Revalidate);
        } else {
            o->message.clear();
        }
    }
    if (o->then == 3 && !o->files.empty()) Navigate(T(), o->files[0], true);
    if (!o->message.empty()) S.message = o->message;
    Invalidate();
}

// The selection inside an archive, taken out on a thread to a folder of its own, then put on the clipboard (then 1)
// or moved into dest (then 2).
void ArcTakeSelection(int then, const std::wstring& dest, const std::wstring& done) {
    Tab& t = T();
    std::wstring archive, inner;
    const std::vector<std::wstring> sel = SelectedFullPaths(t);
    if (sel.empty() || !ArcSplit(t.path, archive, inner)) return;
    std::vector<std::wstring> inners;
    for (const std::wstring& f : sel) {
        std::wstring a, i;
        if (ArcSplit(f, a, i) && !i.empty()) inners.push_back(i);
    }
    if (inners.empty()) return;
    S.message = L"Taking " + (inners.size() == 1 ? LeafOf(sel[0]) : std::to_wstring(inners.size()) + L" items") + L" out of " + LeafOf(archive) + L"…";
    Invalidate();
    std::thread([archive, inners, then, dest, done] {
        auto* o = new ArcOut;
        o->then = then;
        o->dest = dest;
        const std::shared_ptr<const ArcTree> tree = ArcTreeOf(archive);
        static std::atomic<int> n{0};
        const std::wstring dir = ArcTempRoot() + L"\\out-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                                 std::to_wstring(++n);
        std::vector<std::wstring> names;
        for (const std::wstring& i : inners) {
            const ArcTree::Item* it = tree ? tree->Find(i) : nullptr;
            names.push_back(it && !it->raw.empty() ? it->raw : i);
        }
        if (tree && ArcExtractSome(archive, names, dir))
            for (const std::wstring& i : inners)  // each where it landed (its folders above don't go with it)
                if (PathExists(dir + L"\\" + i)) o->files.push_back(dir + L"\\" + i);
        o->message = o->files.empty() ? L"Couldn't take that out of " + LeafOf(archive) : done;
        if (!PostMessageW(g_hwnd, WM_APP_ARCOUT, 0, reinterpret_cast<LPARAM>(o))) delete o;
    }).detach();
}

// Enter on a file inside an archive: taken out, then opened with its program; an archive in it is gone into.
void ArcOpenInside(const std::wstring& full) {
    const bool browse = g_cfg.browseArchives && ArcBrowsableExt(full);
    S.message = L"Opening " + LeafOf(full) + L"…";
    Invalidate();
    std::thread([full, browse] {
        std::wstring file;
        auto* o = new ArcOut;
        const bool took = ArcTakeOut(full, file, ~0ull);
        if (took && browse) {
            o->then = 3;
            o->files.push_back(file);
            o->message = L"Inside " + LeafOf(full) + L": a copy taken out of the archive";
        } else if (took && g_test) {  // tests don't start programs
            o->message = L"(test) would open " + file;
        } else if (took) {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteW(nullptr, nullptr, file.c_str(), nullptr, ParentOf(file).c_str(), SW_SHOWNORMAL);
            CoUninitialize();
            o->message = L"Opened " + LeafOf(full) + L" (a copy taken out of the archive: changes to it don't go back in)";
        } else {
            o->message = L"Couldn't take " + LeafOf(full) + L" out of the archive";
        }
        if (!PostMessageW(g_hwnd, WM_APP_ARCOUT, 0, reinterpret_cast<LPARAM>(o))) delete o;
    }).detach();
}

// Dragging out of an archive: the selection is taken out to a folder of its own first (2 GB at most), and those
// copies are what is dragged, so they can be dropped on a tab, the other pane, File Explorer or the desktop.
bool ArcDragOut(ComPtr<IDataObject>& pdo, std::vector<std::wstring>& files) {
    Tab& t = T();
    std::wstring archive, inner;
    if (!ArcSplit(t.path, archive, inner)) return false;
    const std::shared_ptr<const ArcTree> tree = ArcTreeOf(archive);
    if (!tree) return false;
    std::vector<std::wstring> inners, names;
    uint64_t bytes = 0;
    for (const std::wstring& f : SelectedFullPaths(t)) {
        std::wstring a, i;
        if (!ArcSplit(f, a, i) || i.empty()) continue;
        const ArcTree::Item* it = tree->Find(i);
        if (!it) continue;
        inners.push_back(i);
        names.push_back(it->raw.empty() ? i : it->raw);
        if (!it->dir) {
            bytes += it->size;
        } else {  // everything under it
            std::wstring prefix = it->path + L"/";
            for (const ArcTree::Item& c : tree->items)
                if (!c.dir && c.path.size() > prefix.size() && _wcsnicmp(c.path.c_str(), prefix.c_str(), prefix.size()) == 0) bytes += c.size;
        }
    }
    if (inners.empty()) return false;
    if (bytes > (2ull << 30)) {
        S.message = L"Too much to drag out of the archive at once: F5 or right click → Extract do it without waiting";
        Invalidate();
        return false;
    }
    HCURSOR was = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    static int n = 0;
    const std::wstring dir = ArcTempRoot() + L"\\drag-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++n);
    const bool ok = ArcExtractSome(archive, names, dir);
    SetCursor(was);
    for (const std::wstring& i : inners)
        if (ok && PathExists(dir + L"\\" + i)) files.push_back(dir + L"\\" + i);
    if (files.empty()) {
        S.message = L"Couldn't take that out of " + LeafOf(archive);
        Invalidate();
        return false;
    }
    std::vector<PIDLIST_ABSOLUTE> full;  // the copies as Shell items: what other programs take
    for (const std::wstring& p : files) {
        PIDLIST_ABSOLUTE f = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &f, 0, nullptr)) && f) full.push_back(f);
    }
    ComPtr<IShellItemArray> items;
    if (!full.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(full.size()), (PCIDLIST_ABSOLUTE_ARRAY)full.data(), &items)))
        items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&pdo));
    for (auto f : full) CoTaskMemFree(f);
    return pdo != nullptr;
}

// Commands inside an archive: copying takes things out, the rest of what would change it is refused. True when handled.
bool ArcBrowseCommand(Cmd cmd) {
    Tab& t = T();
    std::wstring archive, inner;
    if (!ArcSplit(t.path, archive, inner)) return false;
    switch (cmd) {
        case Cmd::Copy:
            ArcTakeSelection(1, L"", L"Copied out of " + LeafOf(archive) + L": paste it anywhere");
            return true;
        case Cmd::CopyToOther: {
            Tab& dst = PT(1 - g_act);
            if (!g_split || dst.path.empty() || IsVirtual(dst.path) || InArchive(dst.path)) {
                S.message = L"Open split view on a folder to extract there";
                Invalidate();
                return true;
            }
            ArcTakeSelection(2, dst.path, L"Extracted to " + TabTitle(dst));
            return true;
        }
        case Cmd::Rename: case Cmd::RenameMany: case Cmd::NewFolder: case Cmd::NewFile: case Cmd::Cut: case Cmd::Paste: case Cmd::Delete:
        case Cmd::MoveToOther: case Cmd::CopyTo: case Cmd::MoveTo: case Cmd::Attributes: case Cmd::InUse: case Cmd::Unblock: case Cmd::Duplicate:
        case Cmd::GroupInFolder: case Cmd::Properties: case Cmd::Terminal: case Cmd::OpenAsAdmin: case Cmd::Tag: case Cmd::SearchSubfolders:
        case Cmd::Compress: case Cmd::Compress7z: case Cmd::Extract: case Cmd::ExtractTo:
            S.message = L"Inside " + LeafOf(archive) + L" nothing can be changed: copy out what you need (Ctrl+C), or extract it";
            Invalidate();
            return true;
        default:
            if (IsCustom(cmd)) {
                S.message = L"Your commands work on files on disk: copy it out of the archive first";
                Invalidate();
                return true;
            }
            return false;
    }
}

// The right-click menu inside an archive: open, copy out, extract next to the archive.
void ArcItemMenu(POINT screenPt, int row) {
    Tab& t = T();
    std::wstring archive, inner;
    if (!ArcSplit(t.path, archive, inner)) return;
    std::vector<MenuItem> items;
    auto add = [&](const std::wstring& text, UINT id, const wchar_t* shortcut = L"") {
        MenuItem m;
        m.text = text;
        m.id = id;
        m.shortcut = shortcut;
        items.push_back(std::move(m));
    };
    const std::wstring here = LeafOf(ParentOf(archive));
    if (row >= 0) {
        add(L"Open", 1, L"Enter");
        add(L"Copy", 2, L"Ctrl+C");
        add(L"Extract to \"" + (here.empty() ? ParentOf(archive) : here) + L"\"", 3);
        if (g_split) add(L"Extract to the other pane", 4);
    } else {
        add(L"Extract everything here (next to " + LeafOf(archive) + L")", 5);
    }
    const RECT anchor{screenPt.x, screenPt.y, screenPt.x, screenPt.y};
    PopupOpen(std::move(items), anchor, [archive](UINT id) {
        if (id == 1) OpenSelected(false);
        else if (id == 2) ArcBrowseCommand(Cmd::Copy);
        else if (id == 3) ArcTakeSelection(2, ParentOf(archive), L"Extracted next to " + LeafOf(archive));
        else if (id == 4) ArcBrowseCommand(Cmd::CopyToOther);
        else if (id == 5) {
            if (ArcRefuses()) return;
            ArcJob j;
            j.extract = true;
            j.folder = ParentOf(archive);
            j.archives.push_back(archive);
            ArcStart(std::move(j), L"Extracting " + LeafOf(archive));
        }
    });
}

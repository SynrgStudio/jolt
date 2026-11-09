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
bool ArcRun(const std::wstring& exe, std::wstring cmd, const std::wstring& dir, DWORD& code, std::wstring& tail) {
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
        if (g_arc.stop) {
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
                if (pct != last) PostMessageW(g_hwnd, WM_APP_ARC, 0, last = pct);
            }
        }
        if (text.size() > 16384) text.erase(0, text.size() - 4096);
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
        if (ArcRun(SevenZip(), ArcQuote(SevenZip()) + L" l -ba -slt -sccUTF-8 -- " + ArcQuote(archive), L"", code, out))
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
    if (!archives.empty()) {
        add(L"Extract here", 2, L"jolt:extract");
        add(archives.size() == 1 ? L"Extract to \"" + ArcBase(archives[0]) + L"\\\"" : std::wstring(L"Extract each to its own folder"), 3, L"jolt:extract_to");
    }
    add(L"Compress to ZIP", 0, L"jolt:compress");
    if (!SevenZip().empty()) add(L"Compress to 7z", 1, L"jolt:compress_7z");
    return items;
}
void ArchiveMenuPick(UINT what) {
    if (what == 0) ArchiveCompress(false);
    else if (what == 1) ArchiveCompress(true);
    else ArchiveExtract(what == 3);
}

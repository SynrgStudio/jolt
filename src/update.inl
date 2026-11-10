// Telling when there's a new Jolt (#70). Off by default: Jolt doesn't touch the network unless asked to.
// With [general] check_updates = yes it asks GitHub for the latest release at most once a day (a few seconds after it
// opens, and again while it stays open), quietly if there's no network, and if it is newer says so in the status bar.
// Ctrl+K → "check for updates" asks right away, whatever the setting says (that is asking).
// "Update" downloads that release's jolt.exe, checks its size and SHA-256 against what GitHub publishes for it and its
// version resource, puts it in place of the running one (renamed jolt.old.exe, deleted the next time) and offers to
// restart; the session brings the tabs back. Nothing is installed without a click, and never anything older.
// Included from app.cpp after install.inl.

extern std::wstring g_testOut;  // testmode.inl: where a test puts what it downloads
constexpr UINT WM_APP_UPDATE = WM_APP + 20;
constexpr UINT_PTR kTimerUpdate = 20;
constexpr wchar_t kUpdApi[] = L"https://api.github.com/repos/SynrgStudio/jolt/releases/latest";
constexpr wchar_t kUpdDownload[] = L"https://github.com/SynrgStudio/jolt/releases/download/";  // + tag + /jolt.exe
constexpr wchar_t kUpdPage[] = L"https://github.com/SynrgStudio/jolt/releases/latest";
constexpr wchar_t kUpdReg[] = L"Software\\Jolt";

struct UpdInfo {
    std::wstring version, url, sha;  // "0.5.0", the exact asset URL, its SHA-256 in hex
    uint64_t size = 0;
};
struct UpdMsg {
    int kind = 0;  // 0 checked, 1 the check failed, 2 downloading (percent), 3 downloaded and in place, 4 the download failed
    bool manual = false, newer = false;
    UpdInfo info;
    std::wstring error;
    int percent = 0;
};
struct {
    bool checking = false, downloading = false, ready = false;
    UpdInfo offered;
} g_upd;

// ---- versions: three numbers, compared as numbers
bool UpdParseVersion(std::wstring s, int v[3]) {
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s.erase(0, 1);
    int got = 0;
    size_t p = 0;
    for (; got < 3; ++got) {
        size_t q = p;
        while (q < s.size() && iswdigit(s[q])) ++q;
        if (q == p || q - p > 6) return false;
        v[got] = _wtoi(s.substr(p, q - p).c_str());
        p = q;
        if (got < 2) {
            if (p >= s.size() || s[p] != L'.') return false;
            ++p;
        }
    }
    return p == s.size();
}
int UpdCompare(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
std::wstring UpdCurrent() {  // tests can pretend to be older, to try it against the real latest release
    if (g_test) {
        wchar_t buf[32];
        if (GetEnvironmentVariableW(L"JOLT_FAKE_VERSION", buf, 32)) return buf;
    }
    return JOLT_VERSION_W;
}

// ---- a small JSON reader for GitHub's answer: bounded in depth and size, UTF-8 in, wide strings out
struct UpdJson {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<UpdJson> items;                            // Arr
    std::vector<std::pair<std::wstring, UpdJson>> fields;  // Obj
    const UpdJson* get(const wchar_t* k) const {
        for (const auto& [name, v] : fields)
            if (name == k) return &v;
        return nullptr;
    }
    std::wstring str(const wchar_t* k) const {
        const UpdJson* v = get(k);
        return v && v->type == Str ? v->s : std::wstring();
    }
};
struct UpdJsonReader {
    const char *p, *end;
    int nodes = 0;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool hex4(const char* at, unsigned& out) const {
        if (end - at < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = at[i];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            out = out * 16 + static_cast<unsigned>(d);
        }
        return true;
    }
    bool str(std::wstring& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        std::string bytes;
        while (p < end && *p != '"') {
            if (*p != '\\') {
                bytes += *p++;
                continue;
            }
            if (++p >= end) return false;
            const char c = *p++;
            switch (c) {
                case '"': case '\\': case '/': bytes += c; break;
                case 'b': bytes += '\b'; break;
                case 'f': bytes += '\f'; break;
                case 'n': bytes += '\n'; break;
                case 'r': bytes += '\r'; break;
                case 't': bytes += '\t'; break;
                case 'u': {
                    unsigned cp = 0, lo = 0;
                    if (!hex4(p, cp)) return false;
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, lo) && lo >= 0xDC00 && lo < 0xE000) {  // a surrogate pair
                        p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    // as UTF-8, like the rest
                    if (cp < 0x80) bytes += static_cast<char>(cp);
                    else if (cp < 0x800) bytes += {static_cast<char>(0xC0 | cp >> 6), static_cast<char>(0x80 | (cp & 0x3F))};
                    else if (cp < 0x10000)
                        bytes += {static_cast<char>(0xE0 | cp >> 12), static_cast<char>(0x80 | (cp >> 6 & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F))};
                    else
                        bytes += {static_cast<char>(0xF0 | cp >> 18), static_cast<char>(0x80 | (cp >> 12 & 0x3F)), static_cast<char>(0x80 | (cp >> 6 & 0x3F)),
                                  static_cast<char>(0x80 | (cp & 0x3F))};
                    break;
                }
                default: return false;
            }
        }
        if (p >= end) return false;
        ++p;
        const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        out.assign(static_cast<size_t>(std::max(0, n)), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
        return true;
    }
    bool value(UpdJson& v, int depth) {
        if (depth > 32 || ++nodes > 200000) return false;
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            v.type = UpdJson::Obj;
            ++p;
            ws();
            if (p < end && *p == '}') return ++p, true;
            for (;;) {
                std::wstring k;
                ws();
                if (!str(k)) return false;
                ws();
                if (p >= end || *p++ != ':') return false;
                UpdJson item;
                if (!value(item, depth + 1)) return false;
                v.fields.push_back({std::move(k), std::move(item)});
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                return p < end && *p++ == '}';
            }
        }
        if (*p == '[') {
            v.type = UpdJson::Arr;
            ++p;
            ws();
            if (p < end && *p == ']') return ++p, true;
            for (;;) {
                UpdJson item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                return p < end && *p++ == ']';
            }
        }
        if (*p == '"') {
            v.type = UpdJson::Str;
            return str(v.s);
        }
        auto word = [&](const char* w) {
            const size_t n = strlen(w);
            if (static_cast<size_t>(end - p) < n || strncmp(p, w, n) != 0) return false;
            p += n;
            return true;
        };
        if (word("true")) return v.type = UpdJson::Bool, v.b = true, true;
        if (word("false")) return v.type = UpdJson::Bool, v.b = false, true;
        if (word("null")) return v.type = UpdJson::Null, true;
        const char* s = p;
        while (p < end && (isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) ++p;
        if (p == s) return false;
        v.type = UpdJson::Num;
        v.n = atof(std::string(s, p).c_str());
        return true;
    }
};

// ---- HTTPS with WinHTTP: no cookies, no automatic sign-in, redirects followed (never to plain HTTP), limited in time
// and size. sink takes each piece (false stops); total is the expected size, if the server said.
bool UpdGet(const std::wstring& url, const wchar_t* accept, uint64_t most, const std::function<bool(const BYTE*, DWORD, uint64_t)>& sink,
            DWORD& status, std::wstring& err) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"not an HTTPS address";
        return false;
    }
    const std::wstring agent = std::wstring(L"Jolt/") + JOLT_VERSION_W;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        err = L"WinHTTP isn't available";
        return false;
    }
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = false;
    if (r) {
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
        if (WinHttpSendRequest(r, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            wchar_t len[32] = {};
            DWORD lsize = sizeof(len);
            uint64_t total = 0;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsize, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(len, nullptr, 10);
            ok = status == 200;
            uint64_t got = 0;
            std::vector<BYTE> buf(65536);
            while (ok) {
                DWORD avail = 0, read = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) {
                    ok = false;
                    err = L"the connection was cut";
                    break;
                }
                if (!avail) break;
                if (!WinHttpReadData(r, buf.data(), std::min<DWORD>(avail, static_cast<DWORD>(buf.size())), &read) || !read) {
                    ok = false;
                    err = L"the connection was cut";
                    break;
                }
                got += read;
                if (got > most) {
                    ok = false;
                    err = L"it was bigger than it should be";
                    break;
                }
                if (!sink(buf.data(), read, total)) {
                    ok = false;
                    break;
                }
            }
            if (status != 200 && err.empty()) err = status == 403 || status == 429 ? L"GitHub is limiting requests: try later" : L"GitHub answered " + std::to_wstring(status);
        } else {
            err = L"GitHub couldn't be reached";
        }
    } else {
        err = L"GitHub couldn't be reached";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

void UpdPost(UpdMsg* m) {
    if (!PostMessageW(g_hwnd, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(m))) delete m;
}

// The latest release: its version, and its jolt.exe with the size and SHA-256 GitHub gives for it.
void UpdCheckThread(bool manual) {
    auto* m = new UpdMsg;
    m->kind = 1;
    m->manual = manual;
    std::string body;
    DWORD status = 0;
    if (UpdGet(kUpdApi, L"application/vnd.github+json", 1u << 20, [&](const BYTE* d, DWORD n, uint64_t) { return body.append(reinterpret_cast<const char*>(d), n), true; },
               status, m->error)) {
        UpdJson j;
        UpdJsonReader rd{body.data(), body.data() + body.size()};
        int latest[3], current[3];
        const UpdJson* draft = nullptr;
        if (!rd.value(j, 0) || j.type != UpdJson::Obj) {
            m->error = L"GitHub's answer couldn't be read";
        } else if ((draft = j.get(L"draft")) && draft->b) {
            m->error = L"the latest release is a draft";
        } else if (const UpdJson* pre = j.get(L"prerelease"); pre && pre->b) {
            m->error = L"the latest release is a pre-release";
        } else if (!UpdParseVersion(j.str(L"tag_name"), latest) || !UpdParseVersion(UpdCurrent(), current)) {
            m->error = L"the version " + j.str(L"tag_name") + L" can't be compared";
        } else {
            const std::wstring tag = j.str(L"tag_name");
            const UpdJson* assets = j.get(L"assets");
            const UpdJson* exe = nullptr;
            int found = 0;
            static const std::vector<UpdJson> none;
            for (const UpdJson& a : assets && assets->type == UpdJson::Arr ? assets->items : none)
                if (a.str(L"name") == L"jolt.exe") {
                    exe = &a;
                    ++found;
                }
            const std::wstring digest = exe ? exe->str(L"digest") : L"";
            const UpdJson* size = exe ? exe->get(L"size") : nullptr;
            bool hex = digest.size() == 7 + 64 && digest.rfind(L"sha256:", 0) == 0;
            for (size_t i = 7; hex && i < digest.size(); ++i) hex = iswxdigit(digest[i]) != 0;
            if (found != 1 || exe->str(L"browser_download_url") != std::wstring(kUpdDownload) + tag + L"/jolt.exe" || !hex || !size ||
                size->type != UpdJson::Num || size->n < 1 || size->n > 64.0 * 1024 * 1024) {
                m->error = L"the release has no jolt.exe to trust";
            } else {
                m->kind = 0;
                m->newer = UpdCompare(latest, current) > 0;
                m->info.version = std::to_wstring(latest[0]) + L"." + std::to_wstring(latest[1]) + L"." + std::to_wstring(latest[2]);
                m->info.url = exe->str(L"browser_download_url");
                m->info.sha = Lower(digest.substr(7));
                m->info.size = static_cast<uint64_t>(size->n);
            }
        }
    } else if (status == 404) {
        m->error = L"there are no releases yet";
    }
    UpdPost(m);
}

// The release's jolt.exe: downloaded next to the running one, checked (size, SHA-256, version resource), and put in
// its place. In test mode it only goes to the output folder and is checked.
void UpdDownloadThread(UpdInfo info) {
    auto* m = new UpdMsg;
    m->kind = 4;
    m->info = info;
    const std::wstring exe = ExePath(), dir = ParentOf(exe);
    const std::wstring fresh = g_test ? g_testOut + L"\\jolt-update.exe" : dir + L"\\jolt.new.exe";
    HANDLE f = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = f != INVALID_HANDLE_VALUE && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    if (f == INVALID_HANDLE_VALUE) m->error = L"Jolt's folder can't be written (" + dir + L"): get it from the releases page";
    uint64_t got = 0;
    int last = -1;
    DWORD status = 0;
    if (ok)
        ok = UpdGet(info.url, L"application/octet-stream", info.size,
                    [&](const BYTE* d, DWORD n, uint64_t) {
                        DWORD wrote = 0;
                        if (!WriteFile(f, d, n, &wrote, nullptr) || wrote != n) return false;
                        BCryptHashData(hash, const_cast<PUCHAR>(d), n, 0);
                        got += n;
                        const int pct = static_cast<int>(got * 100 / std::max<uint64_t>(1, info.size));
                        if (pct != last) {
                            auto* p = new UpdMsg;
                            p->kind = 2;
                            p->percent = last = pct;
                            p->info = info;
                            UpdPost(p);
                        }
                        return true;
                    },
                    status, m->error);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    std::wstring sha;
    if (hash) {
        UCHAR h[32];
        if (BCRYPT_SUCCESS(BCryptFinishHash(hash, h, 32, 0)))
            for (UCHAR b : h) {
                wchar_t x[3];
                swprintf(x, 3, L"%02x", b);
                sha += x;
            }
        BCryptDestroyHash(hash);
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (ok && got != info.size) ok = false, m->error = L"it came incomplete";
    if (ok && sha != info.sha) ok = false, m->error = L"it isn't the file GitHub published (its SHA-256 differs)";
    if (ok) {  // and it says it is that Jolt
        DWORD ignore = 0;
        const DWORD n = GetFileVersionInfoSizeW(fresh.c_str(), &ignore);
        std::vector<BYTE> vi(n);
        VS_FIXEDFILEINFO* fi = nullptr;
        wchar_t* product = nullptr;
        UINT len = 0, plen = 0;
        int v[3] = {};
        UpdParseVersion(info.version, v);
        ok = n && GetFileVersionInfoW(fresh.c_str(), 0, n, vi.data()) && VerQueryValueW(vi.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi &&
             HIWORD(fi->dwFileVersionMS) == static_cast<WORD>(v[0]) && LOWORD(fi->dwFileVersionMS) == static_cast<WORD>(v[1]) &&
             HIWORD(fi->dwFileVersionLS) == static_cast<WORD>(v[2]) &&
             VerQueryValueW(vi.data(), L"\\StringFileInfo\\040904B0\\ProductName", reinterpret_cast<void**>(&product), &plen) && product &&
             wcscmp(product, L"Jolt") == 0;
        if (!ok) m->error = L"the file doesn't say it is Jolt " + info.version;
    }
    if (ok && !g_test) {  // in place of the running one, which keeps running from its new name
        const std::wstring old = dir + L"\\jolt.old.exe";
        DeleteFileW(old.c_str());
        if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ok = false;
            m->error = L"the running Jolt couldn't be set aside";
        } else if (!MoveFileExW(fresh.c_str(), exe.c_str(), 0)) {
            MoveFileExW(old.c_str(), exe.c_str(), 0);  // as it was
            ok = false;
            m->error = L"the new Jolt couldn't be put in place";
        }
    }
    if (!ok) DeleteFileW(fresh.c_str());
    if (ok) m->kind = 3;
    UpdPost(m);
}

void UpdCheckNow(bool manual) {
    if (g_upd.checking || g_upd.downloading) return;
    g_upd.checking = true;
    if (manual) {
        S.message = L"Asking GitHub for the latest Jolt…";
        Invalidate();
    }
    std::thread(UpdCheckThread, manual).detach();
}

void UpdRestart();
void UpdDownload() {
    if (g_upd.ready) {
        UpdRestart();
        return;
    }
    if (g_upd.offered.version.empty()) {
        UpdCheckNow(true);
        return;
    }
    if (g_upd.downloading) return;
    g_upd.downloading = true;
    S.message = L"Downloading Jolt " + g_upd.offered.version + L"…";
    S.link.clear();
    Invalidate();
    std::thread(UpdDownloadThread, g_upd.offered).detach();
}

// The new Jolt is in place: start it (it waits for this one to close) and close this one; the session brings the tabs back.
void UpdRestart() {
    if (g_test) {
        S.message = L"(test) would restart into the new Jolt";
        Invalidate();
        return;
    }
    const std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" --after-update=" + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ParentOf(exe).c_str(), &si, &pi)) {
        S.message = L"The new Jolt couldn't be started: it will be the one next time";
        Invalidate();
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SaveSession();
    BgQuit();
}

void UpdMessage(UpdMsg* raw) {
    std::unique_ptr<UpdMsg> m(raw);
    switch (m->kind) {
        case 0: {
            g_upd.checking = false;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            RegWrite(kUpdReg, L"UpdateChecked", std::to_wstring((static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime));
            if (m->newer) {
                g_upd.offered = m->info;
                S.message = L"Jolt " + m->info.version + L" is out (you have " + UpdCurrent() + L"): click here to update";
                S.link = S.message;
                g_statusLink = [] { UpdDownload(); };
            } else if (m->manual) {
                S.message = L"Jolt " + UpdCurrent() + L" is the latest";
            }
            break;
        }
        case 1:
            g_upd.checking = false;
            if (m->manual) S.message = L"Couldn't check for updates: " + (m->error.empty() ? std::wstring(L"GitHub couldn't be reached") : m->error);
            break;
        case 2:
            if (g_upd.downloading) S.message = L"Downloading Jolt " + m->info.version + L"… " + std::to_wstring(m->percent) + L"%";
            break;
        case 3:
            g_upd.downloading = false;
            g_upd.ready = true;
            S.message = g_test ? L"(test) Jolt " + m->info.version + L" downloaded and checked"
                               : L"Jolt " + m->info.version + L" is ready: click here to restart into it (or it opens next time)";
            S.link = S.message;
            g_statusLink = [] { UpdRestart(); };
            break;
        case 4:
            g_upd.downloading = false;
            S.message = L"The update couldn't be downloaded: " + m->error;
            S.link = S.message;
            g_statusLink = [] { ShellExecuteW(nullptr, L"open", kUpdPage, nullptr, nullptr, SW_SHOWNORMAL); };
            break;
    }
    Invalidate();
}

// Once a day at most, while checking is on (the clock is kept in the registry, so restarts don't add checks).
void UpdTick() {
    SetTimer(g_hwnd, kTimerUpdate, 60 * 60 * 1000, nullptr);  // and again in an hour, for a Jolt that stays open
    if (!g_cfg.checkUpdates || g_test || g_adminWindow) return;
    std::wstring last;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t t = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (RegRead(kUpdReg, L"UpdateChecked", last) && t - _wcstoui64(last.c_str(), nullptr, 10) < 24ull * 3600 * 10000000) return;
    UpdCheckNow(false);
}

// At start: what an update left behind goes, and a few seconds later the first check. The installed copy, when
// checking is off and it was never offered, offers it once.
void UpdStartup() {
    if (g_adminWindow) return;
    DeleteFileW((ParentOf(ExePath()) + L"\\jolt.old.exe").c_str());
    DeleteFileW((ParentOf(ExePath()) + L"\\jolt.new.exe").c_str());
    SetTimer(g_hwnd, kTimerUpdate, 6000, nullptr);
}
void UpdOffer() {
    std::wstring offered;
    if (g_test || g_adminWindow || g_cfg.checkUpdates || Key(ExePath()) != Key(InstExe()) || RegRead(kUpdReg, L"UpdateOffered", offered)) return;
    RegWrite(kUpdReg, L"UpdateOffered", L"1");
    if (!S.message.empty()) return;  // not over something else; it was offered all the same
    S.message = L"Jolt can tell you when there's a new version (it asks GitHub once a day): click here to turn it on";
    S.link = S.message;
    g_statusLink = [] {
        g_cfg.checkUpdates = true;
        ConfigSetValue(L"general", L"check_updates", L"yes");
        RefreshConfigSig();
        UpdCheckNow(true);
    };
    Invalidate();
}
void UpdTimer() {
    UpdOffer();
    UpdTick();
}


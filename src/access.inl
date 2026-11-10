// Folders that can't be read: why, in grey next to the name, and the way in if there is one. The folder's own page
// says the same when there's no row to put it on (a typed path, a favorite, a network location).
// Included from app.cpp before the list is drawn.

struct FolderNote {
    DWORD error;
    std::wstring target;  // a protected link: the folder it really is
    bool network = false;
};
std::unordered_map<std::wstring, FolderNote> g_folderNotes;  // Key(path) -> why
std::vector<std::wstring> g_notePaths;                       // what each FolderNote hit acts on, rebuilt every frame
void OpenAsAdmin(const std::wstring& path);                  // background.inl
void TestLog(const std::wstring& s);

// If it's a junction or a symbolic link, the folder it really is.
std::wstring LinkTarget(const std::wstring& path) {
    const std::wstring ext = ToExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(ext.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return L"";
    HANDLE h = CreateFileW(ext.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    wchar_t buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= 4096) return L"";
    std::wstring s(buf, n);
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) s = L"\\\\" + s.substr(8);
    else if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    s = Normalize(s);
    return Key(s) == Key(Normalize(path)) ? L"" : s;
}

void FolderNoteSet(const std::wstring& path, DWORD error) {
    const std::wstring k = Key(Normalize(path));
    auto it = g_folderNotes.find(k);
    if (!error) {
        if (it == g_folderNotes.end()) return;
        g_folderNotes.erase(it);
    } else {
        if (it != g_folderNotes.end() && it->second.error == error) return;
        const bool network = path.rfind(L"\\\\", 0) == 0;
        g_folderNotes[k] = {error, error == ERROR_ACCESS_DENIED && !network ? LinkTarget(path) : L"", network};
    }
    Invalidate();
}

// A listing arrived: kept if it could be read; if not, why.
void RememberListing(const std::wstring& k, const std::shared_ptr<const Listing>& l) {
    if (l->error == 0) {
        g_cache[k] = l;
        NetRememberPath(l->path);  // only what answered: a name typed wrong doesn't stay in the list
    } else if (l->path.rfind(L"\\\\", 0) == 0 && l->error != ERROR_ACCESS_DENIED && l->error != ERROR_LOGON_FAILURE) {
        // nobody there: not even recents keeps it. One that asks for a password does exist, so that one stays
        const std::wstring key = Key(l->path);
        g_recents.erase(std::remove_if(g_recents.begin(), g_recents.end(), [&](const std::wstring& r) { return Key(r) == key; }), g_recents.end());
        if (Key(g_heatPending) == key) g_heatPending.clear();
        HeatForget(l->path);
    }
    FolderNoteSet(l->path, l->error);
}

// Next to the name.
std::wstring FolderWhy(const FolderNote& n) {
    switch (n.error) {
        case ERROR_ACCESS_DENIED: return n.target.empty() ? L"No access" : L"Protected link";
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return L"No longer here";
        case ERROR_NOT_READY: return L"Drive not ready";
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_NETNAME_DELETED:
        case ERROR_NETWORK_UNREACHABLE: return L"Network location unreachable";
        case ERROR_LOGON_FAILURE: return L"Asks for a user and password";
    }
    return ErrorText(n.error);
}

// When the folder itself is what's on screen.
std::wstring FolderWhyLong(const FolderNote& n) {
    switch (n.error) {
        case ERROR_ACCESS_DENIED:
            return n.target.empty() ? L"You don't have permission to open this folder" : L"This is a protected link to " + n.target;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return L"This folder no longer exists";
        case ERROR_NOT_READY: return L"The drive isn't ready";
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_NETNAME_DELETED:
        case ERROR_NETWORK_UNREACHABLE: return L"This network location can't be reached";
        case ERROR_LOGON_FAILURE: return L"This network location asks for a user and password";
    }
    return L"Couldn't open: " + ErrorText(n.error);
}

// Windows' compatibility links (Application Data, My Documents...) refuse everyone, administrators too: they go to the
// real folder. Anything else that's denied may open as administrator.
std::wstring FolderAction(const FolderNote& n) {
    if (n.network && (n.error == ERROR_ACCESS_DENIED || n.error == ERROR_LOGON_FAILURE)) return L"Sign in";
    if (!n.target.empty()) return L"Go to the real folder";
    return n.error == ERROR_ACCESS_DENIED && !g_elevated ? L"Open as administrator" : L"";
}

// Windows' own dialog for a user and password, for a share or for the server (its IPC$ share is what signing in to it is).
void NetSignIn(const std::wstring& path) {
    if (g_test) {
        TestLog(L"sign in: " + path);
        return;
    }
    size_t end = path.find(L'\\', 2);
    if (end != std::wstring::npos) end = path.find(L'\\', end + 1);
    std::wstring res = IsServerPath(path) ? path + L"\\IPC$" : path.substr(0, end);
    NETRESOURCEW nr{};
    nr.dwType = RESOURCETYPE_ANY;
    nr.lpRemoteName = res.data();
    const DWORD e = WNetAddConnection3W(g_hwnd, &nr, nullptr, nullptr, CONNECT_INTERACTIVE | CONNECT_PROMPT);
    if (e == NO_ERROR) Navigate(T(), path, false);
    else if (e != ERROR_CANCELLED) S.message = L"Couldn't sign in: " + ErrorText(e);
    Invalidate();
}

void FolderNoteAct(const std::wstring& path) {
    auto it = g_folderNotes.find(Key(Normalize(path)));
    if (it != g_folderNotes.end() && it->second.network) NetSignIn(path);
    else if (it != g_folderNotes.end() && !it->second.target.empty()) Navigate(T(), it->second.target, true);
    else OpenAsAdmin(path);
}

// Enter on a folder that can't be read: better to stay here and say why next to its name. Only where asking is
// instant; a network location goes in and says it on its page.
bool CantEnter(const std::wstring& path) {
    if (g_cache.count(Key(path)) || !IsLocalFixed(path) || InArchive(path)) return false;
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return false;
    }
    FolderNoteSet(path, GetLastError());
    if (LayoutOf(T())) {  // the cells have no room for the note
        const FolderNote& n = g_folderNotes[Key(Normalize(path))];
        const std::wstring act = FolderAction(n);
        S.message = FolderWhyLong(n) + (act.empty() ? L"" : L" (" + act + L": Ctrl+Shift+Enter)");
    }
    return true;
}

// The note on a row, from x. Returns where the row may go on.
float DrawFolderNote(const std::wstring& full, float x, float xMax, float y, float a, bool onAccent) {
    auto it = g_folderNotes.find(Key(Normalize(full)));
    if (it == g_folderNotes.end()) return x;
    const std::wstring why = FolderWhy(it->second), act = FolderAction(it->second);
    const float w = MeasureW(why, g_fmtName.Get());
    if (x + w > xMax) return x;
    Color(onAccent ? 0x9FB3D9 : cFaint, a);
    Text(why, g_fmtName.Get(), x, y, xMax, y + kRow);
    x += w + 14;
    if (act.empty()) return x;
    const float aw = MeasureW(act, g_fmtName.Get());
    if (x + aw > xMax) return x;
    const int idx = static_cast<int>(g_notePaths.size());
    const bool hot = IsHover(HitKind::FolderNote, idx);
    const D2D1_RECT_F r = D2D1::RectF(x - 6, y + 3, x + aw + 6, y + kRow - 3);
    if (hot) Round(r, 4, 0xFFFFFF, 0.1f);
    Color(hot ? cText : onAccent ? 0xC6D4EE : cDim, a);
    Text(act, g_fmtName.Get(), x, y, xMax, y + kRow);
    g_hits.push_back({r, HitKind::FolderNote, idx});
    g_notePaths.push_back(full);
    return x + aw + 14;
}

// The list of a folder that couldn't be read.
void DrawFolderError(const Tab& t, float L, float R, float top) {
    auto it = g_folderNotes.find(Key(Normalize(t.path)));
    const FolderNote n = it != g_folderNotes.end() ? it->second : FolderNote{t.list->error, L""};
    Color(cDim);
    Text(FolderWhyLong(n), g_fmtCenter.Get(), L + kPad, top + 44, R - kPad, top + 72);
    const std::wstring act = FolderAction(n);
    if (act.empty()) return;
    const float w = MeasureW(act, g_fmtCenter.Get()) + 32, mid = (L + R) / 2;
    const D2D1_RECT_F r = D2D1::RectF(std::floor(mid - w / 2), top + 82, std::floor(mid + w / 2), top + 112);
    const int idx = static_cast<int>(g_notePaths.size());
    Round(r, 6, 0xFFFFFF, IsHover(HitKind::FolderNote, idx) ? 0.14f : 0.08f);
    Color(cText);
    Text(act, g_fmtCenter.Get(), r.left, r.top, r.right, r.bottom);
    g_hits.push_back({r, HitKind::FolderNote, idx});
    g_notePaths.push_back(t.path);
}

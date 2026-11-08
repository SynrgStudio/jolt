// Templates: the files and folders in %APPDATA%\Jolt\Templates. Each one is an entry of the "New" submenu, of
// Ctrl+K ("New: name") and of the new_from_template command: a copy of it lands in the current folder, named like
// the template, ready to rename. Included from app.cpp before menu.inl.

std::wstring TemplatesDir() {
    const std::wstring d = ConfigDir();
    return d.empty() ? L"" : d + L"\\Templates";
}

struct TemplateDef {
    std::wstring name;
    bool isDir;
};

std::vector<TemplateDef> Templates() {
    std::vector<TemplateDef> out;
    const std::wstring d = TemplatesDir();
    if (d.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        out.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const TemplateDef& a, const TemplateDef& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

// The folder, in a new tab; made the first time.
void OpenTemplatesFolder() {
    const std::wstring d = TemplatesDir();
    if (d.empty()) return;
    if (GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(d.c_str(), nullptr);
        S.message = L"Put files or folders here: each one becomes an entry of New";
    }
    NewTab(d);
}

bool CopyTree(const std::wstring& from, const std::wstring& to) {
    if (!CreateDirectoryW(ToExtendedPath(to).c_str(), nullptr)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(ToExtendedPath(from + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring a = from + L"\\" + fd.cFileName, b = to + L"\\" + fd.cFileName;
        ok = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? CopyTree(a, b) : CopyFileW(ToExtendedPath(a).c_str(), ToExtendedPath(b).c_str(), TRUE) != 0;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// A copy of the template in the current folder, named like it (or "name (2)"), and renaming starts.
void NewFromTemplate(const std::wstring& name) {
    Tab& t = T();
    if (t.path.empty() || IsVirtual(t.path) || !t.list || t.list->error || TestRefuses(t.path)) return;
    const std::wstring src = TemplatesDir() + L"\\" + name;
    const DWORD attrs = GetFileAttributesW(src.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        S.message = L"That template is gone: " + name;
        Invalidate();
        return;
    }
    const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t extLen = dir ? 0 : ExtOf(name.c_str(), name.size()).size();
    const std::wstring fresh = FreeName(t.path, name.substr(0, name.size() - extLen), name.substr(name.size() - extLen));
    const std::wstring full = Join(t.path, fresh.c_str());
    const bool ok = dir ? CopyTree(src, full) : CopyFileW(src.c_str(), ToExtendedPath(full).c_str(), TRUE) != 0;
    if (!ok) {
        S.message = L"Couldn't create it from the template: " + ErrorText(GetLastError());
        Invalidate();
        return;
    }
    UndoRecordCreate(full);
    if (!t.filter.empty()) {
        t.filter.clear();
        BuildRows(t);
    }
    t.pendingSelect = fresh;
    g_renameAfterSelect = fresh;
    Enqueue(t.path, JobKind::Revalidate);
}

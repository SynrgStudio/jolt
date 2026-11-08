// In use (Ctrl+K → "in use") and Unblock (Ctrl+K → "unblock"). Included from app.cpp after attrs.inl.
//
// In use: which programs have the selection open, asked to the Restart Manager, in a panel over the window with a
// Close button per program (its windows are asked to close; a program without windows is ended; one that doesn't
// go gets a Force button). A copy, move, delete or rename that fails lands here too, with the reason: if something
// holds the file, the panel offers to close it and goes again by itself once the holders are gone.
// Unblock: drops the Zone.Identifier stream (the "downloaded from the internet" mark) from the selection.

#include <RestartManager.h>
#include <functional>
#pragma comment(lib, "rstrtmgr.lib")

constexpr UINT_PTR kTimerInUse = 16;

struct Holder {
    DWORD pid = 0;
    FILETIME started{};
    std::wstring app, exe;            // what the Restart Manager calls it, and the executable
    int type = RmUnknownApp;          // a service, Explorer or a critical process isn't closed from here
    bool windows = false;             // has visible windows: Close asks them; otherwise End
    std::vector<std::wstring> files;  // which of the asked files it holds (empty: asked all at once)
    int state = 0;                    // 0 nothing asked, 1 asked to close, 2 asked to end
    DWORD askedAt = 0;
};

enum class IuK { None, Act, Retry, Done, Close };
struct IuHit {
    D2D1_RECT_F r;
    IuK k;
    int idx = 0;  // Act: the holder
};

struct InUsePanel {
    bool open = false;
    std::wstring title, sub;  // "In use" and the file, or what failed and why
    std::vector<std::wstring> files;
    std::vector<Holder> holders;
    std::function<void()> retry;  // the operation that failed, to go again
    D2D1_RECT_F box{};
    std::vector<IuHit> hits;
    IuHit hover{};
    bool hovering = false;
} g_iu;

constexpr float kIuW = 560.f, kIuRow = 50.f;
constexpr int kIuMaxRows = 8;

struct IuWndCtx {
    DWORD pid;
    bool post, any;
};
BOOL CALLBACK IuWndProc(HWND w, LPARAM lp) {
    IuWndCtx* c = reinterpret_cast<IuWndCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == c->pid && !GetWindow(w, GW_OWNER) && IsWindowVisible(w)) {  // its top-level windows, as the user sees them
        if (c->post) PostMessageW(w, WM_CLOSE, 0, 0);
        c->any = true;
    }
    return TRUE;
}
bool IuWindows(DWORD pid, bool close) {
    IuWndCtx c{pid, close, false};
    EnumWindows(IuWndProc, reinterpret_cast<LPARAM>(&c));
    return c.any;
}

// The processes with any of these files open. One Restart Manager session per file keeps which process holds
// what, up to a handful of files; past that, one session for all of them.
std::vector<Holder> HoldersOf(const std::vector<std::wstring>& files) {
    std::vector<Holder> out;
    auto query = [&](const std::vector<std::wstring>& group) {
        DWORD session = 0;
        wchar_t key[CCH_RM_SESSION_KEY + 1] = {};
        if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return;
        std::vector<LPCWSTR> ptrs;
        for (const std::wstring& f : group) ptrs.push_back(f.c_str());
        UINT needed = 0, count = 0;
        DWORD reasons = 0;
        if (RmRegisterResources(session, static_cast<UINT>(ptrs.size()), ptrs.data(), 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
            std::vector<RM_PROCESS_INFO> info;
            DWORD rc = RmGetList(session, &needed, &count, nullptr, &reasons);
            if (rc == ERROR_MORE_DATA && needed) {
                info.resize(needed);
                count = needed;
                rc = RmGetList(session, &needed, &count, info.data(), &reasons);
            }
            if (rc == ERROR_SUCCESS) {
                for (UINT i = 0; i < count; ++i) {
                    const RM_PROCESS_INFO& p = info[i];
                    Holder* h = nullptr;
                    for (Holder& e : out)
                        if (e.pid == p.Process.dwProcessId && CompareFileTime(&e.started, &p.Process.ProcessStartTime) == 0) h = &e;
                    if (!h) {
                        Holder n;
                        n.pid = p.Process.dwProcessId;
                        n.started = p.Process.ProcessStartTime;
                        n.app = p.strAppName;
                        n.type = p.ApplicationType;
                        if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, n.pid)) {
                            wchar_t buf[MAX_PATH * 2];
                            DWORD len = _countof(buf);
                            if (QueryFullProcessImageNameW(ph, 0, buf, &len)) n.exe = buf;
                            CloseHandle(ph);
                        }
                        if (n.app.empty()) n.app = n.exe.empty() ? L"PID " + std::to_wstring(n.pid) : LeafOf(n.exe);
                        n.windows = IuWindows(n.pid, false);
                        out.push_back(std::move(n));
                        h = &out.back();
                    }
                    if (group.size() == 1) h->files.push_back(group[0]);
                }
            }
        }
        RmEndSession(session);
    };
    if (files.size() <= 8) {
        for (const std::wstring& f : files) query({f});
    } else {
        query(files);
    }
    return out;
}

// Every file under a folder (no reparse points), up to cap.
void CollectFiles(const std::wstring& dir, std::vector<std::wstring>& out, size_t cap) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToExtendedPath(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (out.size() >= cap) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring p = Join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CollectFiles(p, out, cap);
        else out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Folders become the files in them: the Restart Manager answers for files.
std::vector<std::wstring> IuFilesOf(const std::vector<std::wstring>& sel) {
    std::vector<std::wstring> files;
    for (const std::wstring& p : sel) {
        const DWORD a = GetFileAttributesW(ToExtendedPath(p).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) CollectFiles(p, files, 2000);
        else files.push_back(p);
    }
    return files;
}

std::wstring IuWhat(const std::vector<std::wstring>& files) {
    return files.size() == 1 ? LeafOf(files[0]) : Thousands(static_cast<int>(files.size())) + L" items";
}

// What an HRESULT from the copy engine means, in words.
std::wstring HrText(HRESULT hr) {
    switch (hr) {  // sherrors.h
        case COPYENGINE_E_SHARING_VIOLATION_SRC: case COPYENGINE_E_SHARING_VIOLATION_DEST: return L"it's open in another program";
        case COPYENGINE_E_ACCESS_DENIED_SRC: case COPYENGINE_E_ACCESS_DENIED_DEST: case COPYENGINE_E_ACCESSDENIED_READONLY: return L"access denied";
        case COPYENGINE_E_SAME_FILE: return L"the source and destination are the same";
        case COPYENGINE_E_DEST_SUBTREE: case COPYENGINE_E_DEST_SAME_TREE: return L"the destination is inside the source";
        case COPYENGINE_E_FLD_IS_FILE_DEST: case COPYENGINE_E_FILE_IS_FLD_DEST: return L"something with that name is in the way";
        case COPYENGINE_E_FILE_TOO_LARGE: return L"the file is too large for the destination";
        case COPYENGINE_E_PATH_TOO_DEEP_SRC: case COPYENGINE_E_PATH_TOO_DEEP_DEST: case COPYENGINE_E_NEWFILE_NAME_TOO_LONG:
        case COPYENGINE_E_NEWFOLDER_NAME_TOO_LONG: return L"the path is too long";
        case COPYENGINE_E_DISK_FULL: case COPYENGINE_E_DISK_FULL_CLEAN: case COPYENGINE_E_REMOVABLE_FULL: return L"the disk is full";
        case COPYENGINE_E_PATH_NOT_FOUND_SRC: return L"it no longer exists";
        case COPYENGINE_E_PATH_NOT_FOUND_DEST: return L"the destination no longer exists";
        case COPYENGINE_E_NET_DISCONNECT_SRC: case COPYENGINE_E_NET_DISCONNECT_DEST: return L"the network location is gone";
        case COPYENGINE_E_DIR_NOT_EMPTY: return L"the folder isn't empty";
        case COPYENGINE_E_CANNOT_MOVE_FROM_RECYCLE_BIN: return L"it can't be moved out of the Recycle Bin";
        case COPYENGINE_E_CANNOT_MOVE_SHARED_FOLDER: return L"a shared folder can't be moved";
        case COPYENGINE_E_ROOT_DIR_SRC: case COPYENGINE_E_ROOT_DIR_DEST: return L"not on a drive's root";
        default: break;
    }
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) return ErrorText(HRESULT_CODE(hr));
    wchar_t b[32];
    swprintf_s(b, L"error 0x%08X", static_cast<unsigned>(hr));
    return b;
}

void InUseClose() {
    if (!g_iu.open) return;
    KillTimer(g_hwnd, kTimerInUse);
    g_iu.open = false;
    g_iu.files.clear();
    g_iu.holders.clear();
    g_iu.hits.clear();
    g_iu.retry = nullptr;
    Invalidate();
}

// Asks again who holds the files, keeping what was asked of each process. Once nobody does, a pending operation
// goes again on its own.
void InUseRescan() {
    std::vector<Holder> now = HoldersOf(g_iu.files);
    for (Holder& h : now)
        for (const Holder& old : g_iu.holders)
            if (old.pid == h.pid && CompareFileTime(&old.started, &h.started) == 0) {
                h.state = old.state;
                h.askedAt = old.askedAt;
            }
    g_iu.holders = std::move(now);
    if (g_iu.holders.empty() && g_iu.retry) {
        std::function<void()> again = g_iu.retry;
        InUseClose();
        again();
        return;
    }
    if (g_iu.holders.empty()) g_iu.sub = L"Nothing has it open now";
    Invalidate();
}

void InUseShow(const std::wstring& title, const std::wstring& sub, const std::vector<std::wstring>& files, std::vector<Holder> holders,
               std::function<void()> retry) {
    PopupClose();
    PaletteClose();
    ViewEditorClose();
    MenuEditorClose();
    SettingsClose();
    AttrsClose();
    CancelRename();
    g_iu = InUsePanel{};
    g_iu.open = true;
    g_iu.title = title;
    g_iu.sub = sub;
    g_iu.files = files;
    g_iu.holders = std::move(holders);
    g_iu.retry = std::move(retry);
    SetTimer(g_hwnd, kTimerInUse, 1000, nullptr);  // the programs may be closed by hand too
    Invalidate();
}

// Ctrl+K → "in use": who has the selection open.
void InUseOpen() {
    Tab& t = T();
    const std::vector<std::wstring> sel = SelectedFullPaths(t);
    if (sel.empty() || IsBin(t.path)) {
        S.message = L"Select something first";
        Invalidate();
        return;
    }
    const std::vector<std::wstring> files = IuFilesOf(sel);
    std::vector<Holder> holders = HoldersOf(files);
    if (holders.empty()) {
        S.message = L"Nothing has " + IuWhat(sel) + L" open";
        Invalidate();
        return;
    }
    const std::wstring n = holders.size() == 1 ? L"open in one program" : L"open in " + std::to_wstring(holders.size()) + L" programs";
    InUseShow(L"In use", IuWhat(sel) + L": " + n, files, std::move(holders), nullptr);
}

// An operation failed on these files. If something has them open, the panel says what and offers to close it and
// go again; otherwise the reason goes to the status line.
void InUseAfterFailure(const std::wstring& what, const std::vector<std::wstring>& files, const std::wstring& reason, std::function<void()> retry) {
    std::vector<Holder> holders = HoldersOf(files);
    if (holders.empty()) {
        S.message = what + L": " + reason;
        MessageBeep(MB_ICONWARNING);
        Invalidate();
        return;
    }
    const std::wstring sub = holders.size() == 1 ? L"Open in " + holders[0].app + L". Close it and it goes again"
                                                 : L"Open in " + std::to_wstring(holders.size()) + L" programs. Close them and it goes again";
    InUseShow(what, sub, files, std::move(holders), std::move(retry));
}

// Close (its windows), End or Force (TerminateProcess).
void InUseAct(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_iu.holders.size())) return;
    Holder& h = g_iu.holders[idx];
    const bool force = h.state == 1 || !h.windows;
    if (!force) {
        IuWindows(h.pid, true);
        h.state = 1;
    } else {
        HANDLE ph = OpenProcess(PROCESS_TERMINATE, FALSE, h.pid);
        if (!ph || !TerminateProcess(ph, 1)) {
            const DWORD e = GetLastError();
            S.message = L"Couldn't end " + h.app + L": " + ErrorText(e) + (e == ERROR_ACCESS_DENIED ? L"  ·  it may need Jolt run as administrator" : L"");
        }
        if (ph) CloseHandle(ph);
        h.state = 2;
    }
    h.askedAt = GetTickCount();
    SetTimer(g_hwnd, kTimerInUse, 400, nullptr);  // sooner, while something is on its way out
    Invalidate();
}

void InUseTick() {
    if (!g_iu.open) {
        KillTimer(g_hwnd, kTimerInUse);
        return;
    }
    InUseRescan();
    if (!g_iu.open) return;
    bool waiting = false;
    for (const Holder& h : g_iu.holders) waiting |= h.state != 0;
    SetTimer(g_hwnd, kTimerInUse, waiting ? 400 : 1000, nullptr);
}

// The button of a holder: what it says, and whether it can be clicked.
std::wstring IuLabel(const Holder& h, bool& clickable) {
    clickable = false;
    if (h.pid == GetCurrentProcessId()) return L"Jolt";
    if (h.type == RmCritical) return L"system";
    if (h.type == RmService) return L"service";
    if (h.type == RmExplorer) return L"Explorer";
    clickable = true;
    if (h.state == 2) return GetTickCount() - h.askedAt < 2500 ? L"Ending…" : L"End";
    if (h.state == 1) {
        if (GetTickCount() - h.askedAt < 2500) {
            clickable = false;
            return L"Closing…";
        }
        return L"Force";
    }
    return h.windows ? L"Close" : L"End";
}

void DrawInUse(float W, float H) {
    if (!g_iu.open) return;
    g_iu.hits.clear();
    const int n = static_cast<int>(g_iu.holders.size()), shown = std::min(n, kIuMaxRows);
    const float pw = std::min(kIuW, W - 40);
    const float ph = 70 + (n ? shown * kIuRow + (n > shown ? 22 : 0) : 36) + 62;
    const float x0 = std::floor((W - pw) / 2), y0 = std::floor(std::max(kTabH + 8, (H - ph) / 2));
    const float R = x0 + pw, B = y0 + ph;
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0.45f);
    g_iu.box = D2D1::RectF(x0, y0, R, B);
    for (int i = 4; i >= 1; --i) Round(D2D1::RectF(x0 - i, y0 - i + 2, R + i, B + i + 2), 12.f + i, 0x000000, 0.10f);
    Round(g_iu.box, 12, cTop);
    Color(cBorder);
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(g_iu.box, 12, 12), g_brush.Get(), 1.f);
    auto isHover = [&](IuK k, int idx = 0) { return g_iu.hovering && g_iu.hover.k == k && g_iu.hover.idx == idx; };

    Color(g_iu.retry ? 0xE0A040 : cAccent);
    Glyph(0xE72E, g_fmtIcon.Get(), x0 + 18, y0 + 14, x0 + 42, y0 + 42);  // lock
    Color(cText);
    Text(g_iu.title, g_fmtNameBold.Get(), x0 + 48, y0 + 8, R - 60, y0 + 30);
    Color(cDim);
    Text(g_iu.sub, g_fmtStatus.Get(), x0 + 48, y0 + 27, R - 60, y0 + 48);
    const D2D1_RECT_F cb = D2D1::RectF(R - 42, y0 + 14, R - 12, y0 + 42);
    if (isHover(IuK::Close)) Round(cb, 6, 0xFFFFFF, 0.08f);
    Color(isHover(IuK::Close) ? cText : cDim);
    Glyph(0xE711, g_fmtIconSm.Get(), cb.left, cb.top, cb.right, cb.bottom);
    g_iu.hits.push_back({cb, IuK::Close});
    HLine(x0, R, y0 + 56);

    const float cl = x0 + 22, cr = R - 22;
    float y = y0 + 64;
    if (!n) {
        Color(cFaint);
        Text(L"Nothing has it open", g_fmtName.Get(), cl, y, cr, y + 30);
        y += 36;
    }
    for (int i = 0; i < shown; ++i) {
        const Holder& h = g_iu.holders[i];
        if (!h.exe.empty()) DrawIcon(h.exe, nullptr, nullptr, 0, false, cl, y + (kIuRow - kIcon) / 2 - 2, 1.f);
        bool clickable = false;
        const std::wstring label = IuLabel(h, clickable);
        const D2D1_RECT_F btn = D2D1::RectF(cr - 84, y + 10, cr, y + 38);
        Color(cText);
        Text(h.app, g_fmtNameBold.Get(), cl + 26, y + 4, btn.left - 12, y + 26);
        std::wstring detail = (h.exe.empty() ? L"" : LeafOf(h.exe) + L"  ·  ") + L"PID " + std::to_wstring(h.pid);
        if (!h.files.empty()) {
            detail += L"  ·  " + LeafOf(h.files[0]);
            if (h.files.size() > 1) detail += L" and " + std::to_wstring(h.files.size() - 1) + L" more";
        }
        Color(cDim);
        Text(detail, g_fmtStatus.Get(), cl + 26, y + 25, btn.left - 12, y + 44);
        if (clickable) {
            const bool hot = isHover(IuK::Act, i);
            const bool harsh = label == L"Force" || label == L"End";
            Round(btn, 6, harsh ? 0xC0504A : cAccent, hot ? 1.f : 0.85f);
            Color(0xFFFFFF);
            Text(label, g_fmtCenter.Get(), btn.left, btn.top, btn.right, btn.bottom);
            g_iu.hits.push_back({btn, IuK::Act, i});
        } else {
            Color(cFaint);
            Text(label, g_fmtCenter.Get(), btn.left, btn.top, btn.right, btn.bottom);
        }
        y += kIuRow;
    }
    if (n > shown) {
        Color(cFaint);
        Text(L"and " + std::to_wstring(n - shown) + L" more", g_fmtStatus.Get(), cl + 26, y, cr, y + 22);
        y += 22;
    }
    // buttons
    const D2D1_RECT_F done = D2D1::RectF(cr - 90, B - 44, cr, B - 14);
    Round(done, 6, isHover(IuK::Done) ? 0x3A3A44 : cBg);
    Color(cText);
    Text(g_iu.retry ? L"Cancel" : L"Done", g_fmtCenter.Get(), done.left, done.top, done.right, done.bottom);
    g_iu.hits.push_back({done, IuK::Done});
    if (g_iu.retry) {
        const D2D1_RECT_F retry = D2D1::RectF(done.left - 110, B - 44, done.left - 8, B - 14);
        Round(retry, 6, cAccent, isHover(IuK::Retry) ? 1.f : 0.85f);
        Color(0xFFFFFF);
        Text(L"Try again", g_fmtCenter.Get(), retry.left, retry.top, retry.right, retry.bottom);
        g_iu.hits.push_back({retry, IuK::Retry});
        Color(cFaint);
        Text(L"Enter tries again, Esc leaves it", g_fmtStatus.Get(), cl, B - 40, retry.left - 10, B - 18);
    } else {
        Color(cFaint);
        Text(L"Close asks the program; End and Force don't", g_fmtStatus.Get(), cl, B - 40, done.left - 10, B - 18);
    }
}

// ---- mouse and keyboard
int IuHitAt(float x, float y) {
    for (int i = static_cast<int>(g_iu.hits.size()) - 1; i >= 0; --i)
        if (InRect(g_iu.hits[i].r, x, y)) return i;
    return -1;
}

void InUseMouseMove(float x, float y) {
    const int h = IuHitAt(x, y);
    const bool was = g_iu.hovering;
    const IuHit old = g_iu.hover;
    g_iu.hovering = h >= 0;
    g_iu.hover = h >= 0 ? g_iu.hits[h] : IuHit{};
    if (was != g_iu.hovering || old.k != g_iu.hover.k || old.idx != g_iu.hover.idx) Invalidate();
}

void InUseRetry() {
    if (!g_iu.retry) return;
    std::function<void()> again = g_iu.retry;
    InUseClose();
    again();
}

void InUseClick(float x, float y) {
    if (!InRect(g_iu.box, x, y)) {
        InUseClose();
        return;
    }
    const int hi = IuHitAt(x, y);
    if (hi < 0) return;
    const IuHit h = g_iu.hits[hi];
    switch (h.k) {
        case IuK::Close: case IuK::Done: InUseClose(); return;
        case IuK::Retry: InUseRetry(); return;
        case IuK::Act: InUseAct(h.idx); return;
        default: break;
    }
}

void InUseKey(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE: InUseClose(); break;
        case VK_RETURN: InUseRetry(); break;
        default: break;
    }
}

// ---- Unblock: the Zone.Identifier stream off every selected file (folders: every file in them)
void Unblock() {
    Tab& t = T();
    const std::vector<std::wstring> sel = SelectedFullPaths(t);
    if (sel.empty() || IsBin(t.path)) {
        S.message = L"Select something first";
        Invalidate();
        return;
    }
    std::vector<std::wstring> files;
    for (const std::wstring& p : sel) {
        if (TestRefuses(p)) return;
        const DWORD a = GetFileAttributesW(ToExtendedPath(p).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) CollectFiles(p, files, 20000);
        else files.push_back(p);
    }
    int removed = 0, failed = 0;
    DWORD lastErr = 0;
    for (const std::wstring& f : files) {
        if (DeleteFileW((ToExtendedPath(f) + L":Zone.Identifier").c_str())) {
            ++removed;
        } else if (const DWORD e = GetLastError(); e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_INVALID_NAME) {
            ++failed;
            lastErr = e;
        }
    }
    if (!removed && !failed) S.message = L"Nothing was blocked";
    else S.message = L"Unblocked " + (removed == 1 ? L"one file" : Thousands(removed) + L" files");
    if (failed) S.message += L"; " + std::to_wstring(failed) + L" couldn't be: " + ErrorText(lastErr);
    Invalidate();
}

// The network. Network lists computers and \\server lists its shares: neither is a folder Windows can enumerate as such.
// Finding computers is slow (Windows took 30 seconds on a home network, and most of what it finds are printers and
// televisions), so the list starts with what is known and grows as they turn up.

std::mutex g_netM;
std::map<std::wstring, std::wstring> g_netNames;  // Key(name) -> name
std::atomic<bool> g_netLooking{false};
double g_netLookedAt = -1e9;

void NetRemember(const std::wstring& name) {
    if (name.empty()) return;
    std::lock_guard lk(g_netM);
    g_netNames.emplace(Key(name), name);
}

// The server of a \\server\... path, so it is in the list next time.
void NetRememberPath(const std::wstring& path) {
    if (path.rfind(L"\\\\", 0) != 0) return;
    const size_t end = path.find(L'\\', 2);
    NetRemember(path.substr(2, end == std::wstring::npos ? std::wstring::npos : end - 2));
}

void NetAddMapped() {
    HANDLE h = nullptr;
    if (WNetOpenEnumW(RESOURCE_REMEMBERED, RESOURCETYPE_DISK, 0, nullptr, &h) != NO_ERROR) return;
    alignas(8) unsigned char buf[16384];
    for (;;) {
        DWORD count = 0xFFFFFFFF, size = sizeof(buf);
        if (WNetEnumResourceW(h, &count, buf, &size) != NO_ERROR) break;
        const auto* r = reinterpret_cast<const NETRESOURCEW*>(buf);
        for (DWORD i = 0; i < count; ++i)
            if (r[i].lpRemoteName) NetRememberPath(r[i].lpRemoteName);
    }
    WNetCloseEnum(h);
}

// What Windows finds on the network, the same as its Network folder, leaving out whatever has no shares.
void NetLook() {
    {
        std::lock_guard lk(g_netM);
        if (g_netLooking || NowMs() - g_netLookedAt < 120000) return;
        g_netLooking = true;
    }
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ComPtr<IShellItem> net;
        ComPtr<IEnumShellItems> en;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_NetworkFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&net))) &&
            SUCCEEDED(net->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            for (;;) {
                ComPtr<IShellItem> item;
                if (en->Next(1, &item, nullptr) != S_OK) break;
                wchar_t* s = nullptr;
                if (FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &s))) continue;
                const std::wstring p = s;
                CoTaskMemFree(s);
                if (IsServerPath(p)) {
                    NetRememberPath(p);
                    Enqueue(kNetPath, JobKind::Revalidate);
                }
            }
        }
        en.Reset();
        net.Reset();
        CoUninitialize();
        {
            std::lock_guard lk(g_netM);
            g_netLooking = false;
            g_netLookedAt = NowMs();
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }).detach();
}

void NetEnumerateComputers(Listing& out) {
    out.path = kNetPath;
    wchar_t me[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(me, &n)) NetRemember(me);
    NetAddMapped();
    NetLook();
    std::lock_guard lk(g_netM);
    for (const auto& [k, name] : g_netNames) ListingAdd(out, name.c_str(), name.size(), FILE_ATTRIBUTE_DIRECTORY, 0, 0);
}

void NetEnumerateShares(const std::wstring& server, Listing& out) {
    out.path = server;
    DWORD resume = 0;
    NET_API_STATUS st;
    do {
        SHARE_INFO_1* buf = nullptr;
        DWORD read = 0, total = 0;
        st = NetShareEnum(const_cast<LPWSTR>(server.c_str()), 1, reinterpret_cast<LPBYTE*>(&buf), MAX_PREFERRED_LENGTH, &read, &total, &resume);
        if (st == NERR_Success || st == ERROR_MORE_DATA)
            for (DWORD i = 0; i < read; ++i) {
                if ((buf[i].shi1_type & STYPE_MASK) != STYPE_DISKTREE) continue;
                const std::wstring name = buf[i].shi1_netname;
                // C$ and the like: for administrators, so they go with the hidden items
                ListingAdd(out, name.c_str(), name.size(), FILE_ATTRIBUTE_DIRECTORY | (name.back() == L'$' ? FILE_ATTRIBUTE_HIDDEN : 0), 0, 0);
            }
        if (buf) NetApiBufferFree(buf);
    } while (st == ERROR_MORE_DATA);
    if (st != NERR_Success) out.error = st;
}

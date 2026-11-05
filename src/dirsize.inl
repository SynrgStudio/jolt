// Tamaño de carpetas: suma recursiva en un hilo aparte (con NtQueryDirectoryFile, como el listado).
// Guarda el total de cada subcarpeta que recorre: después de calcular una carpeta, todas sus
// subcarpetas ya tienen tamaño. Se incluye desde app.cpp antes del estado de las pestañas.

constexpr UINT WM_APP_DIRSIZE = WM_APP + 5;

struct DirSize {
    uint64_t bytes = 0, files = 0, dirs = 0;
    bool done = false;
};

std::mutex g_dsm;
std::condition_variable g_dscv;
std::unordered_map<std::wstring, DirSize> g_dirSizes;  // clave: Key(ruta)
std::deque<std::wstring> g_dsQueue;
std::atomic<bool> g_dsAny{false};       // hay algún tamaño (atajo para no bloquear al dibujar)
std::atomic<bool> g_dsBusy{false};      // calculando
std::atomic<bool> g_dsNotify{false};    // hay un aviso pendiente para la ventana
std::atomic<bool> g_dsFinished{false};  // terminó una raíz: reordenar si se ordena por tamaño

bool DirSizeOf(const std::wstring& key, DirSize& out) {
    if (!g_dsAny) return false;
    std::lock_guard lk(g_dsm);
    auto it = g_dirSizes.find(key);
    if (it == g_dirSizes.end()) return false;
    out = it->second;
    return true;
}

void DsPost() {
    if (!g_dsNotify.exchange(true)) PostMessageW(g_hwnd, WM_APP_DIRSIZE, 0, 0);
}

DirSize DsWalk(const std::wstring& path, const std::wstring& rootKey, DirSize& acc, double& lastPost) {
    Listing l;
    EnumerateDir(path, EnumMethod::Nt, l);
    DirSize me;
    for (const Entry& e : l.entries) {
        if (e.isDir()) {
            ++me.dirs;
            ++acc.dirs;
            if (e.attrs & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // enlaces y uniones: no se siguen
            DirSize sub = DsWalk(Join(path, l.name(e)), rootKey, acc, lastPost);
            me.bytes += sub.bytes;
            me.files += sub.files;
            me.dirs += sub.dirs;
        } else {
            ++me.files;
            me.bytes += e.size;
            ++acc.files;
            acc.bytes += e.size;
        }
    }
    me.done = true;
    std::lock_guard lk(g_dsm);
    g_dirSizes[Key(path)] = me;
    double now = NowMs();
    if (now - lastPost > 120) {
        lastPost = now;
        DirSize& r = g_dirSizes[rootKey];
        if (!r.done) r = acc;
        DsPost();
    }
    return me;
}

// ---- persistencia: los tamaños calculados se guardan en %LOCALAPPDATA%\Jolt\tamanos.bin
// (se escriben al terminar cada cálculo y se leen al arrancar, en segundo plano).
std::wstring LocalDataDir() {
    wchar_t* p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        dir = std::wstring(p) + L"\\Jolt";
        CoTaskMemFree(p);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

void SaveDirSizes() {
    std::vector<char> buf;
    auto put = [&](const void* d, size_t n) { buf.insert(buf.end(), static_cast<const char*>(d), static_cast<const char*>(d) + n); };
    {
        std::lock_guard lk(g_dsm);
        uint32_t magic = 0x5A535952, n = 0;  // "RYSZ"
        put(&magic, 4);
        put(&n, 4);
        for (auto& [k, v] : g_dirSizes) {
            if (!v.done) continue;
            uint16_t len = static_cast<uint16_t>(std::min<size_t>(k.size(), 65535));
            put(&len, 2);
            put(k.data(), len * sizeof(wchar_t));
            put(&v.bytes, 8);
            put(&v.files, 8);
            put(&v.dirs, 8);
            ++n;
        }
        memcpy(buf.data() + 4, &n, 4);
    }
    std::wstring f = LocalDataDir() + L"\\tamanos.bin", tmp = f + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    bool ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr) && w == buf.size();
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void LoadDirSizes() {
    std::wstring f = LocalDataDir() + L"\\tamanos.bin";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<char> buf(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got == buf.size();
    CloseHandle(h);
    if (!ok || buf.size() < 8) return;
    size_t pos = 0;
    auto get = [&](void* d, size_t n) {
        if (pos + n > buf.size()) return false;
        memcpy(d, buf.data() + pos, n);
        pos += n;
        return true;
    };
    uint32_t magic = 0, n = 0;
    get(&magic, 4);
    get(&n, 4);
    if (magic != 0x5A535952) return;
    std::unordered_map<std::wstring, DirSize> m;
    m.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t len = 0;
        if (!get(&len, 2) || pos + len * sizeof(wchar_t) > buf.size()) break;
        std::wstring k(reinterpret_cast<const wchar_t*>(buf.data() + pos), len);
        pos += len * sizeof(wchar_t);
        DirSize d;
        if (!get(&d.bytes, 8) || !get(&d.files, 8) || !get(&d.dirs, 8)) break;
        d.done = true;
        m.emplace(std::move(k), d);
    }
    {
        std::lock_guard lk(g_dsm);
        for (auto& [k, v] : m) g_dirSizes.emplace(k, v);  // lo calculado en esta sesión manda
        g_dsAny = !g_dirSizes.empty();
    }
    g_dsFinished = true;
    DsPost();
}

void DirSizeThread() {
    for (;;) {
        std::wstring p;
        {
            std::unique_lock lk(g_dsm);
            g_dscv.wait(lk, [] { return !g_dsQueue.empty(); });
            p = g_dsQueue.front();
            g_dsQueue.pop_front();
        }
        g_dsBusy = true;
        DirSize acc;
        double last = NowMs();
        std::wstring rk = Key(p);
        {
            std::lock_guard lk(g_dsm);
            g_dirSizes[rk] = DirSize{};
        }
        DsWalk(p, rk, acc, last);
        {
            std::lock_guard lk(g_dsm);
            if (g_dsQueue.empty()) g_dsBusy = false;
        }
        g_dsFinished = true;
        DsPost();
        if (!g_dsBusy && !g_test) SaveDirSizes();
    }
}

// Pide calcular el tamaño de estas carpetas (se muestran con "…" hasta que terminan).
void DirSizeRequest(const std::vector<std::wstring>& paths) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(DirSizeThread).detach(); });
    {
        std::lock_guard lk(g_dsm);
        for (const std::wstring& p : paths) {
            std::wstring k = Key(p);
            bool queued = false;
            for (const std::wstring& q : g_dsQueue) queued |= Key(q) == k;
            if (queued) continue;
            g_dsQueue.push_back(p);
            g_dirSizes[k] = DirSize{};
        }
        g_dsAny = true;
        g_dsBusy = true;
    }
    g_dscv.notify_one();
}

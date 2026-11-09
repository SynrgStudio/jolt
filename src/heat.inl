// Usage-based recents: each folder gains heat when used and cools over time, so the ones really in use stay
// on top rather than the last one opened. Files opened from Jolt gain it too: Ctrl+P ranks by it (palette.inl).
// Also holds the state of sidebar drags.

constexpr UINT_PTR kTimerHeat = 13;
constexpr UINT kHeatDwellMs = 1500;                       // passing through a folder doesn't count as using it
constexpr double kHeatHalfLife = 30.0 * 24 * 3600;        // heat halves every 30 days
constexpr size_t kHeatMax = 1000, kHotMax = kHotCountMax;  // folders and files

struct Heat {
    std::wstring path;
    double score = 0;
    long long last = 0;  // seconds since 1970
    bool file = false;   // a file opened from Jolt: it counts in Ctrl+P, not in the sidebar's most used
};
struct HotItem {
    std::wstring path, name;
    float heat = 0;  // 0..1 relative to the most used
};
std::vector<Heat> g_heat;
std::vector<HotItem> g_hot;  // what the sidebar shows, from most to least used
std::wstring g_heatPending;
bool g_heatReady = false;  // restoring the session at startup doesn't count as use

bool FolderExists(const std::wstring& p);
void TouchSession();
void Invalidate();
void DrawSideDrag();

double HeatAt(const Heat& h, long long now) { return h.score * std::exp2(-static_cast<double>(now - h.last) / kHeatHalfLife); }

std::wstring HeatFile() {
    const std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\') + 1) + L"usage.txt";
}

// Cooling is the same for all: the order only changes when a folder is used, not as time passes.
void HeatRefresh() {
    g_hot.clear();
    std::unordered_set<std::wstring> favs;
    for (const Favorite& v : g_favs) favs.insert(Key(v.path));
    const long long now = time(nullptr);
    std::vector<const Heat*> order;
    for (const Heat& h : g_heat)
        if (!h.file && !IsDriveRoot(h.path) && !favs.count(Key(h.path))) order.push_back(&h);
    std::sort(order.begin(), order.end(), [&](const Heat* a, const Heat* b) {
        const double x = HeatAt(*a, now), y = HeatAt(*b, now);
        return x != y ? x > y : a->last > b->last;
    });
    if (order.size() > kHotMax) order.resize(kHotMax);
    if (order.empty()) return;
    const double top = std::max(HeatAt(*order[0], now), 1e-9);
    std::unordered_map<std::wstring, int> leaves;
    for (const Heat* h : order) ++leaves[Key(LeafOf(h->path))];
    for (const Heat* h : order) {
        std::wstring name = LeafOf(h->path);
        if (leaves[Key(name)] > 1) {  // two "src": make clear which is which
            const size_t cut = h->path.find_last_of(L'\\');
            if (cut != std::wstring::npos) name += L" · " + LeafOf(h->path.substr(0, cut));
        }
        g_hot.push_back({h->path, name, static_cast<float>(HeatAt(*h, now) / top)});
    }
}

void HeatAdd(const std::wstring& path, double amount, long long when, bool file = false) {
    const std::wstring k = Key(path);
    auto it = std::find_if(g_heat.begin(), g_heat.end(), [&](const Heat& h) { return Key(h.path) == k; });
    if (it == g_heat.end()) {
        g_heat.push_back({path, amount, when, file});
    } else {
        it->score = HeatAt(*it, when) + amount;
        it->last = when;
        it->path = path;
        it->file = file;
    }
    if (g_heat.size() > kHeatMax) {
        const long long now = time(nullptr);
        g_heat.erase(std::min_element(g_heat.begin(), g_heat.end(), [&](const Heat& a, const Heat& b) { return HeatAt(a, now) < HeatAt(b, now); }));
    }
}

void HeatForget(const std::wstring& path) {
    const std::wstring k = Key(path);
    g_heat.erase(std::remove_if(g_heat.begin(), g_heat.end(), [&](const Heat& h) { return Key(h.path) == k; }), g_heat.end());
    HeatRefresh();
    TouchSession();
}

// Called on navigation. If the user is still there a moment later, it counts.
void HeatMark(const std::wstring& path) {
    if (!g_heatReady || IsVirtual(path)) return;
    g_heatPending = path;
    if (path.empty()) KillTimer(g_hwnd, kTimerHeat);
    else SetTimer(g_hwnd, kTimerHeat, kHeatDwellMs, nullptr);
}

void HeatCommit() {
    if (g_heatPending.empty()) return;
    HeatAdd(g_heatPending, 1.0, time(nullptr));
    g_heatPending.clear();
    HeatRefresh();
    TouchSession();
    Invalidate();
}

// A file opened from Jolt, or picked in Ctrl+P: it counts at once (nobody passes through a file on the way elsewhere).
void HeatUseFile(const std::wstring& path) {
    if (!g_heatReady || path.empty() || IsVirtual(path)) return;
    HeatAdd(path, 1.0, time(nullptr), true);
    TouchSession();
}

void HeatSave() {
    const std::wstring f = HeatFile();
    if (f.empty()) return;
    std::ofstream out(f, std::ios::binary);
    out << "# Jolt usage: heat, last use and path, and \"file\" for files. Rewritten automatically.\n";
    for (const Heat& h : g_heat) out << h.score << '\t' << h.last << '\t' << Utf8(h.path) << (h.file ? "\tfile" : "") << '\n';
}

void HeatLoad() {
    g_heat.clear();
    std::ifstream in(HeatFile(), std::ios::binary);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            const size_t c = line.find('\t', b + 1);  // a fourth column: "file"
            g_heat.push_back({Wide(line.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1)), atof(line.c_str()),
                              atoll(line.c_str() + a + 1), c != std::string::npos && line.compare(c + 1, std::string::npos, "file") == 0});
        }
    } else {
        // first run: the recents history gives a starting point, most recent first
        const long long now = time(nullptr);
        int n = 0;
        for (const std::wstring& r : g_recents) {
            if (n >= 30) break;
            if (FolderExists(r)) g_heat.push_back({r, 1.0, now - n++});
        }
    }
    HeatRefresh();
}

// ---- sidebar state

struct SideDrag {
    bool press = false, active = false, inFavs = false;
    int idx = -1, insert = 0;
    float x0 = 0, y0 = 0, x = 0, y = 0;
    std::wstring path, name;
} g_sideDrag;
float g_favTop = 0, g_favBottom = 0;  // favorites area, recomputed on every draw
int g_sideHotFirst = 0;               // index in g_sidePaths where the recents start
int g_favRename = -1;                 // favorite being renamed

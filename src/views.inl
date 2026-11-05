// Vistas: cómo se muestra una carpeta. Una lista de reglas (ordenar, agrupar, carpetas primero,
// mostrar solo, ocultar) que vale para todas las carpetas, y vistas propias para carpetas puntuales
// (opcionalmente también para sus subcarpetas). Se guardan en %APPDATA%\Jolt\vistas.txt, que se
// puede editar a mano. Se incluye desde app.cpp después de dirsize.inl.
//
// Cómo se combinan las reglas: los grupos son lo de más afuera (cada día, cada tipo...); dentro de
// cada grupo, "carpetas" separa carpetas y archivos; y dentro de eso mandan las reglas de orden, la
// primera de la lista antes que las demás (las siguientes desempatan). Los filtros se aplican antes.

enum class StepKind { Sort, Group, Folders, Show, Hide, Fold };
enum FoldMode { FdAll, FdButToday, FdButFirst, FdCount };
enum SortField { SfName, SfDate, SfSize, SfExt, SfCount };
enum GroupBy { GbDay, GbMonth, GbType, GbExt, GbCount };
enum FolderMode { FmMixed, FmFirst, FmLast, FmFirstByName, FmCount };
enum Cond { CName, CExt, CNewer, COlder, CBigger, CSmaller, CFolders, CFiles, CHidden, CCount };

struct Step {
    StepKind kind = StepKind::Sort;
    int a = 0;  // campo / agrupado / modo de carpetas / condición
    int b = 0;  // orden: 1 = ascendente
    std::wstring text;  // valor de la condición
};
struct View {
    std::vector<Step> steps;
    bool sub = false;   // vale también para las subcarpetas (solo vistas de una carpeta)
    std::wstring path;  // carpeta (vacío = todas)
};

View g_viewAll;
// Ver carpetas / archivos / extensiones (comandos de la paleta; valen para todas las carpetas)
bool g_hideDirs = false, g_hideFiles = false, g_hideExt = false;
std::unordered_map<std::wstring, View> g_views;  // por carpeta (clave: Key(ruta))
uint64_t g_viewEpoch = 1;                        // cambia con cualquier cosa que altere cómo se ve un listado

// La vista que manda en una carpeta: la propia, la de un ancestro con "subcarpetas", o la general.
View* ViewFor(const std::wstring& path) {
    if (!g_views.empty() && !path.empty()) {
        auto it = g_views.find(Key(path));
        if (it != g_views.end()) return &it->second;
        for (std::wstring p = ParentOf(path); !p.empty(); p = ParentOf(p)) {
            auto pit = g_views.find(Key(p));
            if (pit != g_views.end() && pit->second.sub) return &pit->second;
            if (IsDriveRoot(p)) break;
        }
    }
    return &g_viewAll;
}

const Step* FirstStep(const View& v, StepKind k) {
    for (const Step& s : v.steps)
        if (s.kind == k) return &s;
    return nullptr;
}

// Lo que se ve en el encabezado de columnas: el primer orden de la vista.
bool ViewPrimarySort(const View& v, SortBy& by, bool& asc) {
    const Step* s = FirstStep(v, StepKind::Sort);
    if (!s || s->a == SfExt) return false;
    by = s->a == SfDate ? SortBy::Date : s->a == SfSize ? SortBy::Size : SortBy::Name;
    asc = s->b != 0;
    return true;
}

// ---- texto (vistas.txt)
const wchar_t* kSortNames[] = {L"nombre", L"fecha", L"tamano", L"extension"};
const wchar_t* kGroupNames[] = {L"dia", L"mes", L"tipo", L"extension"};
const wchar_t* kFolderNames[] = {L"mezcladas", L"primero", L"ultimo", L"nombre"};
const wchar_t* kFoldNames[] = {L"todos", L"hoy", L"primero"};
const wchar_t* kCondNames[] = {L"nombre", L"extension", L"nuevos", L"viejos", L"grandes", L"chicos", L"carpetas", L"archivos", L"ocultos"};

int IndexOf(const wchar_t* const* names, int n, const std::wstring& s) {
    for (int i = 0; i < n; ++i)
        if (s == names[i]) return i;
    return -1;
}

bool ParseStep(const std::wstring& key, const std::wstring& val, Step& s) {
    std::wstring v = Trim(val), first = v, rest;
    size_t sp = v.find(L' ');
    if (sp != std::wstring::npos) {
        first = v.substr(0, sp);
        rest = Trim(v.substr(sp + 1));
    }
    std::wstring f = Lower(first);
    if (key == L"ordenar") {
        int a = IndexOf(kSortNames, SfCount, f);
        if (a < 0) return false;
        s = {StepKind::Sort, a, Lower(rest) != L"desc", L""};
        return true;
    }
    if (key == L"agrupar") {
        int a = IndexOf(kGroupNames, GbCount, f);
        if (a < 0) return false;
        s = {StepKind::Group, a, 0, L""};
        return true;
    }
    if (key == L"carpetas") {
        int a = IndexOf(kFolderNames, FmCount, f);
        if (a < 0) return false;
        s = {StepKind::Folders, a, 0, L""};
        return true;
    }
    if (key == L"plegar") {
        int a = IndexOf(kFoldNames, FdCount, f);
        if (a < 0) return false;
        s = {StepKind::Fold, a, 0, L""};
        return true;
    }
    if (key == L"mostrar" || key == L"ocultar") {
        int a = IndexOf(kCondNames, CCount, f);
        if (a < 0) return false;
        if (a == CName && Lower(rest).rfind(L"contiene", 0) == 0) rest = Trim(rest.substr(8));
        s = {key == L"mostrar" ? StepKind::Show : StepKind::Hide, a, 0, rest};
        return true;
    }
    return false;
}

std::wstring StepLine(const Step& s) {
    switch (s.kind) {
        case StepKind::Sort: return std::wstring(L"ordenar = ") + kSortNames[s.a] + (s.b ? L" asc" : L" desc");
        case StepKind::Group: return std::wstring(L"agrupar = ") + kGroupNames[s.a];
        case StepKind::Folders: return std::wstring(L"carpetas = ") + kFolderNames[s.a];
        case StepKind::Fold: return std::wstring(L"plegar = ") + kFoldNames[s.a];
        default: {
            std::wstring l = s.kind == StepKind::Show ? L"mostrar = " : L"ocultar = ";
            l += kCondNames[s.a];
            if (s.a == CName) l += L" contiene";
            if (s.a <= CSmaller && !s.text.empty()) l += L" " + s.text;
            return l;
        }
    }
}

std::wstring ViewsFile() {
    std::wstring d = ConfigDir();
    return d.empty() ? L"" : d + L"\\vistas.txt";
}

void SaveViews() {
    std::wstring f = ViewsFile();
    if (f.empty()) return;
    std::wstring s =
        L"# Vistas de Jolt: cómo se muestra cada carpeta. Se edita desde el botón de filtro del encabezado\n"
        L"# (o a mano: se recarga sola). Reglas, una por línea y en orden:\n"
        L"#   ordenar = nombre|fecha|tamano|extension asc|desc   (la primera manda, las demás desempatan)\n"
        L"#   agrupar = dia|mes|tipo|extension\n"
        L"#   carpetas = primero|ultimo|mezcladas|nombre          (nombre: primero solo al ordenar por nombre)\n"
        L"#   plegar = todos|hoy|primero   (cómo arrancan los grupos: todos plegados, todos menos hoy, todos menos el primero)\n"
        L"#   mostrar = ... / ocultar = ...  con: nombre contiene X, extension pdf docx, nuevos N (días),\n"
        L"#            viejos N, grandes N (MB), chicos N, carpetas, archivos, ocultos\n"
        L"# [todas] vale para todas las carpetas; [C:\\ruta] solo para esa (subcarpetas = si: también adentro).\n\n";
    s += L"[todas]\n";
    for (const Step& st : g_viewAll.steps) s += StepLine(st) + L"\n";
    for (auto& [k, v] : g_views) {
        s += L"\n[" + v.path + L"]\n";
        if (v.sub) s += L"subcarpetas = si\n";
        for (const Step& st : v.steps) s += StepLine(st) + L"\n";
    }
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF" << Utf8(s);
}

bool LoadViews() {
    std::ifstream in(ViewsFile(), std::ios::binary);
    if (!in) return false;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw);
    g_viewAll = View{};
    g_views.clear();
    View* cur = nullptr;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        if (!line.empty() && line[0] == L'#') continue;
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            std::wstring name = Trim(line.substr(1, line.size() - 2));
            if (Lower(name) == L"todas") {
                cur = &g_viewAll;
            } else {
                std::wstring p = Normalize(name);
                View& v = g_views[Key(p)];
                v.path = p;
                cur = &v;
            }
            continue;
        }
        size_t eq = line.find(L'=');
        if (!cur || eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        if (key == L"subcarpetas") {
            cur->sub = ParseBool(val);
            continue;
        }
        Step s;
        if (ParseStep(key, val, s)) cur->steps.push_back(s);
    }
    return true;
}

// ---- aplicar una vista a un listado
int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::vector<std::wstring> SplitTerms(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s + L",") {
        if (c == L',' || c == L' ' || c == L';') {
            if (!cur.empty()) out.push_back(Lower(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// 1 = cumple, 0 = no cumple, -1 = no aplica (p. ej. tamaño o extensión de una carpeta)
int CondMatch(const Listing& l, const Entry& e, const Step& s, int64_t now) {
    const int64_t day = 864000000000LL;
    switch (s.a) {
        case CName: {
            std::wstring n = Lower(std::wstring(l.name(e), e.nameLen));
            for (const std::wstring& t : SplitTerms(s.text))
                if (n.find(t) != std::wstring::npos) return 1;
            return s.text.empty() ? 1 : 0;
        }
        case CExt: {
            if (e.isDir()) return -1;
            std::wstring x = ExtOf(l.name(e), e.nameLen);
            for (std::wstring t : SplitTerms(s.text)) {
                if (!t.empty() && t[0] != L'.') t = L"." + t;
                if (x == t) return 1;
            }
            return 0;
        }
        case CNewer: return e.mtime >= now - static_cast<int64_t>(_wtof(s.text.c_str()) * day) ? 1 : 0;
        case COlder: return e.mtime < now - static_cast<int64_t>(_wtof(s.text.c_str()) * day) ? 1 : 0;
        case CBigger:
            if (e.isDir()) return -1;
            return e.size > static_cast<uint64_t>(_wtof(s.text.c_str()) * 1048576.0) ? 1 : 0;
        case CSmaller:
            if (e.isDir()) return -1;
            return e.size < static_cast<uint64_t>(_wtof(s.text.c_str()) * 1048576.0) ? 1 : 0;
        case CFolders: return e.isDir() ? 1 : 0;
        case CFiles: return e.isDir() ? 0 : 1;
        case CHidden: return (e.attrs & FILE_ATTRIBUTE_HIDDEN) ? 1 : 0;
    }
    return -1;
}

// Tipo de archivo por extensión, para "agrupar por tipo".
int TypeGroup(const Listing& l, const Entry& e) {
    if (e.isDir()) return 0;
    std::wstring x = ExtOf(l.name(e), e.nameLen);
    auto in = [&](std::initializer_list<const wchar_t*> k) {
        for (auto s : k)
            if (x == s) return true;
        return false;
    };
    if (in({L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".tif", L".tiff", L".svg", L".ico", L".psd", L".raw", L".avif"})) return 1;
    if (in({L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".m4v", L".flv"})) return 2;
    if (in({L".mp3", L".wav", L".flac", L".ogg", L".m4a", L".aac", L".wma", L".opus"})) return 3;
    if (in({L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".txt", L".md", L".rtf", L".csv", L".epub"})) return 4;
    if (in({L".zip", L".7z", L".rar", L".tar", L".gz", L".xz", L".bz2", L".zst", L".iso"})) return 5;
    if (in({L".exe", L".msi", L".bat", L".cmd", L".ps1", L".lnk", L".appx", L".msix"})) return 6;
    if (in({L".c", L".cpp", L".h", L".hpp", L".inl", L".cs", L".py", L".js", L".ts", L".tsx", L".jsx", L".rs", L".go", L".java",
            L".json", L".xml", L".yaml", L".yml", L".toml", L".html", L".css", L".sql", L".sh", L".lua"})) return 7;
    return 8;
}
const wchar_t* kTypeLabels[] = {L"Carpetas", L"Imágenes", L"Videos", L"Audio", L"Documentos", L"Comprimidos",
                                L"Programas", L"Código", L"Otros"};

std::wstring DayLabel(int y, int m, int d, int wday, const SYSTEMTIME& today, const SYSTEMTIME& yest) {
    static const wchar_t* days[] = {L"domingo", L"lunes", L"martes", L"miércoles", L"jueves", L"viernes", L"sábado"};
    static const wchar_t* months[] = {L"enero", L"febrero", L"marzo", L"abril", L"mayo", L"junio", L"julio",
                                      L"agosto", L"septiembre", L"octubre", L"noviembre", L"diciembre"};
    if (y == today.wYear && m == today.wMonth && d == today.wDay) return L"Hoy";
    if (y == yest.wYear && m == yest.wMonth && d == yest.wDay) return L"Ayer";
    std::wstring s = std::wstring(days[wday]) + L" " + std::to_wstring(d) + L" de " + months[m - 1];
    if (y != today.wYear) s += L" de " + std::to_wstring(y);
    return s;
}

std::shared_ptr<const Listing> ApplyView(const Listing& src, const View& v) {
    auto out = std::make_shared<Listing>();
    out->path = src.path;
    out->names = src.names;
    out->error = src.error;
    out->sortBy = src.sortBy;
    out->sortAsc = src.sortAsc;
    out->foldersFirst = src.foldersFirst;
    out->isView = true;
    // filtros
    const bool hideHidden = !g_cfg.showHidden && !src.path.empty();
    const int64_t now = NowFileTime();
    out->entries.reserve(src.entries.size());
    for (const Entry& e : src.entries) {
        if (hideHidden && (e.attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (!src.path.empty() && (g_hideDirs ? e.isDir() : g_hideFiles && !e.isDir())) continue;
        bool keep = true;
        for (const Step& s : v.steps) {
            if (s.kind != StepKind::Show && s.kind != StepKind::Hide) continue;
            int m = CondMatch(src, e, s, now);
            if ((s.kind == StepKind::Show && m == 0) || (s.kind == StepKind::Hide && m == 1)) {
                keep = false;
                break;
            }
        }
        if (keep) out->entries.push_back(e);
    }
    if (src.path.empty()) return out;  // unidades: en su orden

    std::vector<const Step*> sorts;
    for (const Step& s : v.steps)
        if (s.kind == StepKind::Sort) sorts.push_back(&s);
    const Step* grp = FirstStep(v, StepKind::Group);
    const Step* fold = FirstStep(v, StepKind::Folders);
    int fm = fold ? fold->a : FmMixed;
    if (fm == FmFirstByName) fm = (sorts.empty() || sorts[0]->a == SfName) ? FmFirst : FmMixed;

    struct It {
        Entry e;
        uint64_t size;
        int64_t g;
        uint32_t k = 0;  // su clave de orden por nombre
    };
    std::vector<It> items;
    items.reserve(out->entries.size());
    std::unique_lock<std::mutex> lk(g_dsm, std::defer_lock);
    if (g_dsAny) lk.lock();
    // fechas locales para agrupar por día o mes
    SYSTEMTIME today{}, yest{};
    GetLocalTime(&today);
    {
        FILETIME ft;
        SystemTimeToFileTime(&today, &ft);
        ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
        u.QuadPart -= 864000000000ULL;
        FILETIME fy{u.LowPart, u.HighPart};
        FileTimeToSystemTime(&fy, &yest);
    }
    std::vector<std::wstring> extKeys;
    for (const Entry& e : out->entries) {
        It it{e, e.size, 0};
        if (e.isDir() && lk.owns_lock()) {
            auto f = g_dirSizes.find(Key(Join(src.path, src.name(e))));
            it.size = f != g_dirSizes.end() ? f->second.bytes : 0;
        }
        if (grp) {
            if (grp->a == GbDay || grp->a == GbMonth) {
                FILETIME u{static_cast<DWORD>(e.mtime), static_cast<DWORD>(static_cast<uint64_t>(e.mtime) >> 32)}, loc;
                SYSTEMTIME st{};
                FileTimeToLocalFileTime(&u, &loc);
                FileTimeToSystemTime(&loc, &st);
                it.g = grp->a == GbDay ? st.wYear * 10000 + st.wMonth * 100 + st.wDay : st.wYear * 100 + st.wMonth;
            } else if (grp->a == GbType) {
                it.g = TypeGroup(src, e);
            } else {
                std::wstring x = e.isDir() ? L"/" : ExtOf(src.name(e), e.nameLen);  // "/": carpetas
                auto f = std::find(extKeys.begin(), extKeys.end(), x);
                if (f == extKeys.end()) {
                    extKeys.push_back(x);
                    f = extKeys.end() - 1;
                }
                it.g = f - extKeys.begin();
            }
        }
        it.k = static_cast<uint32_t>(items.size());
        items.push_back(it);
    }
    if (lk.owns_lock()) lk.unlock();
    NameKeys keys;
    keys.Build(*out);
    // orden de los grupos: fechas, lo más nuevo primero salvo que el orden por fecha sea ascendente
    bool dateAsc = false;
    for (const Step* s : sorts)
        if (s->a == SfDate) {
            dateAsc = s->b != 0;
            break;
        }
    std::vector<int> extRank(extKeys.size());
    if (grp && grp->a == GbExt) {
        std::vector<int> idx(extKeys.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) {
            // carpetas primero, sin extensión al final
            auto rank = [](const std::wstring& x) { return x == L"/" ? 0 : x.empty() ? 2 : 1; };
            if (rank(extKeys[a]) != rank(extKeys[b])) return rank(extKeys[a]) < rank(extKeys[b]);
            return extKeys[a] < extKeys[b];
        });
        for (size_t r = 0; r < idx.size(); ++r) extRank[idx[r]] = static_cast<int>(r);
    }
    auto groupOrder = [&](int64_t g) -> int64_t {
        if (!grp) return 0;
        if (grp->a == GbDay || grp->a == GbMonth) return dateAsc ? g : -g;
        if (grp->a == GbExt) return extRank[static_cast<size_t>(g)];
        return g;
    };
    const wchar_t* names = src.names.data();
    std::stable_sort(items.begin(), items.end(), [&](const It& a, const It& b) {
        if (grp && a.g != b.g) return groupOrder(a.g) < groupOrder(b.g);
        bool da = a.e.isDir(), db = b.e.isDir();
        if (da != db && (fm == FmFirst || fm == FmLast)) return fm == FmFirst ? da : db;
        for (const Step* s : sorts) {
            int c = 0;
            switch (s->a) {
                case SfName: c = keys.Compare(a.k, b.k); break;
                case SfDate: c = a.e.mtime < b.e.mtime ? -1 : a.e.mtime > b.e.mtime ? 1 : 0; break;
                case SfSize: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
                case SfExt:
                    c = _wcsicmp(ExtOf(names + a.e.nameOff, a.e.nameLen).c_str(), ExtOf(names + b.e.nameOff, b.e.nameLen).c_str());
                    break;
            }
            if (c) return s->b ? c < 0 : c > 0;
        }
        return keys.Compare(a.k, b.k) < 0;
    });
    for (size_t i = 0; i < items.size(); ++i) out->entries[i] = items[i].e;
    // grupos: dónde empieza cada uno y su título
    if (grp) {
        static const wchar_t* months[] = {L"enero", L"febrero", L"marzo", L"abril", L"mayo", L"junio", L"julio",
                                          L"agosto", L"septiembre", L"octubre", L"noviembre", L"diciembre"};
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0 && items[i].g == items[i - 1].g) continue;
            int64_t g = items[i].g;
            std::wstring label;
            if (grp->a == GbDay) {
                int y = static_cast<int>(g / 10000), m = static_cast<int>(g / 100 % 100), d = static_cast<int>(g % 100);
                SYSTEMTIME st{};
                st.wYear = static_cast<WORD>(y);
                st.wMonth = static_cast<WORD>(m);
                st.wDay = static_cast<WORD>(d);
                FILETIME ft;
                SystemTimeToFileTime(&st, &ft);
                FileTimeToSystemTime(&ft, &st);  // calcula el día de la semana
                label = DayLabel(y, m, d, st.wDayOfWeek, today, yest);
            } else if (grp->a == GbMonth) {
                int y = static_cast<int>(g / 100), m = static_cast<int>(g % 100);
                label = std::wstring(months[std::clamp(m, 1, 12) - 1]) + L" de " + std::to_wstring(y);
                label[0] = towupper(label[0]);
            } else if (grp->a == GbType) {
                label = kTypeLabels[std::clamp(static_cast<int>(g), 0, 8)];
            } else {
                const std::wstring& x = extKeys[static_cast<size_t>(g)];
                label = x == L"/" ? L"Carpetas" : x.empty() ? L"Sin extensión" : x;
            }
            out->groups.push_back({static_cast<uint32_t>(i), label});
        }
        // cómo arranca cada grupo
        if (const Step* fd = FirstStep(v, StepKind::Fold)) {
            out->groupFolded.resize(out->groups.size());
            for (size_t gi = 0; gi < out->groups.size(); ++gi)
                out->groupFolded[gi] = fd->a == FdAll ? 1 : fd->a == FdButFirst ? (gi != 0) : (out->groups[gi].second != L"Hoy");
        }
    }
    return out;
}

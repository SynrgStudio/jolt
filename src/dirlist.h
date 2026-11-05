#pragma once
// Enumeración de carpetas: el núcleo que tiene que ser rápido.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct Entry {
    uint32_t nameOff;  // offset dentro de Listing::names
    uint32_t nameLen;  // en wchar_t, sin el terminador
    uint32_t attrs;
    uint64_t size;
    int64_t mtime;  // FILETIME en UTC como entero
    bool isDir() const { return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};

enum class SortBy { Name, Date, Size };

// Una carpeta listada. Los nombres viven todos juntos en un solo bloque
// (terminados en 0) para no hacer una asignación de memoria por archivo.
struct Listing {
    std::wstring path;  // "" = lista de unidades
    std::vector<Entry> entries;
    std::vector<wchar_t> names;
    DWORD error = 0;  // código Win32 si no se pudo leer
    SortBy sortBy = SortBy::Name;
    bool sortAsc = true;
    bool foldersFirst = true;
    // Vista aplicada (app): grupos (dónde empieza cada uno y su título) y caché del resultado.
    bool isView = false;
    std::vector<std::pair<uint32_t, std::wstring>> groups;
    std::vector<uint8_t> groupFolded;  // cómo arranca cada grupo según la vista (1 = plegado)
    mutable std::shared_ptr<const Listing> view;
    mutable uint64_t viewSig = 0;
    const wchar_t* name(const Entry& e) const { return names.data() + e.nameOff; }
};

enum class EnumMethod { Nt, Win32 };

bool EnumerateDir(const std::wstring& path, EnumMethod method, Listing& out);
// Nombres en orden "natural" como el Explorador. foldersFirst: carpetas arriba; si no, mezcladas.
// Claves de orden por nombre: se calculan una vez por archivo y comparar dos es comparar bytes.
// Ordenan igual que StrCmpLogicalW, que recalcula todo en cada comparación.
struct NameKeys {
    std::vector<char> bytes;
    std::vector<uint32_t> at;
    void Build(const Listing& l);
    int Compare(size_t a, size_t b) const { return std::strcmp(bytes.data() + at[a], bytes.data() + at[b]); }
};

extern bool g_sortWithKeys;  // bench: compara contra el orden directo
void SortListing(Listing& l, SortBy by = SortBy::Name, bool asc = true, bool foldersFirst = true);
bool SameContent(const Listing& a, const Listing& b);
std::wstring ToExtendedPath(const std::wstring& path);  // agrega \\?\ para rutas largas

inline double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

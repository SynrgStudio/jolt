// bench: compara cuánto tarda listar una carpeta con distintas APIs de Windows.
// Uso: bench.exe <carpeta> [iteraciones]
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
// propkey.h necesita initguid para definir las PKEY en esta unidad.
#include <initguid.h>
#include <propkey.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "dirlist.h"

namespace {

int g_count = 0;

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool RunNt(const std::wstring& p) {
    Listing l;
    bool ok = EnumerateDir(p, EnumMethod::Nt, l);
    g_count = static_cast<int>(l.entries.size());
    return ok;
}

bool RunNtSorted(const std::wstring& p) {
    Listing l;
    bool ok = EnumerateDir(p, EnumMethod::Nt, l);
    SortListing(l);
    g_count = static_cast<int>(l.entries.size());
    return ok;
}

bool RunWin32(const std::wstring& p) {
    Listing l;
    bool ok = EnumerateDir(p, EnumMethod::Win32, l);
    g_count = static_cast<int>(l.entries.size());
    return ok;
}

// Lo que hace una app basada en el Shell: nombre visible, fecha y tamaño por cada elemento.
bool RunShellImpl(const std::wstring& p, bool icons) {
    IShellItem* folder = nullptr;
    if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&folder)))) return false;
    IEnumShellItems* en = nullptr;
    HRESULT hr = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en));
    folder->Release();
    if (FAILED(hr)) return false;
    int n = 0;
    IShellItem* it = nullptr;
    while (en->Next(1, &it, nullptr) == S_OK) {
        LPWSTR name = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) CoTaskMemFree(name);
        IShellItem2* it2 = nullptr;
        if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
            FILETIME ft;
            ULONGLONG sz;
            it2->GetFileTime(PKEY_DateModified, &ft);
            it2->GetUInt64(PKEY_Size, &sz);
            it2->Release();
        }
        if (icons) {
            LPWSTR fsPath = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &fsPath))) {
                SHFILEINFOW sfi{};
                SHGetFileInfoW(fsPath, 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
                CoTaskMemFree(fsPath);
            }
        }
        it->Release();
        ++n;
    }
    en->Release();
    g_count = n;
    return true;
}
bool RunShell(const std::wstring& p) { return RunShellImpl(p, false); }
bool RunShellIcons(const std::wstring& p) { return RunShellImpl(p, true); }

struct Method {
    const char* label;
    bool (*fn)(const std::wstring&);
};

}  // namespace

// bench.exe --orden <carpeta>: recorre el árbol y compara el orden con claves contra StrCmpLogicalW.
int CheckOrder(const std::wstring& root) {
    std::vector<std::wstring> stack{root};
    size_t folders = 0, names = 0, bad = 0;
    while (!stack.empty() && folders < 40000) {
        std::wstring p = std::move(stack.back());
        stack.pop_back();
        Listing a, b;
        if (!EnumerateDir(p, EnumMethod::Nt, a)) continue;
        EnumerateDir(p, EnumMethod::Nt, b);
        ++folders;
        names += a.entries.size();
        g_sortWithKeys = false;
        SortListing(a);
        g_sortWithKeys = true;
        SortListing(b);
        for (size_t i = 0; i < a.entries.size() && i < b.entries.size(); ++i) {
            if (std::wcscmp(a.name(a.entries[i]), b.name(b.entries[i])) != 0) {
                if (++bad <= 12) std::printf("distinto en %s: %s / %s\n", Utf8(p).c_str(), Utf8(a.name(a.entries[i])).c_str(), Utf8(b.name(b.entries[i])).c_str());
                break;
            }
        }
        for (const Entry& e : a.entries)
            if (e.isDir() && !(e.attrs & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(p + L"\\" + a.name(e));
    }
    std::printf("%zu carpetas, %zu nombres, %zu carpetas con otro orden\n", folders, names, bad);
    return bad != 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 3 && std::wcscmp(argv[1], L"--orden") == 0) return CheckOrder(argv[2]);
    if (argc < 2) {
        std::printf("Uso: bench.exe <carpeta> [iteraciones]\n");
        return 1;
    }
    wchar_t full[4096];
    GetFullPathNameW(argv[1], 4096, full, nullptr);
    std::wstring path = full;
    if (path.size() > 3 && path.back() == L'\\') path.pop_back();
    int iters = argc > 2 ? std::max(1, _wtoi(argv[2])) : 15;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Una pasada previa sin medir, para que todos los métodos arranquen con el disco en el mismo estado.
    if (!RunNt(path)) {
        std::printf("No se pudo leer la carpeta.\n");
        return 1;
    }
    std::printf("Carpeta:     %s\nElementos:   %d\nIteraciones: %d\n\n", Utf8(path).c_str(), g_count, iters);
    std::printf("   primera     minimo    mediana   metodo\n");

    const Method methods[] = {
        {"NtQueryDirectoryFile, lotes de 64 KiB (Jolt)", RunNt},
        {"  + orden natural, carpetas primero", RunNtSorted},
        {"FindFirstFileEx (Win32 clasico)", RunWin32},
        {"Shell / IShellItem: nombre, fecha, tamano", RunShell},
        {"Shell + indice de icono por archivo", RunShellIcons},
    };
    for (const Method& m : methods) {
        std::vector<double> t;
        bool ok = true;
        for (int i = 0; i < iters && ok; ++i) {
            double t0 = NowMs();
            ok = m.fn(path);
            t.push_back(NowMs() - t0);
        }
        if (!ok) {
            std::printf("   (fallo)                         %s\n", m.label);
            continue;
        }
        double first = t[0];
        std::sort(t.begin(), t.end());
        std::printf("%7.3f ms %7.3f ms %7.3f ms   %s\n", first, t[0], t[t.size() / 2], m.label);
    }
    CoUninitialize();
    return 0;
}

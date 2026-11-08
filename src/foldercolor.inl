// Folder colors: a color for a folder's icon, only in Jolt (Windows and the folder's desktop.ini aren't touched; an
// icon set there already shows, since folder icons come from the Shell). The plain folder icon is recolored keeping
// its shading, in the list, the grid and the sidebar. They live in tags.txt, [folder_colors]. Included from app.cpp
// after the system icons.

struct FolderColorDef {
    const wchar_t* name;
    UINT32 color;
};
const FolderColorDef kFolderColors[] = {{L"Red", 0xE5534B},  {L"Orange", 0xF0883E}, {L"Green", 0x57AB5A}, {L"Teal", 0x2FB5AA},
                                        {L"Blue", 0x4F8EF7}, {L"Purple", 0x9D7CD8}, {L"Pink", 0xE86BA7},  {L"Gray", 0x9198A1}};

bool FolderColor(const std::wstring& full, UINT32& out) {
    if (g_folderColors.empty() || full.empty()) return false;
    std::wstring k = Key(full);
    if (k.size() > 3 && k.back() == L'\\') k.pop_back();
    auto it = g_folderColors.find(k);
    if (it == g_folderColors.end()) return false;
    out = it->second;
    return true;
}

std::wstring FolderColorName(UINT32 c) {
    for (const FolderColorDef& d : kFolderColors)
        if (d.color == c) return d.name;
    return L"#" + Hex(c);
}

// Gives every folder that color (has = false: back to the usual one).
void FolderColorSet(const std::vector<std::wstring>& dirs, bool has, UINT32 color) {
    if (dirs.empty()) return;
    for (const std::wstring& d : dirs) {
        const std::wstring k = Key(Normalize(d));
        if (has) g_folderColors[k] = color;
        else g_folderColors.erase(k);
    }
    const std::wstring what = dirs.size() == 1 ? L"" : L" (" + Thousands(static_cast<int>(dirs.size())) + L" folders)";
    S.message = has ? L"Folder color: " + FolderColorName(color) + what : L"Folder color removed" + what;
    SaveTags();
    Invalidate();
}

// --- the icon, recolored: the folder's average brightness becomes the color itself, and its lighter and darker
// parts stay lighter and darker.
std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap>> g_tintBmp;  // (size kind, color) -> bitmap of the current target
void FolderTintRelease() { g_tintBmp.clear(); }

ComPtr<ID2D1Bitmap> MakeTinted(IImageList* list, int idx, UINT32 color) {
    ComPtr<ID2D1Bitmap> out;
    HICON hi = nullptr;
    if (!list || idx < 0 || !g_rt || FAILED(list->GetIcon(idx, ILD_TRANSPARENT, &hi)) || !hi) return out;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    if (SUCCEEDED(g_wic->CreateBitmapFromHICON(hi, &wb)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
        px.resize(static_cast<size_t>(w) * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) px.clear();
    }
    DestroyIcon(hi);
    if (px.empty()) return out;
    auto luma = [&](size_t i) { return 0.299 * px[i + 2] + 0.587 * px[i + 1] + 0.114 * px[i]; };
    double sum = 0, weight = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        sum += px[i + 3] * luma(i);
        weight += px[i + 3];
    }
    const double ref = weight > 0 && sum > 0 ? sum / weight : 200;
    const double target[3] = {static_cast<double>(color & 0xFF), static_cast<double>((color >> 8) & 0xFF), static_cast<double>((color >> 16) & 0xFF)};
    for (size_t i = 0; i < px.size(); i += 4) {
        const double a = px[i + 3] / 255.0, k = luma(i) / ref;
        for (int c = 0; c < 3; ++c) {
            const double t = target[c];
            const double v = k <= 1 ? t * k : t + (255 - t) * std::min(1.0, (k - 1) * 2.5);
            px[i + c] = static_cast<BYTE>(std::clamp(v, 0.0, 255.0) * a + 0.5);  // premultiplied
        }
    }
    g_rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4,
                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &out);
    return out;
}

// kind 0: the size the list uses; 1: extra large (48 px); 2: jumbo (256 px), for the grid.
ID2D1Bitmap* FolderTinted(int kind, UINT32 color) {
    const uint64_t key = (static_cast<uint64_t>(kind) << 32) | color;
    auto it = g_tintBmp.find(key);
    if (it != g_tintBmp.end()) return it->second.Get();
    static ComPtr<IImageList> big[2];
    IImageList* list = g_imgList.Get();
    if (kind > 0) {
        if (!big[kind - 1]) SHGetImageList(kind == 1 ? SHIL_EXTRALARGE : SHIL_JUMBO, IID_PPV_ARGS(&big[kind - 1]));
        list = big[kind - 1].Get();
    }
    return (g_tintBmp[key] = MakeTinted(list, g_iconFolder, color)).Get();
}

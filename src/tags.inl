// Tags and colors. Tags are labels you put on files and folders, each with a color, shown as little tag-shaped
// labels after the name (dots where there's no room) and usable in the filter ("tag:urgent"); they live in
// %APPDATA%\Jolt\tags.txt, and so do the folder colors (foldercolor.inl).
// Name colors by rule come from [file_colors] in config.txt ("patterns = RRGGBB").
// Included from app.cpp after the filter code, before BuildRows.

struct TagDef {
    std::wstring name;
    UINT32 color;
};
std::vector<TagDef> g_tagDefs;                                             // known tags, in file order
std::unordered_map<std::wstring, std::vector<std::wstring>> g_tagsByPath;  // Key(path) -> tag names
std::vector<std::wstring> g_tagTargets;                                    // what the tag palette acts on
std::unordered_map<std::wstring, UINT32> g_folderColors;                   // Key(folder) -> RRGGBB of its icon
std::wstring Thousands(int n);

std::wstring TagsFile() {
    const std::wstring d = ConfigDir();
    return d.empty() ? L"" : d + L"\\tags.txt";
}

// A color for a tag that has none set: steady, from its name.
UINT32 TagAutoColor(const std::wstring& name) {
    static const UINT32 palette[] = {0xE06C75, 0xE5A84B, 0xC8D15E, 0x6BCB77, 0x4FC1E9, 0x5B8DEF, 0xAC8CF3, 0xF07CBF};
    uint32_t h = 2166136261u;
    for (wchar_t c : Lower(name)) h = (h ^ static_cast<uint32_t>(c)) * 16777619u;
    return palette[h % std::size(palette)];
}

const TagDef* TagFind(const std::wstring& name) {
    for (const TagDef& d : g_tagDefs)
        if (_wcsicmp(d.name.c_str(), name.c_str()) == 0) return &d;
    return nullptr;
}
UINT32 TagColor(const std::wstring& name) {
    const TagDef* d = TagFind(name);
    return d ? d->color : TagAutoColor(name);
}

const std::vector<std::wstring>* TagsOf(const std::wstring& full) {
    auto it = g_tagsByPath.find(Key(full));
    return it == g_tagsByPath.end() || it->second.empty() ? nullptr : &it->second;
}
bool HasTag(const std::wstring& full, const std::wstring& tag) {
    const auto* t = TagsOf(full);
    if (!t) return false;
    for (const std::wstring& x : *t)
        if (_wcsicmp(x.c_str(), tag.c_str()) == 0) return true;
    return false;
}

std::vector<std::wstring> TagSplit(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t p = 0;
    while (p < s.size()) {
        size_t sp = s.find_first_of(L" ,", p);
        std::wstring w = s.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p);
        p = sp == std::wstring::npos ? s.size() : sp + 1;
        if (!w.empty()) out.push_back(w);
    }
    return out;
}

void SaveTags() {
    const std::wstring f = TagsFile();
    if (f.empty()) return;
    std::wstring s =
        L"# Jolt tags. [tags]: name = RRGGBB (a tag without a color gets one on its own).\n"
        L"# [items]: path = tags, separated by spaces. [folder_colors]: folder = RRGGBB, the color of its icon in Jolt.\n"
        L"# Edited by hand it reloads on save.\n\n[tags]\n";
    for (const TagDef& d : g_tagDefs) s += d.name + L" = " + Hex(d.color) + L"\n";
    s += L"\n[items]\n";
    std::vector<std::pair<std::wstring, const std::vector<std::wstring>*>> rows;
    for (const auto& [k, v] : g_tagsByPath)
        if (!v.empty()) rows.push_back({k, &v});
    std::sort(rows.begin(), rows.end());
    for (const auto& [k, v] : rows) {
        std::wstring line = k + L" = ";
        for (size_t i = 0; i < v->size(); ++i) line += (i ? L" " : L"") + (*v)[i];
        s += line + L"\n";
    }
    s += L"\n[folder_colors]\n";
    std::vector<std::pair<std::wstring, UINT32>> colors(g_folderColors.begin(), g_folderColors.end());
    std::sort(colors.begin(), colors.end());
    for (const auto& [k, c] : colors) s += k + L" = " + Hex(c) + L"\n";
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF" << Utf8(s);
    out.close();
    RefreshConfigSig();  // we wrote it ourselves: no need to reload
}

void LoadTags() {
    g_tagDefs.clear();
    g_tagsByPath.clear();
    g_folderColors.clear();
    std::ifstream in(TagsFile(), std::ios::binary);
    if (!in) return;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    const std::wstring text = Wide(raw);
    std::wstring section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = Trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        if (line.empty() || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Lower(line.substr(1, line.size() - 2));
            continue;
        }
        // the path may hold '=': the last one separates the tags (or the color)
        size_t eq = section == L"items" || section == L"folder_colors" ? line.rfind(L'=') : line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        if (section == L"tags") {
            std::wstring h = val;
            if (!h.empty() && h[0] == L'#') h.erase(0, 1);
            g_tagDefs.push_back({key, h.size() == 6 ? static_cast<UINT32>(wcstoul(h.c_str(), nullptr, 16)) : TagAutoColor(key)});
        } else if (section == L"items") {
            std::vector<std::wstring> tags = TagSplit(val);
            if (!tags.empty()) g_tagsByPath[Key(Normalize(key))] = std::move(tags);
        } else if (section == L"folder_colors") {
            std::wstring h = val;
            if (!h.empty() && h[0] == L'#') h.erase(0, 1);
            if (h.size() == 6) g_folderColors[Key(Normalize(key))] = static_cast<UINT32>(wcstoul(h.c_str(), nullptr, 16));
        }
    }
}

// Every tag there is: the defined ones, then any other in use.
std::vector<std::wstring> AllTags() {
    std::vector<std::wstring> out;
    for (const TagDef& d : g_tagDefs) out.push_back(d.name);
    for (const auto& [k, v] : g_tagsByPath)
        for (const std::wstring& t : v) {
            bool have = false;
            for (const std::wstring& o : out) have |= _wcsicmp(o.c_str(), t.c_str()) == 0;
            if (!have) out.push_back(t);
        }
    return out;
}

// Puts the tag on every path, or takes it off all of them when they all have it already.
void TagToggle(const std::vector<std::wstring>& paths, std::wstring tag) {
    tag = Trim(tag);
    std::erase_if(tag, [](wchar_t c) { return c == L' ' || c == L',' || c == L'=' || c == L'#'; });
    if (tag.empty() || paths.empty()) return;
    if (const TagDef* d = TagFind(tag)) tag = d->name;  // the spelling on file
    else g_tagDefs.push_back({tag, TagAutoColor(tag)});
    bool all = true;
    for (const std::wstring& p : paths) all &= HasTag(p, tag);
    for (const std::wstring& p : paths) {
        auto& v = g_tagsByPath[Key(Normalize(p))];
        std::erase_if(v, [&](const std::wstring& x) { return _wcsicmp(x.c_str(), tag.c_str()) == 0; });
        if (!all) v.push_back(tag);
    }
    S.message = all ? L"Tag removed: " + tag : L"Tagged " + Thousands(static_cast<int>(paths.size())) + (paths.size() == 1 ? L" item: " : L" items: ") + tag;
    SaveTags();
    Invalidate();
}

// A renamed or moved item keeps its tags, and a folder its color.
void TagsFollow(const std::wstring& from, const std::wstring& to) {
    bool moved = false;
    if (auto it = g_tagsByPath.find(Key(from)); it != g_tagsByPath.end()) {
        std::vector<std::wstring> v = std::move(it->second);
        g_tagsByPath.erase(it);
        g_tagsByPath[Key(to)] = std::move(v);
        moved = true;
    }
    if (auto it = g_folderColors.find(Key(from)); it != g_folderColors.end()) {
        const UINT32 c = it->second;
        g_folderColors.erase(it);
        g_folderColors[Key(to)] = c;
        moved = true;
    }
    if (moved) SaveTags();
}

// ---- name colors by rule ([file_colors] in config.txt)
UINT32 NameColorFor(const wchar_t* name, size_t len, bool isDir, UINT32 def) {
    if (g_cfg.fileColors.empty()) return def;
    const std::wstring ext = ExtOf(name, len);
    for (const FileColorRule& r : g_cfg.fileColors)
        for (const std::wstring& p : r.patterns) {
            if (p == L"folders" && isDir) return r.color;
            if (p.size() > 1 && p[0] == L'.' && p.find_first_of(L"*?") == std::wstring::npos) {
                if (!isDir && ext == p) return r.color;
            } else if (GlobMatch(name, len, p)) {
                return r.color;
            }
        }
    return def;
}


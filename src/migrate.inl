// One-time upgrade of data written by the Spanish versions of Jolt: renames the files and converts
// their keys and values. This is the only place that still knows the old names.

struct NamePair {
    const wchar_t *from, *to;
};

const NamePair kOldGeneral[] = {
    {L"segundo_plano", L"run_in_background"}, {L"atajo_global", L"global_hotkey"}, {L"icono_bandeja", L"tray_icon"},
    {L"abrir_discos", L"open_drives"},        {L"fuente", L"font"},                {L"tamano", L"font_size"},
    {L"tamaño", L"font_size"},                {L"fila", L"row_height"},            {L"escala", L"scale"},
    {L"barra_lateral", L"sidebar_width"},     {L"vista_previa", L"preview_width"}, {L"ocultos", L"show_hidden"},
    {L"recientes", L"recents"},               {L"carpetas_primero", L"folders_first"},
};
const NamePair kOldColumns[] = {
    {L"mostrar", L"show"},
    {L"ancho_modificado", L"modified_width"},
    {L"ancho_tamano", L"size_width"},
    {L"ancho_tamaño", L"size_width"},
};
const NamePair kOldColors[] = {
    {L"fondo", L"background"}, {L"barra", L"toolbar"},       {L"pestanas", L"tabs"},
    {L"lateral", L"sidebar"},  {L"borde", L"border"},        {L"texto", L"text"},
    {L"texto_secundario", L"text_secondary"},                {L"texto_tenue", L"text_faint"},
    {L"acento", L"accent"},    {L"seleccion", L"selection"}, {L"uso_disco", L"disk_usage"},
    {L"uso_disco_lleno", L"disk_usage_full"},
};
const NamePair kOldCommands[] = {
    {L"subir", L"up"}, {L"entrar", L"enter"}, {L"abrir", L"open"}, {L"abrir_pestana", L"open_tab"},
    {L"atras", L"back"}, {L"adelante", L"forward"}, {L"renombrar", L"rename"}, {L"eliminar", L"delete"},
    {L"carpeta_nueva", L"new_folder"}, {L"copiar", L"copy"}, {L"cortar", L"cut"}, {L"pegar", L"paste"},
    {L"seleccionar_todo", L"select_all"}, {L"favorito", L"favorite"}, {L"pestana_nueva", L"new_tab"},
    {L"cerrar_pestana", L"close_tab"}, {L"pestana_siguiente", L"next_tab"}, {L"pestana_anterior", L"previous_tab"},
    {L"ruta", L"path"}, {L"barra_lateral", L"sidebar"}, {L"recargar", L"reload"}, {L"tiempos", L"timings"},
    {L"menu_contextual", L"context_menu"}, {L"menu_app", L"app_menu"}, {L"paleta", L"palette"}, {L"ir_a", L"go_to"},
    {L"buscar_subcarpetas", L"search_subfolders"}, {L"desplegar", L"expand"}, {L"plegar", L"collapse"},
    {L"ir_a_ubicacion", L"go_to_location"}, {L"vista_previa", L"preview"}, {L"dividir", L"split"},
    {L"otro_panel", L"other_pane"}, {L"copiar_al_otro", L"copy_to_other"}, {L"mover_al_otro", L"move_to_other"},
    {L"copiar_ruta", L"copy_path"}, {L"archivo_nuevo", L"new_file"}, {L"duplicar", L"duplicate"},
    {L"propiedades", L"properties"}, {L"tamano_carpetas", L"folder_sizes"}, {L"ver_carpetas", L"show_folders"},
    {L"ver_archivos", L"show_files"}, {L"ver_ocultos", L"show_hidden"}, {L"ver_extensiones", L"show_extensions"},
    {L"reabrir_pestana", L"reopen_tab"}, {L"cerrar_otras", L"close_others"}, {L"abrir_explorador", L"open_explorer"},
    {L"ubicacion_real", L"real_location"}, {L"plegar_grupos", L"collapse_groups"},
    {L"desplegar_grupos", L"expand_groups"}, {L"agrupar_dia", L"group_by_day"}, {L"agrupar_tipo", L"group_by_type"},
    {L"sin_grupos", L"no_groups"}, {L"ordenar_tipo", L"sort_by_type"}, {L"aumentar_interfaz", L"zoom_in"},
    {L"reducir_interfaz", L"zoom_out"}, {L"restablecer_interfaz", L"zoom_reset"}, {L"fila_mas", L"rows_taller"},
    {L"fila_menos", L"rows_shorter"}, {L"vaciar_papelera", L"empty_recycle_bin"}, {L"deshacer", L"undo"},
    {L"rehacer", L"redo"}, {L"instalacion", L"install"}, {L"salir", L"quit"},
};
const NamePair kOldKeys[] = {
    {L"supr", L"Del"},      {L"retroceso", L"Backspace"}, {L"espacio", L"Space"},    {L"izquierda", L"Left"},
    {L"derecha", L"Right"}, {L"arriba", L"Up"},           {L"abajo", L"Down"},       {L"inicio", L"Home"},
    {L"fin", L"End"},       {L"repag", L"PageUp"},        {L"avpag", L"PageDown"},   {L"intro", L"Enter"},
    {L"mayus", L"Shift"},   {L"mayús", L"Shift"},
};
const NamePair kOldBools[] = {{L"si", L"yes"}, {L"sí", L"yes"}};
const NamePair kOldColumnValues[] = {
    {L"modificado", L"modified"}, {L"fecha", L"modified"}, {L"tamano", L"size"}, {L"tamaño", L"size"},
};
const NamePair kOldFoldersFirst[] = {{L"si", L"yes"}, {L"sí", L"yes"}, {L"nombre", L"name"}};
const NamePair kOldViewKeys[] = {
    {L"ordenar", L"sort"}, {L"agrupar", L"group"}, {L"carpetas", L"folders"},     {L"plegar", L"collapse"},
    {L"mostrar", L"show"}, {L"ocultar", L"hide"},  {L"subcarpetas", L"subfolders"},
};
const NamePair kOldViewValues[] = {
    {L"nombre", L"name"},    {L"fecha", L"date"},      {L"tamano", L"size"},      {L"tamaño", L"size"},
    {L"dia", L"day"},        {L"día", L"day"},         {L"mes", L"month"},        {L"tipo", L"type"},
    {L"primero", L"first"},  {L"ultimo", L"last"},     {L"último", L"last"},      {L"mezcladas", L"mixed"},
    {L"todos", L"all"},      {L"hoy", L"today"},       {L"nuevos", L"newer"},     {L"viejos", L"older"},
    {L"grandes", L"larger"}, {L"chicos", L"smaller"},  {L"carpetas", L"folders"}, {L"archivos", L"files"},
    {L"ocultos", L"hidden"}, {L"si", L"yes"},          {L"sí", L"yes"},
};
const NamePair kOldFavNames[] = {
    {L"Escritorio", L"Desktop"}, {L"Descargas", L"Downloads"}, {L"Documentos", L"Documents"},
    {L"Imágenes", L"Pictures"},  {L"Usuario", L"User"},
};
const NamePair kOldSessionKeys[] = {
    {L"ventana", L"window"}, {L"previa", L"preview"}, {L"ver", L"show"},
    {L"orden", L"sort"},     {L"dividida", L"split"}, {L"activa", L"active"},
};

template <size_t N>
std::wstring MigName(const NamePair (&table)[N], const std::wstring& s) {
    const std::wstring low = Lower(s);
    for (const NamePair& p : table)
        if (low == p.from) return p.to;
    return s;
}

// Replaces whole words only; separators such as + , and spaces stay as they are.
template <size_t N>
std::wstring MigWords(const NamePair (&table)[N], const std::wstring& s) {
    std::wstring out, word;
    for (wchar_t c : s + L'\n') {
        if (iswalnum(c)) {
            word += c;
            continue;
        }
        out += MigName(table, word);
        word.clear();
        if (c != L'\n') out += c;
    }
    return out;
}

std::wstring MigRead(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    return Wide(raw);
}

void MigWrite(const std::wstring& path, const std::wstring& text) {
    std::ofstream out(path, std::ios::binary);
    out << Utf8(text);
}

std::vector<std::wstring> MigLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
    }
    return lines;
}

bool MigIsOldConfig(const std::wstring& text) {
    for (const std::wstring& raw : MigLines(text)) {
        const std::wstring l = Lower(Trim(raw));
        if (l == L"[columnas]" || l == L"[colores]" || l == L"[atajos]") return true;
    }
    return false;
}

// Keeps the user's values and takes the comments and layout from the current default file.
void MigConfig() {
    const std::wstring file = ConfigFile();
    const std::wstring text = MigRead(file);
    if (!MigIsOldConfig(text)) return;
    CopyFileW(file.c_str(), (file + L".old").c_str(), FALSE);

    using Entries = std::vector<std::pair<std::wstring, std::wstring>>;
    std::unordered_map<std::wstring, Entries> user;
    std::wstring section;
    for (std::wstring line : MigLines(text)) {
        size_t hash = line.find(L'#');
        if (hash != std::wstring::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            const std::wstring s = Lower(line.substr(1, line.size() - 2));
            section = s == L"columnas" ? L"columns" : s == L"colores" ? L"colors" : s == L"atajos" ? L"shortcuts" : s;
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        if (section == L"general") {
            key = MigName(kOldGeneral, key);
            val = key == L"folders_first" ? MigName(kOldFoldersFirst, val) : MigName(kOldBools, val);
        } else if (section == L"columns") {
            key = MigName(kOldColumns, key);
            if (key == L"show") val = MigWords(kOldColumnValues, val);
        } else if (section == L"colors") {
            key = MigName(kOldColors, key);
        } else if (section == L"shortcuts") {
            key = MigName(kOldCommands, key);
            val = MigWords(kOldKeys, val);
        }
        user[section].push_back({key, val});
    }

    WriteDefaultConfig();
    std::wstring out;
    auto flush = [&](const std::wstring& s) {  // options the default file doesn't list
        for (const auto& [key, val] : user[s]) out += key + L" = " + val + L"\n";
        user[s].clear();
    };
    section.clear();
    for (const std::wstring& raw : MigLines(MigRead(file))) {
        const std::wstring line = Trim(raw);
        if (line.size() > 1 && line.front() == L'[' && line.back() == L']') {
            flush(section);
            section = Lower(line.substr(1, line.size() - 2));
        } else if (size_t eq = line.find(L'='); eq != std::wstring::npos && line[0] != L'#') {
            const std::wstring key = Lower(Trim(line.substr(0, eq)));
            Entries& entries = user[section];
            auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.first == key; });
            if (it != entries.end()) {
                const size_t hash = line.find(L'#');
                out += key + L" = " + it->second + (hash == std::wstring::npos ? L"" : L"    " + line.substr(hash)) + L"\n";
                entries.erase(it);
                continue;
            }
        }
        out += raw + L"\n";
    }
    flush(section);
    MigWrite(file, std::wstring(1, wchar_t(0xFEFF)) + out);  // BOM: so Notepad opens it as UTF-8
}

void MigViews() {
    const std::wstring file = ViewsFile();
    std::wstring out;
    for (const std::wstring& raw : MigLines(MigRead(file))) {
        const std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            out += (Lower(line) == L"[todas]" ? L"[all]" : line) + L"\n";
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = MigName(kOldViewKeys, Trim(line.substr(0, eq)));
        std::wstring val = Trim(line.substr(eq + 1)), first = val, rest;
        if (size_t sp = val.find(L' '); sp != std::wstring::npos) {
            first = val.substr(0, sp);
            rest = Trim(val.substr(sp + 1));
        }
        first = MigName(kOldViewValues, first);
        if (first == L"name" && Lower(rest).rfind(L"contiene", 0) == 0) rest = L"contains" + rest.substr(8);
        out += key + L" = " + first + (rest.empty() ? L"" : L" " + rest) + L"\n";
    }
    MigWrite(file, out);
    if (LoadViews()) SaveViews();  // puts back the explanatory header
}

void MigMenu() {
    const std::wstring file = MenuCfgFile();
    bool old = false;
    std::wstring out =
        L"# Jolt context menu. Sections: [background] [file] [folder].\n"
        L"# One option per line, in the order they appear. \"-\" in front = hidden. \"---\" = separator.\n"
        L"# Anything not listed here is added at the end, in the order Windows gives it.\n";
    for (std::wstring line : MigLines(MigRead(file))) {
        if (!line.empty() && line[0] == L'#') continue;
        const std::wstring l = Lower(Trim(line));
        if (l == L"[fondo]") line = L"[background]", old = true;
        else if (l == L"[archivo]") line = L"[file]", old = true;
        else if (l == L"[carpeta]") line = L"[folder]", old = true;
        else if (l == L"(elemento sin nombre)") line = L"(unnamed item)";
        else if (l == L"-(elemento sin nombre)") line = L"-(unnamed item)";
        out += line + L"\n";
    }
    if (old) MigWrite(file, out);
}

void MigFavorites(const std::wstring& file) {
    std::wstring out = L"# Jolt favorites: one line per folder, \"Name|Path\" or just the path.\n";
    for (std::wstring line : MigLines(MigRead(file))) {
        if (!line.empty() && line[0] == L'#') continue;
        if (size_t bar = line.find(L'|'); bar != std::wstring::npos) {
            const std::wstring name = line.substr(0, bar);
            for (const NamePair& p : kOldFavNames)
                if (name == p.from) line = p.to + line.substr(bar);
        }
        out += line + L"\n";
    }
    MigWrite(file, out);
}

void MigSession(const std::wstring& file) {
    std::wstring out;
    for (std::wstring line : MigLines(MigRead(file))) {
        if (!line.empty() && line[0] == L'#') continue;
        if (size_t eq = line.find(L'='); eq != std::wstring::npos)
            line = MigName(kOldSessionKeys, line.substr(0, eq)) + line.substr(eq);
        out += line + L"\n";
    }
    MigWrite(file, out);
}

// True when the old file existed and now has the new name.
bool MigRename(const std::wstring& dir, const wchar_t* from, const wchar_t* to) {
    const std::wstring a = dir + L"\\" + from, b = dir + L"\\" + to;
    if (GetFileAttributesW(a.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(b.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    return MoveFileW(a.c_str(), b.c_str()) != 0;
}

void MigrateOldData() {
    const std::wstring dir = ConfigDir();
    if (dir.empty()) return;
    if (MigRename(dir, L"favoritos.txt", L"favorites.txt")) MigFavorites(dir + L"\\favorites.txt");
    if (MigRename(dir, L"sesion.txt", L"session.txt")) MigSession(dir + L"\\session.txt");
    MigRename(dir, L"recientes.txt", L"recents.txt");
    MigRename(dir, L"usos.txt", L"usage.txt");
    MigConfig();
    if (MigRename(dir, L"vistas.txt", L"views.txt")) MigViews();
    MigMenu();
    if (g_test) return;  // the caches below belong to the real installation
    const std::wstring local = LocalDataDir();
    MigRename(local, L"tamanos.bin", L"sizes.bin");
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        const std::wstring letter(1, c), old = L"indice-" + letter;
        MigRename(local, (old + L".bin").c_str(), (L"index-" + letter + L".bin").c_str());
        DeleteFileW((local + L"\\" + old + L".bin.nuevo").c_str());
    }
}

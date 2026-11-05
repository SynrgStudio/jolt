// Configuración: %APPDATA%\Jolt\config.txt. Se genera con los valores por defecto la primera vez
// y se recarga sola cuando se guarda. Se incluye desde app.cpp después de la sección de favoritos.

enum class Cmd {
    None, Subir, Entrar, Abrir, AbrirPestana, Atras, Adelante, Renombrar, Eliminar, CarpetaNueva, Copiar, Cortar,
    Pegar, SeleccionarTodo, Favorito, PestanaNueva, CerrarPestana, PestanaSiguiente, PestanaAnterior, Ruta,
    BarraLateral, Recargar, Tiempos, MenuContextual, MenuApp, Paleta, IrA, BuscarSubcarpetas, Desplegar, Plegar,
    IrAUbicacion, VistaPrevia, Dividir, OtroPanel, CopiarAlOtro, MoverAlOtro, CopiarRuta, ArchivoNuevo, Duplicar,
    Propiedades, TamanoCarpetas, VerCarpetas, VerArchivos, VerOcultos, VerExtensiones, ReabrirPestana, CerrarOtras,
    Terminal, AbrirExplorador, UbicacionReal, PlegarGrupos, DesplegarGrupos, AgruparDia, AgruparTipo, SinGrupos,
    OrdenarTipo, AumentarInterfaz, ReducirInterfaz, RestablecerInterfaz, FilaMas, FilaMenos, VaciarPapelera,
    Deshacer, Rehacer,
    Instalacion, Salir
};
enum : UINT { ModCtrl = 1, ModShift = 2, ModAlt = 4 };

struct KeyBind {
    UINT mods, vk;
    Cmd cmd;
};

struct Config {
    std::wstring font = L"Segoe UI";
    float fontSize = 12;
    bool showHidden = true;
    bool background = true, tray = true, openDrives = false;
    std::wstring globalKey = L"Win+J";
    bool colDate = true, colSize = true;
    std::vector<KeyBind> keys;
} g_cfg;

struct CmdDef {
    const wchar_t* name;
    Cmd cmd;
    const wchar_t* defaults;
    const wchar_t* help;
};
const CmdDef kCommands[] = {
    {L"subir", Cmd::Subir, L"Retroceso, Izquierda, Alt+Arriba", L"ir a la carpeta superior"},
    {L"entrar", Cmd::Entrar, L"Derecha", L"entrar en la carpeta resaltada (no abre archivos)"},
    {L"abrir", Cmd::Abrir, L"Enter", L"abrir lo resaltado (carpeta o archivo)"},
    {L"abrir_pestana", Cmd::AbrirPestana, L"Ctrl+Enter", L"abrir la carpeta resaltada en una pestaña nueva"},
    {L"atras", Cmd::Atras, L"Alt+Izquierda", L"volver"},
    {L"adelante", Cmd::Adelante, L"Alt+Derecha", L"adelante"},
    {L"renombrar", Cmd::Renombrar, L"F2", L"renombrar"},
    {L"eliminar", Cmd::Eliminar, L"Supr, Shift+Supr", L"a la papelera (con Shift, definitivo)"},
    {L"carpeta_nueva", Cmd::CarpetaNueva, L"Ctrl+Shift+N", L"carpeta nueva"},
    {L"copiar", Cmd::Copiar, L"Ctrl+C", L"copiar"},
    {L"cortar", Cmd::Cortar, L"Ctrl+X", L"cortar"},
    {L"pegar", Cmd::Pegar, L"Ctrl+V", L"pegar"},
    {L"seleccionar_todo", Cmd::SeleccionarTodo, L"Ctrl+A", L"seleccionar todo"},
    {L"favorito", Cmd::Favorito, L"Ctrl+D", L"agregar o quitar la carpeta actual de favoritos"},
    {L"pestana_nueva", Cmd::PestanaNueva, L"Ctrl+T", L"pestaña nueva"},
    {L"cerrar_pestana", Cmd::CerrarPestana, L"Ctrl+W", L"cerrar pestaña"},
    {L"pestana_siguiente", Cmd::PestanaSiguiente, L"Ctrl+Tab", L"pestaña siguiente"},
    {L"pestana_anterior", Cmd::PestanaAnterior, L"Ctrl+Shift+Tab", L"pestaña anterior"},
    {L"ruta", Cmd::Ruta, L"Ctrl+L", L"escribir una ruta"},
    {L"barra_lateral", Cmd::BarraLateral, L"Ctrl+B", L"mostrar u ocultar la barra lateral"},
    {L"recargar", Cmd::Recargar, L"Ctrl+R", L"recargar la carpeta"},
    {L"tiempos", Cmd::Tiempos, L"F12", L"mostrar u ocultar los tiempos"},
    {L"menu_contextual", Cmd::MenuContextual, L"Shift+F10, Menu", L"menú contextual"},
    {L"menu_app", Cmd::MenuApp, L"Alt+M", L"menú de Jolt"},
    {L"paleta", Cmd::Paleta, L"Ctrl+K", L"paleta de comandos"},
    {L"ir_a", Cmd::IrA, L"Ctrl+P", L"ir a una carpeta reciente, favorita, del sistema o a una ruta"},
    {L"buscar_subcarpetas", Cmd::BuscarSubcarpetas, L"Ctrl+Shift+F", L"buscar en esta carpeta y todas sus subcarpetas"},
    {L"desplegar", Cmd::Desplegar, L"Shift+Derecha", L"desplegar o plegar la carpeta resaltada (árbol)"},
    {L"plegar", Cmd::Plegar, L"Shift+Izquierda", L"plegar la carpeta (o volver a la de arriba en el árbol)"},
    {L"ir_a_ubicacion", Cmd::IrAUbicacion, L"Ctrl+Shift+E", L"abrir la carpeta que contiene lo resaltado"},
    {L"vista_previa", Cmd::VistaPrevia, L"Espacio", L"mostrar u ocultar la vista previa"},
    {L"dividir", Cmd::Dividir, L"Ctrl+S", L"vista dividida en dos paneles"},
    {L"otro_panel", Cmd::OtroPanel, L"Tab", L"pasar al otro panel"},
    {L"copiar_al_otro", Cmd::CopiarAlOtro, L"F5", L"copiar la selección al otro panel"},
    {L"mover_al_otro", Cmd::MoverAlOtro, L"F6", L"mover la selección al otro panel"},
    {L"copiar_ruta", Cmd::CopiarRuta, L"Ctrl+Shift+C", L"copiar la ruta de lo seleccionado (o de la carpeta)"},
    {L"archivo_nuevo", Cmd::ArchivoNuevo, L"Ctrl+N", L"archivo nuevo"},
    {L"duplicar", Cmd::Duplicar, L"Ctrl+Shift+D", L"duplicar lo seleccionado"},
    {L"propiedades", Cmd::Propiedades, L"Alt+Enter", L"propiedades de Windows"},
    {L"tamano_carpetas", Cmd::TamanoCarpetas, L"Alt+R", L"calcular el tamaño de las carpetas"},
    {L"ver_carpetas", Cmd::VerCarpetas, L"", L"mostrar u ocultar las carpetas (toggle folders)"},
    {L"ver_archivos", Cmd::VerArchivos, L"", L"mostrar u ocultar los archivos (toggle files)"},
    {L"ver_ocultos", Cmd::VerOcultos, L"Ctrl+H", L"mostrar u ocultar los archivos ocultos (toggle hidden files)"},
    {L"ver_extensiones", Cmd::VerExtensiones, L"", L"mostrar u ocultar las extensiones (toggle file extensions)"},
    {L"reabrir_pestana", Cmd::ReabrirPestana, L"Ctrl+Shift+T", L"reabrir la última pestaña cerrada"},
    {L"cerrar_otras", Cmd::CerrarOtras, L"Ctrl+Alt+W", L"cerrar las demás pestañas"},
    {L"terminal", Cmd::Terminal, L"Ctrl+Alt+T", L"abrir la terminal en esta carpeta (consola, cmd, powershell)"},
    {L"abrir_explorador", Cmd::AbrirExplorador, L"Ctrl+Alt+E", L"abrir en el Explorador de Windows"},
    {L"ubicacion_real", Cmd::UbicacionReal, L"Ctrl+Alt+L", L"ir a la ubicación real de un acceso directo"},
    {L"plegar_grupos", Cmd::PlegarGrupos, L"", L"plegar todos los grupos"},
    {L"desplegar_grupos", Cmd::DesplegarGrupos, L"", L"desplegar todos los grupos"},
    {L"agrupar_dia", Cmd::AgruparDia, L"", L"agrupar esta carpeta por día"},
    {L"agrupar_tipo", Cmd::AgruparTipo, L"", L"agrupar esta carpeta por tipo"},
    {L"sin_grupos", Cmd::SinGrupos, L"", L"sacar los grupos de esta carpeta"},
    {L"ordenar_tipo", Cmd::OrdenarTipo, L"", L"ordenar por tipo (extensión)"},
    {L"aumentar_interfaz", Cmd::AumentarInterfaz, L"Ctrl++, Ctrl+Shift++, Ctrl+Num+", L"agrandar la interfaz (zoom)"},
    {L"reducir_interfaz", Cmd::ReducirInterfaz, L"Ctrl+-, Ctrl+Num-", L"achicar la interfaz (zoom)"},
    {L"restablecer_interfaz", Cmd::RestablecerInterfaz, L"Ctrl+0", L"volver al tamaño normal de la interfaz (zoom)"},
    {L"fila_mas", Cmd::FilaMas, L"", L"más espacio entre filas"},
    {L"fila_menos", Cmd::FilaMenos, L"", L"menos espacio entre filas"},
    {L"vaciar_papelera", Cmd::VaciarPapelera, L"", L"vaciar la papelera de reciclaje"},
    {L"deshacer", Cmd::Deshacer, L"Ctrl+Z", L"deshacer lo último (renombrar, mover, copiar, crear, papelera)"},
    {L"rehacer", Cmd::Rehacer, L"Ctrl+Y, Ctrl+Shift+Z", L"rehacer lo que se deshizo"},
    {L"instalacion", Cmd::Instalacion, L"", L"instalar Jolt, agregarlo al PATH, hacerlo predeterminado"},
    {L"salir", Cmd::Salir, L"", L"cerrar Jolt de verdad"},
};

struct KeyName {
    const wchar_t* name;
    UINT vk;
};
const KeyName kKeyNames[] = {
    {L"enter", VK_RETURN},    {L"intro", VK_RETURN},   {L"esc", VK_ESCAPE},      {L"escape", VK_ESCAPE},
    {L"supr", VK_DELETE},     {L"delete", VK_DELETE},  {L"del", VK_DELETE},      {L"retroceso", VK_BACK},
    {L"backspace", VK_BACK},  {L"tab", VK_TAB},        {L"espacio", VK_SPACE},   {L"space", VK_SPACE},
    {L"izquierda", VK_LEFT},  {L"left", VK_LEFT},      {L"derecha", VK_RIGHT},   {L"right", VK_RIGHT},
    {L"arriba", VK_UP},       {L"up", VK_UP},          {L"abajo", VK_DOWN},      {L"down", VK_DOWN},
    {L"inicio", VK_HOME},     {L"home", VK_HOME},      {L"fin", VK_END},         {L"end", VK_END},
    {L"repag", VK_PRIOR},     {L"pageup", VK_PRIOR},   {L"avpag", VK_NEXT},      {L"pagedown", VK_NEXT},
    {L"menu", VK_APPS},       {L"apps", VK_APPS},      {L"insert", VK_INSERT},   {L"ins", VK_INSERT},
    {L"+", VK_OEM_PLUS},      {L"-", VK_OEM_MINUS},    {L",", VK_OEM_COMMA},     {L".", VK_OEM_PERIOD},
    {L"num+", VK_ADD},        {L"num-", VK_SUBTRACT},
};

struct ColorDef {
    const wchar_t* name;
    UINT32* var;
    UINT32 def;
    const wchar_t* help;
};
const ColorDef kColors[] = {
    {L"fondo", &cBg, 0x1A1A1E, L"fondo de la lista"},
    {L"barra", &cTop, 0x202024, L"barra de navegación, pestaña activa y menús"},
    {L"pestanas", &cTabBar, 0x0F0F12, L"barra de pestañas (título)"},
    {L"lateral", &cSide, 0x151518, L"barra lateral y barra de estado"},
    {L"borde", &cBorder, 0x2B2B31, L"líneas y bordes"},
    {L"texto", &cText, 0xE9E9EE, L"texto principal"},
    {L"texto_secundario", &cDim, 0x8C8C97, L"fechas, tamaños, pestañas inactivas"},
    {L"texto_tenue", &cFaint, 0x4C4C55, L"títulos de sección, elementos deshabilitados"},
    {L"acento", &cAccent, 0x4F8CF7, L"resaltados y bordes de destino"},
    {L"seleccion", &cSel, 0x2C4C86, L"fila seleccionada"},
    {L"error", &cError, 0xF07178, L"mensajes de error"},
    {L"uso_disco", &cDisk, 0x3B8EEA, L"barra de uso de disco"},
    {L"uso_disco_lleno", &cDiskFull, 0xE5484D, L"barra de uso de disco al 90 % o más"},
};

std::wstring ConfigDir() {
    std::wstring f = FavFile();
    return f.empty() ? L"" : f.substr(0, f.find_last_of(L'\\'));
}
std::wstring ConfigFile() {
    std::wstring d = ConfigDir();
    return d.empty() ? L"" : d + L"\\config.txt";
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t c = s.find(L',', start);
        std::wstring part = Trim(s.substr(start, c == std::wstring::npos ? std::wstring::npos : c - start));
        if (!part.empty()) out.push_back(part);
        if (c == std::wstring::npos) break;
        start = c + 1;
    }
    return out;
}

// "Ctrl+Shift+N", "F2", "Alt+Izquierda", "Supr"...
bool ParseKey(const std::wstring& spec, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    std::wstring s = Lower(Trim(spec));
    size_t start = 0;
    for (;;) {
        size_t plus = s.find(L'+', start);
        bool last = plus == std::wstring::npos || plus + 1 >= s.size();  // "+" solo al final es la tecla "+"
        std::wstring part = Trim(last ? s.substr(start) : s.substr(start, plus - start));
        if (!last) {
            if (part == L"ctrl" || part == L"control") mods |= ModCtrl;
            else if (part == L"shift" || part == L"mayus" || part == L"mayús") mods |= ModShift;
            else if (part == L"alt") mods |= ModAlt;
            else return false;
            start = plus + 1;
            continue;
        }
        if (part.size() == 1) {
            wchar_t c = part[0];
            if (c >= L'a' && c <= L'z') vk = static_cast<UINT>(c - L'a' + 'A');
            else if (c >= L'0' && c <= L'9') vk = static_cast<UINT>(c);
        }
        if (!vk && part.size() >= 2 && part[0] == L'f') {
            int n = _wtoi(part.c_str() + 1);
            if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
        }
        if (!vk)
            for (const KeyName& k : kKeyNames)
                if (part == k.name) vk = k.vk;
        return vk != 0;
    }
}

void AddBindings(Cmd cmd, const std::wstring& list) {
    for (const std::wstring& spec : SplitList(list)) {
        UINT mods, vk;
        if (ParseKey(spec, mods, vk)) g_cfg.keys.push_back({mods, vk, cmd});
    }
}

std::wstring Hex(UINT32 c) {
    wchar_t b[8];
    swprintf(b, 8, L"%06X", c & 0xFFFFFF);
    return b;
}

void WriteDefaultConfig() {
    std::wstring f = ConfigFile();
    if (f.empty()) return;
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF";  // BOM: para que el Bloc de notas lo abra como UTF-8
    std::string s =
        "# Configuración de Jolt. Se recarga sola al guardar este archivo.\n"
        "# Las líneas que empiezan con # son comentarios. Si borrás una opción, vuelve a su valor por defecto.\n"
        "\n[general]\n"
        "# Fuente y tamaño (en píxeles a 100 %) de toda la interfaz.\n"
        "fuente = Segoe UI\n"
        "tamano = 12\n"
        "# Alto de cada fila de la lista.\n"
        "fila = 25\n"
        "# Tamaño de toda la interfaz, en porcentaje (Ctrl + y Ctrl - lo cambian).\n"
        "escala = 100\n"
        "# Ancho de la barra lateral.\n"
        "barra_lateral = 208\n"
        "# Ancho de la vista previa (Espacio).\n"
        "vista_previa = 360\n"
        "# Mostrar archivos ocultos y de sistema (atenuados): si / no\n"
        "ocultos = si\n"
        "# Carpetas arriba de los archivos: si / no / nombre (solo al ordenar por nombre; por fecha y tamaño van mezcladas)\n"
        "carpetas_primero = nombre\n"
        "# Cerrar la ventana la esconde y Jolt queda listo para volver al instante: si / no\n"
        "segundo_plano = si\n"
        "# Atajo desde cualquier app: abre Jolt, y si ya está al frente lo esconde. Vacío: sin atajo.\n"
        "# Con el Explorador al frente, abre su carpeta. Windows no cede Win+E.\n"
        "atajo_global = Win+J\n"
        "# Icono en la bandeja del sistema: si / no\n"
        "icono_bandeja = si\n"
        "# Abrir los discos y pendrives apenas se conectan: si / no\n"
        "abrir_discos = no\n"
        "\n[columnas]\n"
        "# Columnas visibles además del nombre: modificado, tamano\n"
        "mostrar = modificado, tamano\n"
        "ancho_modificado = 118\n"
        "ancho_tamano = 72\n"
        "\n[colores]\n"
        "# RRGGBB en hexadecimal.\n";
    for (const ColorDef& c : kColors) s += Utf8(std::wstring(c.name) + L" = " + Hex(c.def) + L"    # " + c.help + L"\n");
    s +=
        "\n[atajos]\n"
        "# Varias teclas por comando, separadas por coma. Modificadores: Ctrl, Shift, Alt.\n"
        "# Teclas: letras, números, F1..F24, Enter, Esc, Supr, Retroceso, Tab, Espacio, Izquierda, Derecha,\n"
        "# Arriba, Abajo, Inicio, Fin, RePag, AvPag, Menu, Insert, + - , .\n"
        "# Las flechas Arriba/Abajo, RePag/AvPag, Inicio/Fin (con Shift extienden la selección), Esc,\n"
        "# Ctrl+Espacio, Ctrl+1..9 (favoritos) y Alt+1..9 (pestañas) son fijos.\n";
    for (const CmdDef& c : kCommands) s += Utf8(std::wstring(c.name) + L" = " + c.defaults + L"    # " + c.help + L"\n");
    out << s;
}

bool ParseBool(const std::wstring& v) {
    std::wstring s = Lower(Trim(v));
    return s == L"si" || s == L"sí" || s == L"yes" || s == L"true" || s == L"1" || s == L"on";
}

void LoadConfig() {
    // valores por defecto
    g_cfg = Config{};
    for (const ColorDef& c : kColors) *c.var = c.def;
    kRow = 25;
    g_uiPx = 12;
    kSideW = 208;
    g_previewW = 360;
    g_foldersFirst = 2;
    kDateW = 118;
    kSizeW = 72;
    for (const CmdDef& c : kCommands) AddBindings(c.cmd, c.defaults);

    std::wstring f = ConfigFile();
    if (f.empty()) return;
    std::ifstream in(f, std::ios::binary);
    if (!in) {
        WriteDefaultConfig();
        return;
    }
    std::vector<KeyBind> userKeys;
    bool anyKeys = false;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw), section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        size_t hash = line.find(L'#');
        if (hash != std::wstring::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == L'[' && line.back() == L']') {
            section = Lower(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        if (section == L"general") {
            if (key == L"fuente" && !val.empty()) g_cfg.font = val;
            else if (key == L"tamano" || key == L"tamaño") g_cfg.fontSize = std::clamp(static_cast<float>(_wtof(val.c_str())), 8.f, 32.f);
            else if (key == L"fila") kRow = std::clamp(static_cast<float>(_wtof(val.c_str())), 16.f, 60.f);
            else if (key == L"escala") g_uiPx = std::clamp(static_cast<int>(std::lround(_wtof(val.c_str()) * 12 / 100)), kUiPxMin, kUiPxMax);
            else if (key == L"barra_lateral") kSideW = std::clamp(static_cast<float>(_wtof(val.c_str())), 120.f, 500.f);
            else if (key == L"ocultos") g_cfg.showHidden = ParseBool(val);
            else if (key == L"segundo_plano") g_cfg.background = ParseBool(val);
            else if (key == L"atajo_global") g_cfg.globalKey = val;
            else if (key == L"icono_bandeja") g_cfg.tray = ParseBool(val);
            else if (key == L"abrir_discos") g_cfg.openDrives = ParseBool(val);
            else if (key == L"vista_previa") g_previewW = std::clamp(static_cast<float>(_wtof(val.c_str())), 200.f, 1200.f);
            else if (key == L"carpetas_primero") {
                std::wstring v = Lower(Trim(val));
                g_foldersFirst = v == L"nombre" ? 2 : ParseBool(v) ? 1 : 0;
            }
        } else if (section == L"columnas") {
            if (key == L"mostrar") {
                g_cfg.colDate = g_cfg.colSize = false;
                for (const std::wstring& c : SplitList(Lower(val))) {
                    if (c == L"modificado" || c == L"fecha") g_cfg.colDate = true;
                    if (c == L"tamano" || c == L"tamaño") g_cfg.colSize = true;
                }
            } else if (key == L"ancho_modificado") kDateW = std::clamp(static_cast<float>(_wtof(val.c_str())), kDateMin, kDateMax);
            else if (key == L"ancho_tamano" || key == L"ancho_tamaño") kSizeW = std::clamp(static_cast<float>(_wtof(val.c_str())), kSizeMin, kSizeMax);
        } else if (section == L"colores") {
            for (const ColorDef& c : kColors)
                if (key == c.name) {
                    std::wstring h = val;
                    if (!h.empty() && h[0] == L'#') h.erase(0, 1);
                    if (h.size() == 6) *c.var = static_cast<UINT32>(wcstoul(h.c_str(), nullptr, 16));
                }
        } else if (section == L"atajos") {
            for (const CmdDef& c : kCommands)
                if (key == c.name) {
                    anyKeys = true;
                    std::vector<KeyBind> saved = std::move(g_cfg.keys);
                    g_cfg.keys.clear();
                    AddBindings(c.cmd, val);
                    userKeys.insert(userKeys.end(), g_cfg.keys.begin(), g_cfg.keys.end());
                    g_cfg.keys = std::move(saved);
                    // el comando configurado reemplaza sus teclas por defecto
                    g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(),
                                                    [&](const KeyBind& k) { return k.cmd == c.cmd; }),
                                     g_cfg.keys.end());
                }
        }
    }
    if (anyKeys) {
        // las teclas del usuario tienen prioridad sobre cualquier otra asignación por defecto
        for (const KeyBind& u : userKeys)
            g_cfg.keys.erase(std::remove_if(g_cfg.keys.begin(), g_cfg.keys.end(),
                                            [&](const KeyBind& k) { return k.mods == u.mods && k.vk == u.vk; }),
                             g_cfg.keys.end());
        g_cfg.keys.insert(g_cfg.keys.begin(), userKeys.begin(), userKeys.end());
    }
}

// Escribe en config.txt las teclas de un comando (y le quita esa tecla a cualquier otro comando).
// Conserva comentarios y el resto del archivo. Si falta la sección, la agrega.
void ConfigWriteShortcut(const std::wstring& cmdName, const std::wstring& specs, UINT mods, UINT vk) {
    std::wstring f = ConfigFile();
    if (f.empty()) return;
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaultConfig();
    std::ifstream in(f, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw);
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::wstring section;
    int sectionEnd = -1;  // última línea de [atajos]
    bool written = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring body = lines[i], comment;
        size_t hash = body.find(L'#');
        if (hash != std::wstring::npos) {
            comment = body.substr(hash);
            body = body.substr(0, hash);
        }
        std::wstring tb = Trim(body);
        if (tb.empty()) continue;
        if (tb.front() == L'[' && tb.back() == L']') {
            section = Lower(tb.substr(1, tb.size() - 2));
            continue;
        }
        if (section != L"atajos") continue;
        sectionEnd = static_cast<int>(i);
        size_t eq = tb.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(tb.substr(0, eq))), val = Trim(tb.substr(eq + 1));
        std::wstring pad = comment.empty() ? L"" : L"    " + comment;
        if (key == cmdName) {
            lines[i] = cmdName + L" = " + specs + pad;
            written = true;
            continue;
        }
        // otro comando con la misma tecla: se la quitamos
        std::vector<std::wstring> keep;
        bool changed = false;
        for (const std::wstring& spec : SplitList(val)) {
            UINT m, k;
            if (ParseKey(spec, m, k) && m == mods && k == vk) changed = true;
            else keep.push_back(spec);
        }
        if (changed) {
            std::wstring joined;
            for (size_t j = 0; j < keep.size(); ++j) joined += (j ? L", " : L"") + keep[j];
            lines[i] = key + L" = " + joined + pad;
        }
    }
    // comandos que usan la tecla por defecto y no figuran en el archivo: hay que fijarlos sin ella
    for (const CmdDef& c : kCommands) {
        if (c.name == cmdName) continue;
        bool inFile = false;
        std::wstring sec;
        for (const std::wstring& l : lines) {
            std::wstring tb = Trim(l.substr(0, l.find(L'#')));
            if (!tb.empty() && tb.front() == L'[' && tb.back() == L']') sec = Lower(tb.substr(1, tb.size() - 2));
            else if (sec == L"atajos" && tb.find(L'=') != std::wstring::npos && Lower(Trim(tb.substr(0, tb.find(L'=')))) == c.name) inFile = true;
        }
        if (inFile) continue;
        std::vector<std::wstring> keep;
        bool changed = false;
        for (const std::wstring& spec : SplitList(c.defaults)) {
            UINT m, k;
            if (ParseKey(spec, m, k) && m == mods && k == vk) changed = true;
            else keep.push_back(spec);
        }
        if (!changed) continue;
        std::wstring joined;
        for (size_t j = 0; j < keep.size(); ++j) joined += (j ? L", " : L"") + keep[j];
        std::wstring nl = std::wstring(c.name) + L" = " + joined + L"    # " + c.help;
        if (sectionEnd >= 0) lines.insert(lines.begin() + (++sectionEnd), nl);
        else {
            lines.push_back(L"");
            lines.push_back(L"[atajos]");
            lines.push_back(nl);
            sectionEnd = static_cast<int>(lines.size()) - 1;
        }
    }
    if (!written) {
        std::wstring nl = cmdName + L" = " + specs;
        for (const CmdDef& c : kCommands)
            if (c.name == cmdName) nl += std::wstring(L"    # ") + c.help;
        if (sectionEnd >= 0) lines.insert(lines.begin() + sectionEnd + 1, nl);
        else {
            lines.push_back(L"");
            lines.push_back(L"[atajos]");
            lines.push_back(nl);
        }
    }
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF";
    for (const std::wstring& l : lines) out << Utf8(l) << "\r\n";
}

// Cambia una opción en config.txt (conserva comentarios); si no está, la agrega al principio de la sección.
void ConfigSetValue(const std::wstring& sectionName, const std::wstring& key, const std::wstring& value) {
    std::wstring f = ConfigFile();
    if (f.empty()) return;
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaultConfig();
    std::ifstream in(f, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3);
    std::wstring text = Wide(raw);
    std::vector<std::wstring> lines;
    for (size_t pos = 0;;) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    std::wstring section;
    int header = -1;
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        std::wstring body = lines[i], comment;
        size_t hash = body.find(L'#');
        if (hash != std::wstring::npos) {
            comment = body.substr(hash);
            body = body.substr(0, hash);
        }
        std::wstring tb = Trim(body);
        if (!tb.empty() && tb.front() == L'[' && tb.back() == L']') {
            section = Lower(tb.substr(1, tb.size() - 2));
            if (section == sectionName) header = static_cast<int>(i);
            continue;
        }
        size_t eq = tb.find(L'=');
        if (section == sectionName && eq != std::wstring::npos && Lower(Trim(tb.substr(0, eq))) == key) {
            lines[i] = key + L" = " + value + (comment.empty() ? L"" : L"    " + comment);
            done = true;
        }
    }
    if (!done) {
        if (header < 0) {
            lines.push_back(L"[" + sectionName + L"]");
            header = static_cast<int>(lines.size()) - 1;
        }
        lines.insert(lines.begin() + header + 1, key + L" = " + value);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::ofstream out(f, std::ios::binary);
    out << "\xEF\xBB\xBF";
    for (const std::wstring& l : lines) out << Utf8(l) << "\r\n";
}

Cmd FindCommand(UINT mods, UINT vk) {
    for (const KeyBind& k : g_cfg.keys)
        if (k.mods == mods && k.vk == vk) return k.cmd;
    return Cmd::None;
}

// Vigila la carpeta de configuración: cualquier cambio avisa a la ventana principal.
constexpr UINT WM_APP_CONFIG = WM_APP + 4;
void ConfigWatcherMain() {
    std::wstring dir = ConfigDir();
    if (dir.empty()) return;
    HANDLE ch = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                 FILE_NOTIFY_CHANGE_SIZE);
    if (ch == INVALID_HANDLE_VALUE) return;
    while (WaitForSingleObject(ch, INFINITE) == WAIT_OBJECT_0) {
        PostMessageW(g_hwnd, WM_APP_CONFIG, 0, 0);
        if (!FindNextChangeNotification(ch)) break;
    }
    FindCloseChangeNotification(ch);
}

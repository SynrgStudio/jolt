# Jolt (nombre provisorio)

Explorador de archivos para Windows, pensado para que abrir carpetas se sienta instantáneo.
C++20 + Win32 + Direct2D/DirectWrite, sin dependencias externas.

## Instalar

El mismo `jolt.exe` es el instalador. Copialo a cualquier lado y abrilo: ofrece instalarse en `%LOCALAPPDATA%\Jolt` sin
pedir permisos de administrador. Desde ☰ → Instalación se prenden y apagan el PATH, el explorador predeterminado y el
inicio con Windows. Al hacerlo predeterminado se guarda lo que había y al apagarlo vuelve. Las
compilaciones con `jolt.dev` al lado no lo ofrecen solas.

## Siempre listo

Cerrar la ventana la esconde y Jolt sigue abierto: vuelve al instante con el atajo global, el icono de la bandeja o
abriendo `jolt.exe carpeta`, que usa la instancia abierta. ☰ → Salir y `jolt.exe --salir` lo cierran de verdad.
`jolt.exe --oculto` arranca sin mostrar la ventana. Opciones en `config.txt`: `segundo_plano`, `atajo_global`,
`icono_bandeja`, `abrir_discos`.

## Compilar

```
build.bat           optimizado (/O2)         ~2 s
build.bat rapido    sin optimizar (/Od)      ~1 s, para iterar
build.bat limpiar   borra build\
```

Requiere Visual Studio 2022 con la carga de trabajo de C++. Genera `build\jolt.exe` y `build\bench.exe`.

La primera vez tarda más: guarda el entorno del compilador en `build\env.bat` (para no correr `vcvars64.bat`
en cada build) y genera el header precompilado con todo lo del sistema (`src\pch.h`, ~580 mil líneas que así
se procesan una sola vez). Si agregás un header del sistema nuevo, ponelo en `src\pch.h` y corré `build.bat limpiar`.

## Usar

`build\jolt.exe [carpeta]`

| Tecla / mouse | Acción |
|---|---|
| ↑ ↓ RePág AvPág Inicio Fin | moverse |
| Enter / doble click | abrir |
| → / ← | entrar en la carpeta resaltada / subir un nivel (en el árbol, ← pliega y vuelve a la carpeta de arriba) |
| Shift+flechas / Shift+click | seleccionar un rango |
| Ctrl+click / Ctrl+Espacio | agregar o quitar de la selección |
| arrastrar desde el vacío o la parte libre de una fila | **seleccionar con un recuadro**. Shift agrega, Ctrl alterna, Esc cancela; cerca del borde la lista se desplaza. Sobre el icono, el nombre o una selección de varios, arrastrar sigue moviendo archivos |
| Ctrl+A / Esc | seleccionar todo / dejar solo el cursor |
| escribir letras | **filtro difuso** (como fzf, sin importar tildes): filtra y ordena por coincidencia; Retroceso edita, Esc limpia |
| Ctrl+P | **Ir a**: recientes, favoritos, lugares del sistema, unidades y **todo lo de los discos** (índice global) con búsqueda difusa; escribir una ruta (`C:\Us…`, `%APPDATA%`, `~`) autocompleta sus carpetas. Ctrl+Enter abre en pestaña nueva |
| Ctrl+Shift+F | **buscar en subcarpetas**: el filtro busca en toda la carpeta y sus subcarpetas (se ve dónde está cada resultado); Esc vuelve |
| Ctrl+Shift+E | abrir la carpeta que contiene lo resaltado (útil en la búsqueda y el árbol) |
| chevrón ›, Shift+→ / Shift+← | **árbol**: desplegar / plegar subcarpetas dentro de la lista. Se recuerda por carpeta |
| Espacio | **vista previa** a la derecha: imágenes (con orientación EXIF), texto y código, contenido de carpetas, y miniaturas del Shell (PDF, videos, Office…). Rueda: desplazar |
| Ctrl+S | **vista dividida**: dos paneles, cada uno con sus pestañas. Tab cambia de panel; el divisor se arrastra (doble click: mitad y mitad) |
| F5 / F6 | con la vista dividida: copiar / mover la selección a la carpeta del otro panel |
| Alt+R | **tamaño de carpetas**: calcula todas las de la lista (o las seleccionadas); quedan calculadas también todas sus subcarpetas, y se recuerdan entre sesiones (`%LOCALAPPDATA%\Jolt\tamanos.bin`). Ordenar por tamaño las tiene en cuenta |
| F2 | renombrar (queda seleccionado el nombre sin la extensión); **Tab / Shift+Tab** guarda y pasa al siguiente / anterior |
| Ctrl+N / Ctrl+Shift+N | archivo nuevo / carpeta nueva, listos para renombrar |
| Ctrl+Shift+D | duplicar ("nombre - copia.ext") |
| Ctrl+Shift+C | copiar la ruta de lo seleccionado (o de la carpeta), con comillas solo si hacen falta |
| Alt+Enter | propiedades de Windows |
| Ctrl+C / Ctrl+X / Ctrl+V | copiar, cortar y pegar, compatible con el Explorador (progreso, conflictos y deshacer de Windows) |
| Supr / Shift+Supr | a la papelera / borrar definitivamente |
| Ctrl+Z / Ctrl+Y o Ctrl+Shift+Z | **deshacer / rehacer** lo hecho desde Jolt en esta sesión (hasta 100 pasos): renombrar, mover, copiar, duplicar, crear y mandar a la papelera. Nunca pisa nada ni toca lo que cambió después, y lo que quita va a la papelera. No cubre lo que reemplazó o combinó algo que ya existía, ni el borrado definitivo |
| arrastrar y soltar | entre carpetas, paneles, pestañas, favoritos y unidades de Jolt, y desde/hacia otras apps; mismas reglas que el Explorador (Ctrl copia, Shift mueve) |
| Ctrl+Enter / click medio | abrir carpeta en pestaña nueva |
| Retroceso / Alt+↑ / botón ↑ | carpeta superior |
| Alt+← / Alt+→ / botones laterales del mouse | atrás / adelante |
| click en una miga de la ruta | ir a esa carpeta (click medio: en pestaña nueva) |
| Ctrl+L o click en el espacio libre de la ruta | escribir una ruta (acepta `%VARIABLES%`) |
| Ctrl+K | **paleta de comandos**: todos los comandos, orden, favoritos, pestañas y configuración. **Clic en el atajo de un comando** → presioná la tecla nueva y queda guardada en `config.txt` (Supr la quita, Esc cancela) |
| Ctrl+T / Ctrl+W / Ctrl+Tab | pestaña nueva / cerrar / siguiente (del panel activo) |
| Ctrl+Shift+T | reabrir la última pestaña cerrada, en su lugar y con su historial |
| Ctrl+Alt+W | cerrar las demás pestañas del panel |
| Alt+1 … Alt+8, Alt+9 | ir a esa pestaña del panel / a la última |
| Ctrl+rueda | cambiar de pestaña en el panel que está debajo: arriba, la siguiente; abajo, la anterior |
| Win+J (desde cualquier app) | abrir Jolt; si ya está al frente, esconderlo. Con el Explorador al frente, abre su carpeta. Se cambia con `atajo_global` |
| Ctrl+Shift+rueda | mover la pestaña activa del panel que está debajo: arriba, a la derecha; abajo, a la izquierda |
| Ctrl+Alt+T / Ctrl+Alt+E | abrir la terminal (Windows Terminal, o cmd) / el Explorador en esta carpeta |
| Ctrl+Alt+L | ir a la ubicación real de un acceso directo (también enlaces simbólicos y junctions) |
| Ctrl+B o el botón a la izquierda de las pestañas | mostrar / ocultar la barra lateral |
| arrastrar desde la barra de pestañas | mover la ventana (doble click: maximizar; click derecho: menú del sistema) |
| click medio en una pestaña | cerrarla |
| Ctrl+D | agregar o quitar la carpeta actual de Favoritos |
| Ctrl+1 … Ctrl+9 | ir al favorito N |
| click en Nombre / Modificado / Tamaño | ordenar (segundo click invierte) |
| arrastrar el borde izquierdo de Modificado o Tamaño | cambiar el ancho de la columna (doble click: justo para el contenido). Se guarda en `config.txt` |
| Ctrl + / Ctrl - / Ctrl+0 | agrandar, achicar o volver al tamaño normal de toda la interfaz (se guarda como `escala`) |
| click derecho / Shift+F10 / tecla Menú | menú contextual de Windows (archivo, carpeta o fondo); con Shift, el extendido |
| ☰ (arriba a la izquierda) | menú de la app: editor del menú contextual, carpeta de configuración, salir |
| Ctrl+H | mostrar / esconder los archivos ocultos (se guarda en `config.txt`) |
| Ctrl+K → "toggle…" | mostrar u ocultar carpetas, archivos o extensiones (sin atajo por defecto: se asigna con un click en la paleta) |
| Ctrl+K → "agrupar…", "plegar…", "desplegar…" | agrupar esta carpeta por día o por tipo, sacar los grupos, plegar o desplegar todos |
| Ctrl+K → "ordenar por tipo", "espacio entre filas", "vaciar la papelera" | sin atajo por defecto; la papelera pide confirmación de Windows |
| Ctrl+R | recargar |
| F12 | mostrar/ocultar tiempos |

Al cerrar se guardan las pestañas de cada panel, la vista dividida, la vista previa, la posición de la ventana, la
barra lateral y el orden (`%APPDATA%\Jolt\sesion.txt`), y las carpetas recientes de Ctrl+P (`recientes.txt`). Si se
pasa una carpeta por línea de comandos, se abre en una pestaña nueva además de las restauradas. Con `JOLT_DEBUG=1`
en el entorno, escribe un registro en `%TEMP%\jolt.log`.

Los favoritos se guardan en `%APPDATA%\Jolt\favoritos.txt`, una línea por carpeta (`Nombre|Ruta`).
Se puede editar a mano; se lee al iniciar.

## Configuración

`%APPDATA%\Jolt\config.txt` se genera la primera vez con todos los valores y comentarios, y **se recarga sola al
guardar** (también `favoritos.txt` y `menu.txt`). Secciones:

- `[general]`: fuente, tamaño, alto de fila, ancho de la barra lateral y de la vista previa, mostrar ocultos, y `carpetas_primero`
  (`si` / `no` / `nombre`: por defecto las carpetas van arriba solo al ordenar por nombre; por fecha y tamaño se
  mezclan con los archivos, que es lo que uno espera).
- `[columnas]`: qué columnas mostrar y sus anchos.
- `[colores]`: todos los colores de la interfaz en RRGGBB.
- `[atajos]`: un comando por línea con sus teclas (`subir = Retroceso, Izquierda, Alt+Arriba`). Por defecto, como
  en un commander: **→ entra en la carpeta resaltada y ← sube un nivel**. Las flechas arriba/abajo, RePág/AvPág,
  Inicio/Fin, Esc, Ctrl+Espacio, Ctrl+1..9 y Alt+1..9 son fijas. Si borrás una opción, vuelve al valor por defecto.

## Vistas: ordenar, agrupar y filtrar (por carpeta)

El botón de filtro a la izquierda de "Nombre" (o Ctrl+K → "Vista de esta carpeta") abre las reglas de la vista,
estilo Notion: **Ordenar por** (nombre, fecha, tamaño, tipo; varias reglas = desempates), **Agrupar** (por día, mes,
tipo o extensión: la lista se separa en secciones con título), **Carpetas** (primero, al final, mezcladas) y
**Mostrar solo / Ocultar** (nombre contiene, extensión es, más nuevos/viejos que N días, más grandes/chicos que N MB,
carpetas, archivos, ocultos). Cada parte se cambia con un desplegable, las reglas se reordenan arrastrando la
agarradera y todo se aplica al instante.

Arriba se elige qué se edita: **Todas las carpetas** o **Solo «esta carpeta»** (con la casilla *y sus subcarpetas*,
vale también para todo lo de adentro). Cuando una carpeta tiene vista propia, el botón se ve azul. Cómo se combinan:
primero los grupos, dentro de cada grupo las carpetas, y después el orden (la regla de más arriba manda). Hacer click
en los encabezados de columna cambia el orden de la vista de esa carpeta.

Se guardan en `%APPDATA%\Jolt\vistas.txt` (se puede editar a mano y se recarga solo):

```
[todas]
carpetas = nombre
ordenar = fecha desc

[C:\Users\yo\Downloads]
subcarpetas = no
ordenar = nombre asc
agrupar = dia
carpetas = primero
ocultar = extension tmp part crdownload
```

Click en el título de un grupo lo pliega o lo despliega.

## Índice de los discos (Ctrl+P)

Jolt arma un índice de todos los discos fijos para que Ctrl+P encuentre cualquier carpeta o archivo al instante.
Se guarda en `%LOCALAPPDATA%\Jolt\indice-X.bin`: al arrancar está disponible enseguida, lo que se crea con Jolt
abierto se suma solo, lo que se borra se descarta al mostrar resultados, y si el índice tiene más de 12 horas se
vuelve a armar en segundo plano (con prioridad baja, sin molestar). Sin permisos, recorrer un disco grande lleva unos
minutos la primera vez; con Ctrl+K → *Reindexar los discos ya* Jolt pide permiso de administrador una vez y lee la
tabla del disco (MFT, como Everything), que tarda segundos.

## Menú contextual

Es el menú real de Windows (el mismo que ve el Explorador, con las extensiones instaladas), pedido al Shell
y dibujado por Jolt. Desde ☰ → *Menú contextual…* se abre un editor que muestra ese menú para tres contextos
(fondo de la carpeta, archivo, carpeta) y permite **ocultar opciones** (casilla) y **reordenarlas** (arrastrar,
o Ctrl+↑/↓). Se guarda en `%APPDATA%\Jolt\menu.txt`, que también se puede editar a mano:

```
[fondo]
-anycode              ← oculto
git_shell
---                   ← separador
properties
```

Cada opción se identifica por su "verbo" (lo que muestra la columna gris del editor). Lo que no figure en el
archivo se agrega al final, en el orden que lo da Windows. Los submenús no se personalizan todavía.

## Probar sin tocar nada (modo de prueba)

`build\jolt.exe --prueba guion.txt [--salida carpeta]` corre Jolt sin mostrar la ventana (no le roba el foco a nadie):
dibuja en un bitmap en memoria, ejecuta el guion y guarda capturas PNG (con los menús emergentes encima). Usa una copia
de la configuración en `%TEMP%\jolt-prueba\datos`, así que no toca la sesión ni nada del usuario. Pasos, uno por línea:

```
ir C:\carpeta          tecla Ctrl+Shift+F      escribir texto        comando ver_carpetas
click 300 118          doble x y / derecho x y  mover x y            rueda x y -3
esperar 300            captura nombre.png       tamano 1100 700       eco texto (va a log.txt)
apretar x y            arrastrar x y            levantar x y          mantener Alt / mantener nada
medir 300              (primer cuadro, duración de los cuadros y etapas del arranque)
atajo / abrir_ruta C:\x / pestanas / disco D si / explorador / estado / reg clave | valor / regponer clave | valor | dato
seleccion              (escribe en el registro cuántos y cuáles hay seleccionados, el cursor y el desplazamiento)
existe C:\a.txt         tocar C:\a.txt (agrega un byte o lo crea)      mensaje (anota el aviso de abajo)
```

Los clicks van en coordenadas de la ventana (las de las capturas). El registro queda en `capturas\log.txt`.

Con `--prueba-escritorio guion.txt` corre igual pero en un **escritorio de Windows aparte**: las ventanas del sistema
que se abran (Propiedades, el diálogo de conflicto al copiar) quedan ahí, invisibles. Pasos extra:

```
esperar_ventana Properties | 6000      captura_ventana Properties | p.png      boton Properties | Cancel
cerrar_ventana Properties              ventanas  (anota las abiertas)
arrastrar_sobre 400 300 copiar C:\a.txt | C:\b.txt   (como si viniera de otra app; o "seleccion")
soltar / cancelar_arrastre             sacar C:\destino copiar  (la selección soltada en otra carpeta)
activar no / activar si                (la ventana pierde o recupera el foco)
```

Los botones se aprietan con UI Automation, buscándolos por su texto (en el idioma de Windows). Si un paso abre algo
modal (copiar con conflicto), el guion sigue corriendo adentro de esa espera. Lo único que no se puede automatizar es
el permiso de administrador: Windows lo muestra en un escritorio seguro, a propósito fuera del alcance de los programas.

## Medir

`build\bench.exe <carpeta> [iteraciones]` compara cuánto tarda listar la carpeta con
`NtQueryDirectoryFile` (lo que usa Jolt), `FindFirstFileEx` y el Shell de Windows.

## Cómo es rápido

- Lista con `NtQueryDirectoryFile` en lotes de 64 KiB, en un hilo aparte.
- Cada carpeta visitada queda en memoria; al volver se muestra al instante y se revisa en segundo plano.
- Vigila la carpeta actual y se actualiza sola si algo cambia.
- Precarga la carpeta seleccionada (discos locales) para que Enter no tenga que leer el disco.
- Iconos: los que dependen solo de la extensión se resuelven al momento sin tocar el disco; los que dependen
  del archivo (carpetas, .exe, accesos directos) se piden a otro hilo y solo para las filas visibles.
- Dibuja solo lo visible.

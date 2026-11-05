# Plan de Jolt

Acordado el 2026-11-04. Se implementa en este orden.

| # | Función | Estado |
|---|---|---|
| 1 | Deshacer con `Ctrl+Z` | hecho |
| 2 | Jolt siempre listo | aprobado |
| 3 | Renombrar varios a la vez | aprobado |
| 4 | Comandos propios, también en el botón derecho | aprobado |
| 5 | Comprimir y extraer | aprobado, en tres etapas |
| 6 | Temas | opcional, al final si sigue interesando |

## 1. Deshacer con Ctrl+Z (hecho)

Está en `src/undo.inl`; las reglas de uso están en el README.

Límites que quedaron:
- Lo que llega arrastrado desde otras apps y las demás opciones del menú contextual de Windows no quedan registradas.
- Lo que reemplazó o combinó algo que ya existía no se puede deshacer: `Ctrl+Z` pasa a la operación anterior.
- Si deshacer falla, esa operación sale del historial.
- Una carpeta con una pestaña abierta más adentro no se puede renombrar: Windows la da por en uso.

## 2. Jolt siempre listo (hecho, ver `plan-implementacion.md`)

**Qué hace:** cerrar la ventana la esconde y Jolt sigue vivo. Un atajo global la trae al instante desde cualquier app.

**Cómo:**
- **Una sola instancia:** hoy no existe. Se agrega un mutex, y `jolt.exe carpeta` le pasa la ruta por `WM_COPYDATA` a
  la instancia abierta.
- **Atajo global:** `RegisterHotKey`, configurable en `config.txt`.
- **Bandeja del sistema:** un ícono para abrir y salir, que se puede sacar. ☰ → Salir cierra de verdad.
- **Al esconder:** guardar la sesión, por si Windows se apaga o Jolt se cuelga.
- **Al mostrar:** revisar la carpeta actual, por si cambió mientras estaba escondido.
- **Opciones nuevas:** `segundo_plano`, `atajo_global`, `icono_bandeja` e `iniciar_con_windows`. Esta última viene
  apagada.

**A resolver:**
- Si `Win+E` se puede tomar. Windows lo reserva, así que `RegisterHotKey` puede rechazarlo. Si es así, usamos otra
  combinación antes que un truco frágil.
- Cuánta memoria ocupa escondido.

**Probar:** el atajo global y la bandeja necesitan una prueba real, porque el modo de prueba no los ejercita. Avisar
antes de hacerla.

## 3. Renombrar varios a la vez

**Qué hace:** `F2` con varios archivos seleccionados abre un panel con la lista de nombres y el resultado en vivo. Con
uno solo, `F2` sigue igual.

**Opciones:**
- Buscar y reemplazar.
- Plantilla con `{nombre}`, `{ext}` y `{n}`, con número inicial y cantidad de dígitos.
- Mayúsculas, minúsculas y Tipo Título.

**Cómo:**
- Antes de aplicar, marcar nombres repetidos, caracteres inválidos y choques con archivos que ya existen. Con errores
  no se aplica nada.
- Renombrar en dos pasos, con nombres temporales, para que funcionen los intercambios (`a → b` y `b → a`).
- Si algo falla a mitad de camino, volver todo atrás.
- Queda como una sola operación en `Ctrl+Z`, por eso va después del punto 1.

**Probar:** intercambios, repetidos, nombres que solo cambian de mayúsculas, numeración y `Ctrl+Z`.

## 4. Comandos propios

**Qué hace:** cada comando definido aparece en `Ctrl+K`, puede tener atajo y puede sumarse al botón derecho.

Archivo nuevo, `%APPDATA%\Jolt\comandos.txt`, que se recarga solo al guardar:

```
[Abrir en VS Code]
correr = code {carpeta}
menu = fondo, carpeta
atajo = Ctrl+Alt+C

[Convertir a MP4]
correr = ffmpeg -i {archivo} {sin_ext}.mp4
en = terminal
menu = archivo
extensiones = mkv avi mov
```

**Campos:**
- `correr`: la línea a ejecutar.
- `en`: `terminal` deja ver la salida; `oculto` corre sin ventana.
- `menu`: en qué menús aparece, `archivo`, `carpeta` o `fondo`.
- `extensiones`: solo se ofrece para esos tipos.
- `atajo`: opcional, con el mismo formato que `[atajos]`.

**Variables, con comillas automáticas:**
- `{archivo}`: corre una vez por cada archivo seleccionado.
- `{archivos}`: corre una sola vez con todos.
- `{carpeta}`, `{nombre}`, `{sin_ext}`, `{ext}` y `{otro_panel}`.

**Cómo:**
- En el menú entran con un verbo propio, `jolt:nombre`. Así el editor de `menu.txt` los oculta y los ordena como a
  cualquier otra opción.
- En la paleta salen junto a `kCommands`, con la misma asignación de atajo por click.
- Más adelante, si hace falta: crear comandos desde la propia app.

**Probar:** comillas con espacios y acentos, uno contra varios archivos, filtro por extensión y orden en el menú. En
modo de prueba el comando se anota sin ejecutarse, y la terminal real se prueba una vez a mano.

## 5. Comprimir y extraer

Formatos que usa: zip y 7z siempre, rar solo para abrir lo que le mandan, y `.tar.zst` como formato nuevo. Usa
contraseñas. Casi siempre extrae todo y a veces saca archivos sueltos.

**Motor:** `7z.dll`, cargado solo la primera vez que se abre un comprimido, así no afecta el arranque de Jolt. Va al
lado de `jolt.exe` con su licencia (LGPL con la restricción de unRAR, que solo prohíbe crear un compresor de RAR).
Para crear `.tar.zst` se suma la biblioteca zstd (BSD), porque `7z.dll` puede leer ese formato pero no crearlo.

Se descartó libarchive. Probado el 2026-11-04 con el `tar.exe` de Windows (libarchive 3.8.9) y 7-Zip 26.02:

| Caso | libarchive | 7z.dll |
|---|---|---|
| Listar 7z con contraseña | sí | sí |
| Extraer 7z con contraseña | no: "not supported" | sí |
| 7z con nombres cifrados | no: ni lista | sí |
| Extraer zip AES con contraseña | sin error, contenido sin comprobar | sí |
| Crear zst | sí | no |

La columna de `7z.dll` en los casos de extracción sale de sus capacidades declaradas (`7z i`); hay que confirmarla
en la implementación, junto con los RAR con contraseña.

**Etapa 1, extraer (lo que más usa):**
- "Extraer aquí" y "Extraer en carpeta" en el botón derecho, `Ctrl+K` y con atajo.
- "Extraer en carpeta" no crea una carpeta doble si el comprimido ya trae una sola adentro.
- Corre en segundo plano, con progreso, cancelar y varios a la vez.
- Respeta los conflictos: nunca pisa nada sin preguntar.

**Etapa 2, entrar como carpeta:**
- Enter abre el comprimido en la lista de siempre, con filtro, orden y vista previa.
- Copiar o arrastrar saca solo lo elegido. Solo lectura.
- `tar.zst` no tiene índice: para listarlo hay que leerlo entero. Se muestra progreso.

**Etapa 3, crear:**
- zip para compartir, 7z para ocupar lo mínimo y `.tar.zst` para velocidad.
- Con contraseña: zip AES-256 y 7z con nombres cifrados. `tar.zst` no admite contraseña, así que la opción no aparece.

**Contraseñas:**
- Se piden en un cuadro de Jolt. Si es incorrecta, se vuelve a pedir sin perder lo que se estaba haciendo.
- Se recuerdan solo en memoria mientras Jolt esté abierto, nunca en disco.

**Seguridad:** rechazar las rutas que salen de la carpeta de destino (`..\`, rutas absolutas), porque un comprimido
malicioso podría escribir en cualquier lado. Un error a mitad de camino no deja archivos a medias como si estuvieran
completos.

**Probar:** cada formato con y sin contraseña, contraseña incorrecta, nombres con acentos, archivos de más de 4 GB,
comprimidos dañados y rutas maliciosas. Todo se puede en modo de prueba, salvo el cuadro de progreso real.

**Falta decidir al implementar:** cómo instalar `7z.dll` y zstd en el proyecto (fuente, versión y `build.bat`).

## 6. Temas (opcional)

Entre 4 y 5 temas cuidados, y un selector en `Ctrl+K` que los aplica en vivo mientras te movés por la lista. Los
colores ya son configurables, así que es poco trabajo.

# Plan de implementación: Jolt más liviano, siempre listo e instalable

Reemplaza al punto 2 de `plan.md`. El orden importa: primero se mide, y la memoria va antes de dejar Jolt siempre
abierto.

## Resultado

Medido en `System32` (4.868 elementos) a 1900×1050:

| Qué | Antes | Después |
|---|---|---|
| Duración de un cuadro | 2,05 ms | 0,93 ms |
| Listar y ordenar | 6,0 ms | 3,1 ms |
| Memoria privada | 348 MB | 81 MB |
| Arranque hasta el ciclo de mensajes | ~50 ms | ~50 ms |

- Hecho: índice mapeado, caché de textos armados, claves de orden (mismo orden que `StrCmpLogicalW` en 180 mil nombres).
- **Probado y descartado:** `/GL` + `/LTCG` no cambió nada medible, así que tampoco se siguió con la optimización guiada
  por uso: casi todo el tiempo está en DirectWrite y Direct2D, no en el código de Jolt.
- **Descartado sin hacer:** repintar solo lo que cambió. Con cuadros de menos de 1 ms no se justifica la complejidad.
- `Win+E` lo tiene Windows: `RegisterHotKey` lo rechaza. El atajo es `Win+J`.

## Fase 1. Medir

- Paso `medir` en el modo de prueba: tiempo hasta el primer cuadro y duración de cada cuadro.
- Con esos números se decide cuánto vale cada optimización de la fase 2. Si una no mejora nada, no se deja.

## Fase 2. Optimizaciones

1. **Índice en memoria:** leerlo directo del archivo (`MapViewOfFile`) en vez de copiarlo a memoria privada. Al
   reemplazar el archivo hay que soltar el mapeo antes, porque Windows no deja pisar un archivo mapeado.
2. **Dibujar menos:** reutilizar lo que hoy se recalcula en cada cuadro por fila visible (fechas, rutas, claves).
3. **Repintar solo lo que cambió:** recortar el dibujo a la zona inválida.
4. **Ordenar más rápido:** preparar la clave de orden una vez por archivo.
5. **Compilación:** `/GL` + `/LTCG`, y optimización guiada por uso si la diferencia lo justifica.

## Fase 3. Siempre listo

1. **Una sola instancia:** mutex. `jolt.exe carpeta` le pasa la ruta a la instancia abierta (`WM_COPYDATA`) y le da
   permiso para pasar al frente.
2. **La X esconde:** Alt+F4 también. ☰ → Salir y `jolt.exe --salir` cierran de verdad. `build.bat` pasa a usar
   `--salir`, porque `CloseMainWindow` ya no alcanza.
3. **Al esconder:** se guarda la sesión. **Al mostrar:** se revisa la carpeta actual.
4. **Atajo global:** `RegisterHotKey`, configurable. Abre y esconde. Se prueba `Win+E`; si Windows no lo cede, se usa
   otra combinación.
5. **Bandeja:** icono con click para abrir y menú para salir.
6. **`--oculto`:** arranca sin mostrar la ventana, para el inicio con Windows.
7. **Abrir donde hace falta:** si el atajo se aprieta con el Explorador al frente, Jolt abre esa carpeta.
8. **Discos nuevos:** abrir un pendrive al conectarlo. Apagado por defecto.

Opciones de `config.txt`: `segundo_plano`, `atajo_global`, `icono_bandeja`, `abrir_discos`.

## Fase 4. Instalación al estilo File Pilot

El mismo `.exe` es el instalador. Abierto desde otro lado, ofrece instalarse. Sin programa de instalación aparte.

- **Panel "Instalación"** (☰ → Instalación): instalar, agregar al PATH, explorador predeterminado, iniciar con Windows.
  Cada opción se prende y se apaga. Se abre solo la primera vez que corre desde afuera de la carpeta de instalación.
- **Instalar:** copia el `.exe` a `%LOCALAPPDATA%\Jolt\jolt.exe`, crea el acceso del menú Inicio y registra la
  desinstalación en Windows (`--desinstalar`). Si ya hay otra versión instalada, la actualiza.
- **PATH:** `Path` del usuario en `HKCU\Environment`, avisando a Windows del cambio.
- **Predeterminado:** `HKCU\Software\Classes\Directory\shell\open` y `Drive\shell\open`, igual que File Pilot.
  **Hoy esas claves apuntan a File Pilot:** se guarda el valor anterior en `HKCU\Software\Jolt` y al apagar la opción se
  restaura.
- **Iniciar con Windows:** valor en `HKCU\...\Run` con `--oculto`.
- **Las compilaciones de desarrollo** (`build\jolt.dev` al lado del exe) no ofrecen instalarse solas.
- **Desinstalar:** deshace todo lo anterior y borra el exe. Los datos del usuario quedan.

## Pruebas

- Todo el registro y las carpetas pasan por rutas redirigidas en el modo de prueba, para no tocar el sistema real.
- **Nada se activa solo en la máquina del usuario:** instalar, predeterminado e inicio con Windows los aprieta el usuario.
- El atajo global, la bandeja real y el reemplazo del Explorador necesitan una prueba a mano.

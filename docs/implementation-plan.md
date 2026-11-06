# Implementation plan: a lighter Jolt, always ready and installable

Replaces item 2 of `plan.md`. The order matters: measure first, and memory comes before leaving Jolt always
open.

## Result

Measured on `System32` (4,868 items) at 1900×1050:

| What | Before | After |
|---|---|---|
| Frame duration | 2.05 ms | 0.93 ms |
| List and sort | 6.0 ms | 3.1 ms |
| Private memory | 348 MB | 81 MB |
| Startup to the message loop | ~50 ms | ~50 ms |

- Done: mapped index, cache of built strings, sort keys (same order as `StrCmpLogicalW` across 180k names).
- **Tried and dropped:** `/GL` + `/LTCG` changed nothing measurable, so profile-guided optimization wasn't pursued
  either: almost all the time is in DirectWrite and Direct2D, not in Jolt's code.
- **Dropped without trying:** repainting only what changed. With frames under 1 ms the complexity isn't justified.
- `Win+E` belongs to Windows: `RegisterHotKey` rejects it. The hotkey is `Win+J`.

## Phase 1. Measure

- `measure` step in test mode: time to the first frame and duration of each frame.
- Those numbers decide how much each phase 2 optimization is worth. If one doesn't improve anything, it isn't kept.

## Phase 2. Optimizations

1. **In-memory index:** read it straight from the file (`MapViewOfFile`) instead of copying it to private memory. When
   replacing the file, the mapping must be released first, because Windows doesn't allow overwriting a mapped file.
2. **Draw less:** reuse what is recalculated today on each frame per visible row (dates, paths, keys).
3. **Repaint only what changed:** clip drawing to the invalid area.
4. **Sort faster:** prepare the sort key once per file.
5. **Build:** `/GL` + `/LTCG`, and profile-guided optimization if the difference justifies it.

## Phase 3. Always ready

1. **Single instance:** mutex. `jolt.exe folder` passes the path to the running instance (`WM_COPYDATA`) and gives it
   permission to come to the foreground.
2. **The X hides:** Alt+F4 too. ☰ → Quit and `jolt.exe --quit` really close it. `build.bat` switches to
   `--quit`, because `CloseMainWindow` is no longer enough.
3. **On hide:** the session is saved. **On show:** the current folder is rechecked.
4. **Global hotkey:** `RegisterHotKey`, configurable. Opens and hides. Try `Win+E`; if Windows doesn't give it up, use
   another combination.
5. **Tray:** icon with click to open and a menu to quit.
6. **`--hidden`:** starts without showing the window, for starting with Windows.
7. **Open where needed:** if the hotkey is pressed with File Explorer in front, Jolt opens that folder.
8. **New drives:** open a USB drive when it is plugged in. Off by default.

`config.txt` options: `run_in_background`, `global_hotkey`, `tray_icon`, `open_drives`.

## Phase 4. File Pilot style installation

The same `.exe` is the installer. Opened from elsewhere, it offers to install itself. No separate setup program.

- **"Install" panel** (☰ → Install): install, add to PATH, default file explorer, start with Windows.
  Each option turns on and off. It opens by itself the first time it runs from outside the install folder.
- **Install:** copies the `.exe` to `%LOCALAPPDATA%\Jolt\jolt.exe`, creates the Start menu shortcut and registers the
  uninstaller in Windows (`--uninstall`). If another version is already installed, it updates it.
- **PATH:** the user's `Path` in `HKCU\Environment`, notifying Windows of the change.
- **Default:** `HKCU\Software\Classes\Directory\shell\open` and `Drive\shell\open`, same as File Pilot.
  **Today those keys point to File Pilot:** the previous value is saved in `HKCU\Software\Jolt` and restored when the
  option is turned off.
- **Start with Windows:** value in `HKCU\...\Run` with `--hidden`.
- **Development builds** (`build\jolt.dev` next to the exe) don't offer to install on their own.
- **Uninstall:** undoes all of the above and deletes the exe. The user's data stays.

## Tests

- All registry access and folders go through redirected paths in test mode, so the real system isn't touched.
- **Nothing activates by itself on the user's machine:** install, default and start with Windows are pressed by the user.
- The global hotkey, the real tray and the File Explorer replacement need a manual test.

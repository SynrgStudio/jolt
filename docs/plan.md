# Jolt plan

Agreed on 2026-11-04. To be implemented in this order.

| # | Feature | Status |
|---|---|---|
| 1 | Undo with `Ctrl+Z` | done |
| 2 | Jolt always ready | approved |
| 3 | Rename several at once | approved |
| 4 | Custom commands, also on right click | approved |
| 5 | Compress and extract | approved, in three stages |
| 6 | Themes | optional, at the end if still of interest |

## 1. Undo with Ctrl+Z (done)

It is in `src/undo.inl`; the usage rules are in the README.

Remaining limits:
- What arrives by dragging from other apps and the other Windows context menu options are not recorded.
- What replaced or merged with something that already existed can't be undone: `Ctrl+Z` moves on to the previous operation.
- If an undo fails, that operation leaves the history.
- A folder with a tab open deeper inside can't be renamed: Windows considers it in use.

## 2. Jolt always ready (done, see `implementation-plan.md`)

**What it does:** closing the window hides it and Jolt stays alive. A global hotkey brings it back instantly from any app.

**How:**
- **Single instance:** doesn't exist today. Add a mutex, and `jolt.exe folder` passes the path via `WM_COPYDATA` to
  the running instance.
- **Global hotkey:** `RegisterHotKey`, configurable in `config.txt`.
- **System tray:** an icon to open and quit, which can be removed. ☰ → Quit really closes it.
- **On hide:** save the session, in case Windows shuts down or Jolt hangs.
- **On show:** recheck the current folder, in case it changed while hidden.
- **New options:** `run_in_background`, `global_hotkey`, `tray_icon` and `start_with_windows`. The last one is off by
  default.

**To resolve:**
- Whether `Win+E` can be taken. Windows reserves it, so `RegisterHotKey` may reject it. If so, we use another
  combination rather than a fragile trick.
- How much memory it uses while hidden.

**Test:** the global hotkey and the tray need a real test, because test mode doesn't exercise them. Give notice
before doing it.

## 3. Rename several at once

**What it does:** `F2` with several files selected opens a panel with the list of names and the live result. With
just one, `F2` stays the same.

**Options:**
- Find and replace.
- Template with `{name}`, `{ext}` and `{n}`, with a starting number and digit count.
- Uppercase, lowercase and Title Case.

**How:**
- Before applying, flag repeated names, invalid characters and clashes with files that already exist. With errors,
  nothing is applied.
- Rename in two steps, with temporary names, so that swaps work (`a → b` and `b → a`).
- If something fails halfway, roll everything back.
- It counts as a single operation for `Ctrl+Z`, which is why it comes after item 1.

**Test:** swaps, repeats, names that only change case, numbering and `Ctrl+Z`.

## 4. Custom commands

**What it does:** each defined command appears in `Ctrl+K`, can have a shortcut and can be added to the right-click menu.

New file, `%APPDATA%\Jolt\commands.txt`, which reloads itself on save:

```
[Open in VS Code]
run = code {folder}
menu = background, folder
shortcut = Ctrl+Alt+C

[Convert to MP4]
run = ffmpeg -i {file} {no_ext}.mp4
in = terminal
menu = file
extensions = mkv avi mov
```

**Fields:**
- `run`: the line to execute.
- `in`: `terminal` shows the output; `hidden` runs without a window.
- `menu`: which menus it appears in, `file`, `folder` or `background`.
- `extensions`: only offered for those types.
- `shortcut`: optional, in the same format as `[shortcuts]`.

**Variables, with automatic quoting:**
- `{file}`: runs once for each selected file.
- `{files}`: runs once with all of them.
- `{folder}`, `{name}`, `{no_ext}`, `{ext}` and `{other_pane}`.

**How:**
- In the menu they enter with their own verb, `jolt:name`. That way the `menu.txt` editor hides and orders them like
  any other option.
- In the palette they appear alongside `kCommands`, with the same click-to-assign shortcut.
- Later, if needed: creating commands from the app itself.

**Test:** quoting with spaces and accents, one versus several files, extension filter and menu order. In
test mode the command is logged without running, and the real terminal is tested once by hand.

## 5. Compress and extract

Formats in use: zip and 7z always, rar only to open what gets sent, and `.tar.zst` as a new format. Passwords are
used. It almost always extracts everything and sometimes pulls out individual files.

**Engine:** `7z.dll`, loaded only the first time an archive is opened, so it doesn't affect Jolt's startup. It sits
next to `jolt.exe` with its license (LGPL with the unRAR restriction, which only forbids creating a RAR compressor).
To create `.tar.zst`, the zstd library (BSD) is added, because `7z.dll` can read that format but not create it.

libarchive was ruled out. Tested on 2026-11-04 with Windows' `tar.exe` (libarchive 3.8.9) and 7-Zip 26.02:

| Case | libarchive | 7z.dll |
|---|---|---|
| List password-protected 7z | yes | yes |
| Extract password-protected 7z | no: "not supported" | yes |
| 7z with encrypted names | no: can't even list | yes |
| Extract password-protected AES zip | no error, content unchecked | yes |
| Create zst | yes | no |

The `7z.dll` column for the extraction cases comes from its declared capabilities (`7z i`); it has to be confirmed
during implementation, along with password-protected RAR files.

**Stage 1, extract (the most used):**
- "Extract here" and "Extract to folder" on right click, in `Ctrl+K` and with a shortcut.
- "Extract to folder" doesn't create a double folder if the archive already has a single one inside.
- Runs in the background, with progress, cancel and several at once.
- Respects conflicts: never overwrites anything without asking.

**Stage 2, enter as a folder:**
- Enter opens the archive in the usual list, with filter, sorting and preview.
- Copying or dragging extracts only what was chosen. Read-only.
- `tar.zst` has no index: listing it means reading all of it. Progress is shown.

**Stage 3, create:**
- zip for sharing, 7z for minimum size and `.tar.zst` for speed.
- With a password: AES-256 zip and 7z with encrypted names. `tar.zst` doesn't support passwords, so the option doesn't appear.

**Passwords:**
- Asked for in a Jolt dialog. If wrong, it asks again without losing what was in progress.
- Remembered only in memory while Jolt is open, never on disk.

**Security:** reject paths that leave the destination folder (`..\`, absolute paths), because a malicious archive
could write anywhere. An error halfway doesn't leave partial files looking
complete.

**Test:** each format with and without a password, wrong password, names with accents, files over 4 GB,
damaged archives and malicious paths. All possible in test mode, except the real progress dialog.

**Still to decide at implementation:** how to bring `7z.dll` and zstd into the project (source, version and `build.bat`).

## 6. Themes (optional)

Between 4 and 5 polished themes, and a selector in `Ctrl+K` that applies them live as you move through the list.
Colors are already configurable, so it is little work.

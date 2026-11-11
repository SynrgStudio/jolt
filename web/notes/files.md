---
title: Working with files
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Everything goes through Windows' own machinery, so progress, conflicts and the Recycle Bin behave as in File Explorer. Then Jolt adds undo on top.

| Keys | What it does |
|---|---|
| Ctrl+C / Ctrl+X / Ctrl+V | copy, cut and paste, compatible with File Explorer both ways. Pasting in the same folder makes a copy with another name |
| Ctrl+V with an image or text on the clipboard | saves it in the folder as a PNG (keeping transparency) or a UTF-8 `.txt`, named by the moment and ready to rename |
| drag and drop | between Jolt's folders, panes, tabs, favorites and drives, and from or to other apps. Same rules as File Explorer: Ctrl copies, Shift moves. Held over a tab for half a second, that tab comes to the front, so the drop can go into one of its folders; a quick drop on the tab goes into its folder |
| Ctrl+N / Ctrl+Shift+N | new file / new folder, ready to rename |
| right click → Compress to ZIP / 7z | compresses the selection next to it, named after the item (or after the folder, for several). Zip works with what Windows already has; with 7-Zip installed it is faster, and 7z works too. The progress shows in the status bar, the palette's "archive stop" stops it, and Ctrl+Z sends the new archive to the Recycle Bin |
| right click → Extract here / Extract to "name\" | extracts zip, 7z, rar, tar.gz, cab, iso and the like. "Here" puts an archive holding a single folder (or file) right in the folder and anything else in a folder named after it, so nothing spills out; "to" always makes that folder. With 7-Zip installed it does the work; without it, zip goes through Windows and the rest through Windows' own `tar.exe`, which can't write some non-Latin names (Jolt says so) |
| Enter on a zip, 7z, rar, tar or cab | goes into it like a folder: its folders, sizes and dates, the preview of what is inside, the path bar and Backspace work as anywhere. Enter on a file opens a copy taken out of it, Ctrl+C copies things out to paste anywhere, dragging takes copies out to drop on a tab, the other pane, File Explorer or the desktop, F5 extracts the selection into the other pane, and the right-click menu extracts it next to the archive. It is read-only: nothing inside can be renamed, deleted or changed. `browse_archives = no` (or Settings) opens archives with their program instead; jar, apk, epub, iso and the like have "Browse inside" in their right-click menu |
| Ctrl+Shift+D | duplicate ("name - Copy.ext") |
| Ctrl+Shift+G | **group in a new folder**: moves the selection into a new folder next to it (named after the item if there is one) and starts renaming the folder |
| Del / Shift+Del | to the Recycle Bin / delete permanently (with Windows' own confirmation) |
| Ctrl+Shift+C / Ctrl+Alt+C | copy the path (quoted only if needed) / copy just the name. "Copy the path with forward slashes" is in the palette |
| Alt+R | **folder sizes**: calculates every folder in the list, or the selected ones. Subfolders get calculated too, sorting by size uses them, and they are remembered between sessions |
| Alt+Enter | Windows properties |
| Shift+Alt+Enter | **attributes and dates** of the selection: hidden and read-only as switches (a dash when the items differ: those stay as they are), and the modified and created dates as text, with a Now button. Nothing changes until Apply |
| Ctrl+K → "in use" | **who has it open**: the programs holding the selection (asked to the Restart Manager), each with a Close button: its windows are asked to close; a program without windows is ended, and one that stays gets Force. When a copy, move, delete or rename fails because a file is open, the same panel opens by itself with the reason and goes again once they're closed, or with Try again. Other failures say why on the status line, with no Windows dialog |
| Ctrl+K → "unblock" | **unblock** downloaded files: drops the Zone.Identifier mark from the selection (folders: everything inside), so Windows stops warning about them |
| Ctrl+Alt+T / Ctrl+Alt+E | open a terminal (Windows Terminal, or cmd) / File Explorer in this folder |
| right click / Shift+F10 / Menu key | the Windows context menu for the file, the folder or the background; with Shift, the extended one. See context menu |

## Tags and colors

Right click → **Tags**, or Ctrl+K → "tag", puts a label on the selection: pick a known one or type a new name. Each tag gets a color and shows as a little tag-shaped label after the name, with its name inside while there is room (a dot when there isn't, and in the grid's corner), and in the preview's info; an item can have several. `tag:urgent` in the filter keeps the tagged items, `!tag:done` leaves them out, in subfolder search too. They are saved in `%APPDATA%\Jolt\tags.txt`, which can be edited by hand (`name = RRGGBB` under `[tags]` sets a color), and they follow renames done from Jolt.

Right click → **Folder color**, or Ctrl+K → "folder color", paints the icon of the selected folders: red, orange, green, teal, blue, purple, pink or gray, keeping its shading, in the list, the grid and the sidebar. It is Jolt's own: Windows and the folder's `desktop.ini` aren't touched. The colors are saved in `tags.txt` under `[folder_colors]` (`folder = RRGGBB`, any color by hand) and follow renames. A folder icon set in Windows (Properties → Customize, which writes `desktop.ini`) shows as it is.

Names can also take a color by rule, in `config.txt`:

```
[file_colors]
.jpg .png .gif .webp = 7FB3FF
.zip .7z .rar = C9A86A
*backup* *.bak = E06C75
folders = E6B450
```

## Undo

Ctrl+Z undoes and Ctrl+Y (or Ctrl+Shift+Z) redoes what was done from Jolt in this session, up to 100 steps: rename, move, copy, duplicate, create, group, paste and send to the Recycle Bin. It never overwrites anything or touches what changed afterwards, and what it removes goes to the Recycle Bin. It doesn't cover what replaced or merged with something that already existed, or permanent deletion.

## Status bar

Item count and total size of the folder (folders count once their size has been calculated). With several items selected, their count and size too. While filtering, how many match.

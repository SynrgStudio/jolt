---
title: The basics: moving, selecting, tabs, split view
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

It works like a commander: the keyboard does everything, the mouse does the same.

## Moving around

| Keys | What it does |
|---|---|
| ↑ ↓ PageUp PageDown Home End | move the cursor |
| → / ← | enter the highlighted folder / go up one level. In the tree, ← collapses and returns to the folder above |
| Enter / double click | open (folders in Jolt, files with their program; the program starts in the file's folder) |
| Backspace / Alt+↑ / ↑ button | parent folder, with the folder you came from highlighted |
| Alt+← / Alt+→ / mouse side buttons | back / forward |
| click a breadcrumb | go to that folder (middle click: in a new tab). Click the free space of the path, or Ctrl+L, to type a path (`%VARIABLES%` and `~` work) |
| Network | in the sidebar, Ctrl+P or the path bar: your computer, mapped drives and the servers you've been to right away, and what Windows finds as it turns up. `\\server` lists its shared folders, and a share that asks for a password offers **Sign in**. A server that doesn't answer never holds up the rest of Jolt |
| Recycle Bin | in the sidebar, Ctrl+P or the path bar. It's a folder like the others: the filter, sorting and the preview work, with the original location next to each name and the deletion date in its column. Ctrl+Shift+R or right click restores, Del deletes for good after asking |
| cloud folders | in OneDrive and the like, a **cloud** next to the name marks what is online only and a **check** what is always on this PC, in the list, the grid and the preview (`cloud_status` in `config.txt` turns the marks off). Listing, the preview, thumbnails, icons and content search never download anything: an online-only file shows as such until you open it |
| a folder that can't be opened | Jolt stays where it is and says why in grey next to the name, with what can be done: **Open as administrator**, or **Go to the real folder** on Windows' protected links like `Application Data`. A typed path shows the same on the folder's page |
| Ctrl+Shift+Enter | opens the highlighted folder, or this one, in a **window as administrator**: a separate Jolt with a badge on the title bar that lists every folder, closes for real and leaves your session alone. The global shortcut and Win+E hide it and bring it back along with the normal window. What you open from it runs as administrator too |
| click a breadcrumb's chevron, right click a breadcrumb, or Alt+↓ | the **folder dropdown**: the chevron lists the folders inside that segment, a right click the ones next to it, and it opens on the folder you are in. Click to go (middle click: in a new tab), or use the arrows and type the first letters to jump |
| chevron ›, Shift+→ / Shift+← | **tree**: expand or collapse subfolders inside the list. Remembered per folder |
| Ctrl+Shift+E | open the folder containing the highlighted item (handy in search results and in the tree) |
| Ctrl+Alt+L | go to the real location of a shortcut, symbolic link or junction |

## Selecting

| Keys | What it does |
|---|---|
| Shift+arrows / Shift+click | select a range |
| Ctrl+click / Ctrl+Space | add to or remove from the selection |
| Ctrl+A / Esc | select all / keep only the cursor |
| Ctrl+Alt+A | **select by pattern**: type the same terms as the filter, like `*.png`, `report*`, `>10mb`, `ext:pdf,jpg`, `type:folder`, `newer:7d`, `!tmp` or `tag:done`; plain words are names that contain them. The list shows what it takes while you type. Enter selects it, the other rows add it to the selection or take it out, and Esc leaves the selection as it was |
| Ctrl+Alt+X / Ctrl+I | select every file with the extension of the highlighted one (on a folder: every folder) / invert the selection |
| checkbox | **selection checkboxes**, with `checkboxes = hover` or `always` (Settings → General, or Ctrl+K → "checkboxes"): a click on a row's checkbox adds it to the selection or takes it out, without Ctrl; Shift+click checks the range. The one in the header selects all, or none when all are. The selection a folder opens with (just the cursor) goes with the first checkbox |
| drag from empty space | **box select**. Shift adds, Ctrl toggles, Esc cancels; near the edge the list scrolls. Dragging from an icon, a name or a multiple selection moves files instead |

## Tabs and panes

| Keys | What it does |
|---|---|
| Ctrl+T / Ctrl+W / Ctrl+Tab | new tab / close / next tab. Middle click on a tab closes it |
| Ctrl+Enter / middle click | open the folder in a new tab |
| Ctrl+Shift+T | reopen the last closed tab, in its place and with its history |
| Alt+1 … Alt+9 | go to that tab (9: the last one). Ctrl+Alt+W closes the others |
| Ctrl+wheel / Ctrl+Shift+wheel | over the tab bar: switch tab / move the active tab |
| Ctrl+Alt+wheel | **view**: up, a grid of thumbnails with bigger and bigger cells; down, back to the list. Per folder. See views |
| Ctrl+Alt+P, or right click on a tab | **pin the tab**: it stays at the left with a short name, Ctrl+W and "close the others" leave it alone, and it comes back with the session |
| Ctrl+S | **split view**: two panes, each with its own tabs. Tab switches pane; drag the divider (double click: half and half) |
| F5 / F6 | in split view: copy / move the selection to the other pane's folder |
| Shift+F5 / Shift+F6 | **copy to… / move to…**: pick the destination with the Go to list (recents, favorites, drives, or a typed path), no dragging. Ctrl+Z undoes it |
| drag the tab bar | move the window (double click maximizes; right click: system menu) |

On close, Jolt saves each pane's tabs, the split, the preview, the window position, the sidebar and the sort order, and restores them next time.

## Sidebar

Ctrl+B, or the button left of the tabs, shows and hides it.

**Zen mode** (F11, or ☰ → Zen mode) leaves only the tabs, the list and the status line: the sidebar slides out and the navigation bar slides up. Ctrl+L brings the path bar back while you type a path; F11, Ctrl+B or the sidebar button bring everything back. It is remembered between sessions.

- **Favorites:** Ctrl+D adds or removes the current folder; Ctrl+1…9 jumps to one. Drag to reorder, or drag any folder onto the list to add it; right click to rename or remove. Saved in `%APPDATA%\Jolt\favorites.txt` as `Name|Path` lines.

- **Drives:** hover one and its row turns into the usage bar with free and total space, updated every second.

- **Recents:** sorted by how much you use them, not by last opened. A folder earns a point when you stay at least 1.5 s, and its heat halves every 30 days. `recents = 0` in `config.txt` hides the section.

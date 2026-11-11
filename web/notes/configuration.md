---
title: Configuration
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Ctrl+, opens the settings. Underneath, plain text files in `%APPDATA%\Jolt`; each one reloads itself when saved.

The settings are a panel over the window, in sections: **General** (hidden files, folders first, close names in searches, Recents, cloud status, columns, running in the background, the global shortcut), **Appearance** (font, interface size, row height, widths, and every color), **Shortcuts** (type to search; click one and press the new key, Del removes it), **File colors**, your own **Commands**, **Tags** (their colors), **Favorites** (their names), and **More** (the context menu editor, the view of this folder, and the files themselves). Every change is written to the files below as you make it, keeping their comments, and applies at once. Also in ☰ → Settings… and Ctrl+K.

| Keys | What it does |
|---|---|
| `config.txt` | generated the first time with every value and a comment. `[general]`: font, size, row height, sidebar and preview width, hidden files, `folders_first` (yes / no / name), `recent_mark` (today / no / a number of hours: a thin bar at the left of the files changed lately, a dot in the grid, fading with the hours), background, hotkey, tray. `[columns]`: which columns, left to right, and their widths (in the app: right click the titles to pick them, drag a title to move its column, drag its edge to resize): `modified`, `size`, and also `dimensions` (images and videos), `duration` (songs and videos), `items` (how many a folder has) and `attributes` (R H S A C E, L for links). `size_bars = yes` draws a bar behind each size, as long as the item is next to the largest in the folder (folders count once their size is calculated with Alt+R); it is also in the titles' right-click menu. `[colors]`: every interface color in RRGGBB. `[shortcuts]`: one command per line with its keys. `[commands]`: your own commands. Deleting an option returns it to its default |
| `favorites.txt` | `Name\|Path`, one per line |
| `views.txt` | the views per folder |
| `menu.txt` | the context menu order and hidden entries |
| `tags.txt` | the tags: their colors and which items have them; and the folder colors |
| `session.txt`, `recents.txt` | tabs and window, and the recent folders with their heat |
| `%LOCALAPPDATA%\Jolt` | the installed program, the drive index, the calculated folder sizes (`sizes.bin`) and the thumbnail cache (`thumbs\`, safe to delete) |

`☰ → Configuration folder` opens it. With `JOLT_DEBUG=1` in the environment, Jolt writes a log to `%TEMP%\jolt.log`.

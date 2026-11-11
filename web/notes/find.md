---
title: Finding things: filter, Ctrl+P, search, duplicates
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Three ways: type in the folder, search its subfolders, or search every drive.

## Type to filter

Just start typing in any folder. It filters **fuzzily**, like fzf: `rpt` finds *report.pdf*, accents are ignored, and the best matches rise to the top. Backspace edits, Esc clears. Letters that are configured as shortcuts don't filter. See filter terms for narrowing by extension, size or date.

**When nothing here matches**, a note under "No matches" says where there is something: how many in the subfolders and how many on the drives, each with the shortcut that shows them (Ctrl+Shift+F, or Ctrl+P, which opens with your words already typed). A click on it does the same. It is looked for in the background a moment after you stop typing: the drives' index answers at once, and the subfolders are walked up to a limit, on local drives only.

## Ctrl+Shift+F — search subfolders

The same filter, over the whole folder and everything inside it. Results show where each one is; Ctrl+Shift+E opens the folder of the highlighted result. Esc goes back.

**The results work like any list.** Delete, rename (F2 or the rename panel), move to the other pane (F6), group into a folder, cut and copy, drag to another app, and the right-click menu, all with items from several subfolders at once. The results update as you go: what you delete or move leaves, renamed items take their new name, and a new folder is searched too. Ctrl+A selects the real matches and leaves the close names out.

## Ctrl+Alt+F — duplicate files

Looks through the folder and its subfolders for identical files. Files of the same size are compared by their content: a SHA-256 of the first 64 KB and, when that matches, of the whole file. Each set of copies shows as a group, with the oldest copy first and the sets that free the most space on top; the status line says how much could be freed.

Ctrl+Shift+A selects every copy but the oldest of each set, so Del sends the extras to the Recycle Bin. Each set then stays in the list with the copy that is left, marked "only copy left", so you can see what was kept. Everything from subfolder search works here too, and the filter narrows the sets: `>1mb`, `.jpg` or a name. Cloud files that are only online are left out, since comparing them would download them, and so are hard links to the same file, which don't take space twice. Esc or Ctrl+Alt+F goes back.

## Ctrl+P — go to anything

Recents, favorites, system places, drives and **everything on your drives**, with fuzzy search. Names exactly like what you typed and programs come first, the list loads more as you scroll, and it says how many there are. Typing a path (`C:\Us…`, `%APPDATA%`, `~`) autocompletes its folders. Enter opens, Ctrl+Enter opens in a new tab.

**It learns what you mean.** Among results that match about as well, what you use most lately goes higher: every folder you stay in and every file you open counts, and that fades over the weeks. So does what is near the folder you're in, more the fewer folders lie between: inside a project, `config` finds that project's config before every other program's. And it remembers what you pick: after picking the same result three times for the same text, that one comes first for that text, even above names that match better; three picks of something else change it. Long paths are shortened in the middle (`C:\Users\…\project\file.txt`), so the drive and the last folders stay in sight.

Behind it is an index of the fixed drives, saved in `%LOCALAPPDATA%\Jolt\index-X.bin`: available at once on startup, kept up to date while Jolt runs, and rebuilt in the background at low priority when it is older than 12 hours. Walking a large drive without elevated rights takes a few minutes the first time; `Ctrl+K → Reindex the drives now` asks for administrator rights once and reads the drive's own table (the MFT), which takes seconds.

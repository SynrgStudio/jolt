---
title: Renaming, one or many
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

One item inline, or many at once with a preview.

## F2 — one item

The name without the extension is selected. Tab / Shift+Tab save and move on to renaming the next / previous item, so a column of files can be renamed without leaving the keyboard.

## F2 with several selected, or Ctrl+F2 — many at once

A panel shows every result before anything is renamed. Invalid names, names already taken and repeated results are marked in red, and Rename stays off until they are fixed. Enter renames, Esc cancels, Tab moves between the boxes, and Ctrl+Z takes the whole batch back in one step. Shifts like 1 → 2 → 3 go through temporary names, so nothing collides.

**New name** is a pattern:

| Keys | What it does |
|---|---|
| `{name}` `{name:lower}` `{name:upper}` `{name:title}` | the name without its extension, as is or with its case changed (`{NAME}` and `{Name}` work too) |
| `{ext}` `{ext:upper}` | the extension, with its dot |
| `{n}` `{n:3}` `{n:3:10}` `{n:3:10:5}` | a counter: padding, start, step. `{n:3:10:5}` gives 010, 015, 020… |
| `{date}` `{date:YYYYMMDD_hhmm}` `{time}` `{created}` | modified or created date, as YYYY-MM-DD or with a format made of YYYY YY MM DD hh mm ss |
| `{parent}` | the folder's name |
| `{random}` `{random:12:letters}` `{random:6:digits}` `{uuid}` | random text, drawn once per item so the preview holds still |

**Find / Replace** runs on the name before the pattern: plain text replaces every occurrence, ignoring case; `/regex/` uses a regular expression, with `$1` `$2` for its groups in Replace.

Examples: `Trip {n:2}{ext}` — Trip 01.jpg, Trip 02.jpg…   `{date}-{name}{ext}` — the date in front.   Find `/^IMG_(\d+)/`, Replace `photo-$1` — IMG_0042.jpg becomes photo-0042.jpg.   `{name:title}{ext}` — Title Case.

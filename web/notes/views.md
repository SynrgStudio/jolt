---
title: Views per folder
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Sort, group and filter each folder its own way. Rules apply instantly and are remembered.

The filter button left of "Name" (or `Ctrl+K → View for this folder`) opens the rules, Notion style: **Sort by** (name, date, size, type; several rules act as tiebreakers), **Group** (by day, month, type or extension: the list splits into titled sections you can collapse), **Folders** (first, last, mixed) and **Show only / Hide** (name contains, extension is, newer or older than N days, larger or smaller than N MB, folders, files, hidden). Rules are reordered by dragging their handle.

At the top you choose **All folders** or **Only this folder** (with *and its subfolders* to apply it to everything inside). When a folder has its own view, the button turns blue. Clicking the column headers changes the sort order of that view. They are saved in `%APPDATA%\Jolt\views.txt`, which can be edited by hand and reloads itself:

```
[all]
folders = name
sort = date desc

[C:\Users\me\Downloads]
subfolders = no
sort = name asc
group = day
folders = first
hide = extension tmp part crdownload
```

## List or grid

Each folder can be a **grid of thumbnails** instead of a list. Ctrl+Alt+wheel up makes the cells bigger (80 to 256 px), down goes back to the list; the palette has the same as commands. In the grid, the arrows walk the cells, and everything else works as in the list: typing filters, box select, drag and drop, the preview. The thumbnails come from the Shell (photos, videos, PDF, Office, and the icon for the rest) and are cached on disk, so a folder you have seen opens with its pictures already there. It is saved per folder in `views.txt` as `layout = grid 144`.

Ctrl+Alt+M turns a tab into **Miller columns**: the folder you are in is a column, the folders above it are columns to its left with the way down highlighted, and what the highlighted folder holds shows in the column to its right. ← and → move between them, a click on another column goes there (the columns stay put, so what you clicked doesn't move), a double click opens, and the wheel scrolls the column under the mouse. The folder's own column is the usual list: selection, rename, drag and drop and typing to filter work as always. The columns share the pane, each about `miller_width` wide (240 by default), and the tab remembers the mode between sessions.

Quick toggles in the palette (Ctrl+K): show or hide folders, files, hidden files (Ctrl+H) and extensions; group this folder by day or type; collapse or expand all groups; sort by type; more or less space between rows. Any of them can get a key with a click.

---
title: The context menu
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

The real Windows menu, with your extensions, drawn by Jolt and trimmed by you.

`☰ → Context menu…` opens an editor that shows the menu for three contexts (folder background, file, folder) and lets you **hide entries** (checkbox) and **reorder them** (drag, or Ctrl+↑/↓). Saved in `menu.txt`:

```
[background]
-anycode              ← hidden
git_shell
---                   ← separator
properties
```

Each entry is identified by its verb (the gray column in the editor). Whatever isn't listed is added at the end in the order Windows gives it. The editor's fourth tab is the **"New" submenu**: Windows' entries and your templates, hidden and reordered the same way, saved under `[background/new]`. Other submenus can't be customized yet. Delete goes through Jolt so it stays in the undo history.

## Templates

Every file or folder in `%APPDATA%\Jolt\Templates` is an entry at the top of the right-click **New** submenu, and in Ctrl+K as "New: name" (the `new_from_template` command lists just them). A copy lands in the folder, named like the template (or "name (2)"), ready to rename; a folder template is copied with everything in it. "Templates folder…" at the end of the submenu, and Settings → More, opens the folder, making it the first time.

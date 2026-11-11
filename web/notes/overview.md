---
title: What Jolt is
status: stable
updated: 2026-11-11
---

Jolt is a file explorer for Windows 10 and 11. It does what File Explorer does, and is built to be fast and to be
driven from the keyboard. It is one executable of about 2.5 MB with no dependencies: the same `jolt.exe` is the
program and its installer, and it installs for the current user without administrator rights. It is written in
C++20 directly on Win32, Direct2D and DirectWrite, with no UI framework. It sends nothing over the network unless
update checks are turned on.

## What it does, in short

- **Folders open at once.** It reads folders with `NtQueryDirectoryFile` in 64 KiB batches on a separate thread,
  keeps every visited folder in memory (going back shows it without reading the disk, and rechecks it in the
  background), and preloads the highlighted folder. See how-it-stays-fast.md and measurements.md.
- **Find anything.** Typing filters the current folder with fuzzy matching and filter terms (`*.png`, `>10mb`,
  `newer:7d`, `ext:pdf,jpg`, `tag:done`…). Ctrl+P searches every name on every drive from Jolt's own index, and ranks
  what you use most first. Ctrl+Shift+F searches subfolders, and the `content:` term searches the text inside files.
- **Preview** with Space: images (with EXIF), SVG, fonts, Markdown rendered, code with syntax highlighting, CSV as a
  table, what an archive holds, songs and videos that play, Shell thumbnails, and hex for binaries.
- **Tabs and split view**, restored as they were: two panes, each with its own tabs.
- **Undo**: Ctrl+Z and Ctrl+Y for renames, moves, copies, deletes and new items, up to 100 steps.
- **Keyboard first**: a command palette (Ctrl+K) with every command, and any shortcut can be changed.
- **Views per folder**: list, grid of thumbnails or Miller columns; sorting, grouping and filtering remembered per
  folder, with rules.
- **Recents that learn**: folders rise by how much they are really used, and fade over weeks.
- **The real context menu**: Windows' own, with the user's extensions, which can be trimmed and reordered.
- **Batch rename**, duplicates finder, archives (zip read directly; compress to zip, or 7z when 7-Zip is installed),
  tags, folder colors, custom commands, a window as administrator.
- **Always ready**: closing the window hides it; Win+J brings it back from anywhere.

## What it is not, yet

- Windows only.
- Releases are not code-signed yet: SmartScreen may warn on the first run.
- The interface is in English only.
- No screen reader support (UI Automation) or high-contrast theme, and no light theme.
- One window per user (plus an optional window as administrator); split view has two panes.
- No phones or cameras over MTP. No portable mode.

All of these are open issues on the public roadmap (roadmap.md).

---
title: Always ready: running in the background
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Closing the window hides it. Jolt stays open and comes back instantly.

- Win+J from any app shows Jolt; if it is already in front, hides it. With File Explorer in front, it opens that folder in Jolt. Change the key with `global_hotkey`.

- The tray icon does the same with a click; right click to quit.

- `jolt.exe C:\some\folder` opens the folder in the running instance (a new tab, or the tab that already has it).

- `☰ → Quit` or `jolt.exe --quit` really close it. `jolt.exe --hidden` starts without showing the window.

Options in `config.txt`: `run_in_background`, `global_hotkey`, `tray_icon`, `open_drives` (open USB sticks as soon as they are plugged in).

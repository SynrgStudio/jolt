---
title: Command line
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

| Keys | What it does |
|---|---|
| `jolt.exe C:\folder` | open the folder (in the running instance if there is one) |
| `jolt.exe --select "C:\folder\file.txt"` | open the folder with that item highlighted, even if it was just created. What other apps' "Reveal in File Explorer" needs; Explorer's `/select,` form works too |
| `jolt.exe --hidden` | start without showing the window |
| `jolt.exe --toggle` | show or hide, like the global shortcut |
| `jolt.exe --quit` | really close the running instance |
| `jolt.exe --panel` | open with the install panel |
| `jolt.exe --uninstall` | remove the installed copy and its registrations; settings are kept |
| `jolt.exe --test script.txt` | run a test script without a window; see for developers |

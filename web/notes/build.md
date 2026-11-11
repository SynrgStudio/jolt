---
title: Building, testing and releasing Jolt
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

## Build

```
build.bat           optimized (/O2)          ~2 s
build.bat fast      unoptimized (/Od)        ~1 s, for iterating
build.bat clean     deletes build\
```

Requires Visual Studio 2022 with the C++ workload. Produces `build\jolt.exe` and `build\bench.exe`. The first build saves the compiler environment in `build\env.bat` and generates the precompiled header (`src\pch.h`); if you add a system header, put it there and run `build.bat clean`. `bench.exe <folder>` compares the listing APIs.

## Test mode

`jolt.exe --test script.txt [--output folder]` runs Jolt without a window: it draws into a bitmap, runs the script and saves PNG captures, using a copy of the configuration in `%TEMP%\jolt-test\data` so nothing of the user's is touched. Steps, one per line:

```
go C:\folder        key Ctrl+Shift+F      type text          command show_folders
click 300 118       double x y            right x y          wheel x y -3
press x y           drag x y              release x y        hold Alt / hold none
wait 300            capture name.png      size 1100 700      echo text
selection           rows                  message            exists C:\a.txt
touch C:\a.txt      open_path C:\x        select_path C:\x   rename_preview
usage_set path | points | days            measure 300        quit
```

Every feature on this page was checked this way before it shipped. With `--test-desktop` the same runs on a separate Windows desktop, so system dialogs (properties, copy conflicts) stay out of sight; extra steps drive those windows by their text through UI Automation.

## Release

Bump `src/version.h` and push to master: GitHub Actions builds, tags and publishes the release, with the SHA-256 in the notes. The latest build is always at `github.com/SynrgStudio/jolt/releases/latest/download/jolt.exe`. This site is published from `site/` by the same kind of workflow.

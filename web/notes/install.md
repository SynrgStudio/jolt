---
title: Getting and installing Jolt
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

One file. The same `jolt.exe` is the program and the installer.

1. [Download jolt.exe](https://github.com/SynrgStudio/jolt/releases/latest/download/jolt.exe). It isn't signed yet, so Windows SmartScreen may warn you the first time: *More info → Run anyway*. See Privacy & signing.

2. Open it from anywhere. It offers to install itself to `%LOCALAPPDATA%\Jolt`, for your user only, with no administrator rights.

3. In the install panel (`☰ → Install`) you can turn on **Add to PATH** (type `jolt` in any terminal), **Default file explorer** (folders open with Jolt; what was there before is saved and comes back if you turn it off) and **Start with Windows** (starts hidden, ready for the shortcut).

Updating is the same: open a newer `jolt.exe` and press **Update**. To uninstall, use `☰ → Install → Uninstall`, Windows Settings → Apps, or `jolt.exe --uninstall`. Your settings and favorites stay in `%APPDATA%\Jolt`.

> **Win+E.** Windows doesn't give up its own shortcut, so Jolt installs a handler for it when it is the default explorer. The global shortcut Win+J works from anywhere regardless.

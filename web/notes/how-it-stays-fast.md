---
title: How Jolt stays fast
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

C++20, Win32 and Direct2D/DirectWrite. No external dependencies, one executable.

## How it stays fast

- Lists with `NtQueryDirectoryFile` in 64 KiB batches, on a separate thread.

- Every visited folder stays in memory; going back shows it instantly and rechecks it in the background.

- Watches the current folder and updates itself when something changes.

- Preloads the highlighted folder (local drives) so Enter doesn't have to read the disk.

- Icons that depend only on the extension are resolved on the spot; those that depend on the file (folders, .exe, shortcuts) come from another thread, for visible rows only.

- Draws only what is visible.

- Shell thumbnails are made on a thread and cached on disk (`%LOCALAPPDATA%\Jolt\thumbs`, PNG, by path, size and modified time; entries unused for 60 days are dropped), so the preview is instant the second time.

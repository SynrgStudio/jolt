---
title: How Jolt's numbers were measured
status: stable
updated: 2026-11-11
---

Every number in `facts.toml` comes from one of the runs below, on one PC: AMD Ryzen 7 5800X (8 cores, 16 threads),
32 GB of RAM, an NVMe SSD (Lexar NM620), Windows 11 Pro 10.0.26200, on 2026-11-10. Nothing here is estimated.

## Reading a folder: `bench.exe`

`build\bench.exe <folder> <runs>` is in the repository (`src/bench.cpp`). It reads the same folder several ways and
prints the first, fastest and median time of each:

- **NtQueryDirectoryFile in 64 KiB batches**: how Jolt reads a folder (`src/dirlist.cpp`, the same code as in 0.5.0).
- **The same plus sorting** in natural order with folders first, the way File Explorer and Jolt show a folder.
- **FindFirstFileEx**: the classic Win32 way most programs use.
- **The Windows Shell** (`IShellItem`, name, date and size): the interface File Explorer is built on.
- **The Shell plus each file's icon index.**

The folders were made just before the runs, on the NVMe SSD: 1,000, 10,000 and 100,000 files of 0 to 499 bytes,
named `file 000000.txt` onwards; and `C:\Windows\System32` (4,868 entries). Medians are of 20 runs for 1,000 and
10,000 files, and of 3 for 100,000 and System32 (the Shell with icons takes 90 seconds per run there). All rows are in
`data/listing-2026-11-10.csv`.

| files | Jolt reads | Jolt reads and sorts | FindFirstFileEx | Shell (name, date, size) |
|---:|---:|---:|---:|---:|
| 1,000 | 0.47 ms | 0.71 ms | 0.53 ms | 16 ms |
| 10,000 | 4.3 ms | 6.9 ms | 4.7 ms | 130 ms |
| 100,000 | 46.7 ms | 71.4 ms | 49.2 ms | 1,247 ms |

What this shows, and what it doesn't:

- Reading straight from the file system is 17 to 35 times faster than going through the Shell (sorted or not).
  This compares reading a folder, not the two programs as a whole.
- FindFirstFileEx is about as fast as Jolt's way here: the classic API is not slow, the Shell is. Jolt reads on a
  thread of its own, so the window keeps responding while it reads.
- **Windows' file cache was warm** (the files had just been made). A cold read, after a restart or of a folder not
  touched for a long time, takes longer, and was not measured.
- This is reading and sorting, not the whole time until the folder is on screen. Drawing it is the next section;
  icons for files whose icon depends on the file itself come later, from another thread, for the visible rows only.

## The released program, without a window: `jolt.exe --test`

`jolt.exe 0.5.0`, downloaded from the GitHub release (its SHA-256 matched the published one), run in Jolt's own test
mode: `jolt.exe --test script.txt --output folder`. In this mode the window is not shown, it draws into a bitmap in
memory, it uses a copy of the settings in `%TEMP%`, and it doesn't build the drive index for Ctrl+P. The window was
1280×800. The script opened the 1,000, 10,000 and 100,000-file folders and System32, and ran `measure`, which draws
the window a number of times and logs how long each frame took, plus the time stamps of startup.

- **Startup**: 105 ms from the process starting until settings, window, session and tabs were ready. The first frame
  on a real screen was not timed.
- **Drawing** with the 100,000-file folder open: 3.1 ms per frame (median of 300), 3.9 ms at the 95th percentile.
  Jolt draws only the visible rows, so this barely depends on the folder's size. Drawing into a bitmap in memory may
  not be the same as on screen.
- **Memory**: sampled every half second with the process's counters; at the end 52 MB private, 70 MB working set,
  83 MB peak working set. Without the drive index: with it, Jolt uses more, and that was not measured on a release.

## Not measured yet

- Opening a folder end to end on a real screen (key press to finished frame). Jolt shows it in the status bar
  (`open … ms · disk … + sort …`), but reading it needs a real window.
- A cold read, with Windows' file cache empty.
- Showing the window from the global shortcut (Win+J), with Jolt running in the background.
- Memory at rest of a release build with the drive index.

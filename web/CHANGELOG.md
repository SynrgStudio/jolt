# Changes to Jolt's corpus

## 2026-11-11 (2)

- `notes/measurements.md`: the ratio to the Shell said "17 to 35"; from the CSV's medians it is 17.5 to 34.4 in the
  three test folders. Now it gives each one, and System32's (20.0 and 44.3).
- `notes/find.md`: the drive index note no longer names another tool (rule 3).
- `rules.md`, rule 2: it covers performance numbers. Numbers in the notes that describe behavior (defaults,
  thresholds, limits) come from the source and can be used as written.

## 2026-11-11

- First version of the corpus, for Jolt 0.5.0.
- `facts.toml`: version, license, links, the PC the numbers were measured on, and 16 facts:
  - the size of `jolt.exe`, the number of commands and the undo steps;
  - reading 1,000, 10,000 and 100,000 files and System32, the same through the Windows Shell, and the ratio;
  - startup, frame time and memory of the released 0.5.0 without a window.
- `notes/`: one note per topic, taken from the 0.5.0 docs page, plus `overview.md` and `measurements.md`.
- `data/shortcuts.csv`: the 111 commands of 0.5.0 with their keys. `data/listing-2026-11-10.csv`: the raw listing
  benchmark.
- `media/`: the icon, the tour and three pictures, dated.
- `rules.md`.

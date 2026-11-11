# What is never published about Jolt

These rules come before everything else in this corpus.

1. **Only what a release has.** The corpus describes Jolt 0.5.0. Work that is on `master` or in open pull requests but
   not in a release isn't announced, and isn't shown in pictures, until a release ships it. Today that is: comparing
   two files or two folders, checksums, Ctrl+Shift+Enter running programs as administrator, deleting empty folders,
   flattening folders, and the disk usage panel. They come into `notes/` when they ship.
   *Why: someone who downloads Jolt from the page must find what the page shows.*

2. **No numbers that were not measured.** Every number on the page is a `fact`. No "instant start", "opens in N ms on
   screen", "cold" or "uses N MB at rest" until `facts.toml` has it. "Folders open instantly" is fine as a description
   of how it feels; next to it, use the measured times and say they are with Windows' file cache warm.
   *Why: the facts are what make the page trustworthy, and the ones that are missing were not measured.*

3. **Comparisons only with their numbers.** Jolt can be compared with File Explorer through the Windows Shell
   numbers in `facts.toml`, saying what was compared (reading a folder, not the whole program). No other file
   managers are named or compared with: none were measured.
   *Why: an unmeasured comparison is a claim we can't back.*

4. **No disk paths, user names or folder names from the author's PC**, beyond what is already in the published
   pictures in `media/`. Measurements describe their folders by what they hold (100,000 files), not by where they
   were.
   *Why: privacy.*

5. **Code signing is a plan, not a fact.** Say releases are not signed yet and that SmartScreen may warn. Don't say
   when they will be, or that any signing program has accepted Jolt.
   *Why: it hasn't happened.*

6. **The pictures are from 2026-11-06, before 0.1.0.** They can be used, with the dates in `facts.toml`; don't
   present them as 0.5.0 screenshots.

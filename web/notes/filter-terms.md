---
title: Filter terms
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Plain words stay the fuzzy pattern. The other terms narrow the list, in any folder and in subfolder search.

| Term | Meaning |
|---|---|
| `.png` `*.png` `ext:png,jpg` | extension (several: any of them) |
| `>10mb` `<500kb` `>2gb` | size. A bare number is MB. Folders have no size and are left out |
| `type:folder` `type:file` | kind |
| `newer:7d` `older:2w` `modified:today` | date: d h w m y, or `today` (since midnight) |
| `!tmp` `!.bak` `!*.log` | exclude by name, extension or pattern |
| `report*.pdf` `?.txt` | wildcard pattern on the name |
| `content:word` `content:"two words"` | **search inside the files**: keeps the text files whose content has it, and shows the line around the match next to the name. Files are read in the background (UTF-8, UTF-16 or the system code page; binaries and files over 32 MB are skipped). With Ctrl+Shift+F it covers the subfolders too |
| `tag:urgent` `!tag:done` | items with (or without) a tag |

**Typos are forgiven.** Names close to what you typed appear under the real matches, with how close they are in grey next to the name: `filepocker` finds `filepicker` at 90%, and swapped letters stay at 100% (`filepikcer`). It works in subfolder search, Ctrl+P and Ctrl+K too. `filter_typos` in `config.txt` sets the lowest percentage, 75 by default, or `no` to turn it off.

Examples: `content:TODO .cpp` — source files with a TODO.   `invoice .pdf newer:30d` — the invoices of the last month.   `>500mb !.iso` — what is taking space, except disc images.   `type:folder old` — folders with "old" in the name.

Only the words light up in the names. The status bar shows how many match.

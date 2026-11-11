---
title: Custom commands
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Your own entries in the palette and the context menu, from `config.txt`.

```
[commands]
Open in VS Code = code "{dir}"
Edit with Notepad = notepad "{file}"
PowerShell here = wt -d "{dir}" powershell
Compress = "C:\Program Files\7-Zip\7z.exe" a "{name}.zip" {files}

[shortcuts]
open in vs code = Ctrl+Alt+V
```

- `{file}` is the highlighted item, `{files}` everything selected (each quoted), `{dir}` the folder, `{name}` the item's name.

- The first word is the program, the rest its arguments. It runs in the folder, and a bare name is found in PATH.

- They appear in Ctrl+K, where a click on the row assigns a key, and at the end of the right-click menu, where the menu editor can hide or move them like any other entry.

- A `#` starts a comment anywhere in `config.txt`, so it can't be part of a command.

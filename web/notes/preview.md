---
title: Preview
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Space opens a panel on the right that follows the cursor. Held, it shows a big floating preview over the list instead, until you let go.

While Space is held, the arrows move through the files and the floating preview follows them; letting go hides it and leaves the panel as it was. A quick tap opens or closes the panel as always. How long counts as held is `preview_hold` in `config.txt` (150 ms by default, 50 to 1000), also in Settings: shorter is quicker, but then a slow tap may show the card for a moment instead of opening the panel.

- **Images** with their EXIF orientation, and under photos what the camera wrote: camera, lens, exposure (shutter, aperture, ISO, focal length), when and where they were taken.

- **Zoom, move and turn** images and SVG: Ctrl+wheel zooms around the mouse (up to 4000%, the pixels sharp from 300% on), dragging or the wheel moves what doesn't fit, a double click goes between fitting and the real size. With the mouse over it, buttons turn it a quarter left or right and show the zoom (a click on it: fit / real size). A photo bigger than what was first decoded is read again at its whole size when you zoom past it. See-through images sit on a checkerboard, light by default, dark with `preview_checker = dark`, Settings or the palette. The palette also has "preview rotate", "preview zoom in" and "preview zoom out". Only the view changes: the file isn't touched.

- **Copy the picture**: a right click on the preview (or the palette's "preview copy image") puts it on the clipboard. An image goes at its whole size and turned as it shows; a thumbnail or a song's cover as it is; an SVG drawn 1024 px on its long side; a playing video, the frame on screen. Opaque pictures go as a plain bitmap every program reads; see-through ones also as PNG, so their transparency survives the paste.

- **SVG** drawn by Direct2D, as big as the panel allows, on a light checkerboard. Their CSS (`style` attributes and `<style>` rules for tags, classes and ids) is applied; text inside them isn't drawn. One Direct2D can't read shows as its source.

- **Fonts** (TTF, OTF, TTC, WOFF, WOFF2): the name in the typeface itself, the alphabet and digits, and a sentence at 12, 16, 22, 30 and 40 px. The info says the family, the style and how many faces the file has. The file is read into memory, so it is never held open.

- **Markdown rendered**: headings, bold, italic, code, links, lists (nested, numbered, tasks), quotes and GitHub's `[!NOTE]` alerts, code blocks highlighted by their language, and tables. `preview_markdown = no` in `config.txt`, the palette's "preview markdown" or Settings show its source instead.

- **Text and code with syntax highlighting** (C-family, JS/TS, C#, Java, Go, Rust, Swift, Kotlin, PHP, Python, shell, PowerShell, batch, Lua, Ruby, SQL, CSS, HTML/XML, JSON, YAML, INI/TOML, Markdown, diffs, Dockerfile, Makefile), with line numbers and long lines wrapped (`preview_wrap = no` in `config.txt`, or the palette's "wrap long lines", turns that off). JSON that is all on one line is shown indented.

- **CSV and TSV as a table**: the separator is found on its own (comma, semicolon, tab or bar), the header stays on top while you scroll, columns of numbers line up to the right, and Shift+wheel moves a wide table sideways.

- **What an archive holds**, as a tree with each file's size, and below how many files and folders and how much they take unpacked. Zip and its kin (jar, apk, nupkg, vsix, whl, epub...) are read directly; 7z, rar, tar and its compressions, cab and iso go through Windows' own `tar.exe`.

- **Songs and videos play** in the panel: the play button or a click on the picture starts them, the bar below seeks (click or drag), and the time shows as it goes. Songs show their cover, title, artist and album; videos, the frame Windows has until they start. Nothing plays until asked, and it stops when the cursor moves on or the preview closes. The palette's "preview play" plays and pauses, and can be given a key.

- The **contents of a folder**, and the Shell thumbnails of anything else: PDF, videos, Office documents, executables' icons at full size. A binary Windows has nothing to show for gets its first 64 KB **in hex**, with the bytes as characters beside them.

- Below it: type, size, modified date, and the dimensions of images, the length of songs and videos, the rows and columns of a table, what an archive holds or the number of items in a folder.

- Wheel scrolls. Drag its left edge to change the width; double click the edge to reset it. The width is saved.

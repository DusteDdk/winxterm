# Third-Party Notices

Project-owned `winxterm` and `dstshell` source/assets are treated as WTFPL. The
files below are third-party material or generated from third-party material and
keep their upstream licensing.

Project-owned icon files, including `resources/winxterm_256.png` and the
generated `resources/winxterm.ico`, are not xterm-derived and are distributed
under the WTFPL.

## SQLite

Files:

- `dependencies/sqlite3/sqlite3.c`
- `dependencies/sqlite3/sqlite3.h`
- `dependencies/sqlite3/sqlite3ext.h`

Source: SQLite 3.53.3 amalgamation from
`https://sqlite.org/2026/sqlite-amalgamation-3530300.zip`.

License: SQLite source files state that the author disclaims copyright and
provides the SQLite blessing. See `dependencies/sqlite3/README.md` and the
headers of the vendored files.

## Noto Fonts

Files:

- `resources/ttf/Noto_Color_Emoji/**`
- `resources/ttf/Noto_Sans/**`
- `resources/ttf/Noto_Sans_Math/**`

License: SIL Open Font License, Version 1.1.

Copyright notices:

- Noto Color Emoji: Copyright 2021 Google Inc. All Rights Reserved.
- Noto Sans: Copyright 2022 The Noto Project Authors.
- Noto Sans Math: Copyright 2022 The Noto Project Authors.

The full OFL text is included in each font directory as `OFL.txt` and must stay
with redistributed font files.

## 6x13 Fixed Bitmap Font

Files:

- `resources/6x13-ISO8859-1.pcf`
- `resources/winxterm_font_6x13.h`
- `generated/winxterm_font_6x13.h`

License: public domain according to the PCF file metadata:
`Public domain font. Share and enjoy.`

`resources/winxterm_font_6x13.h` is generated from the PCF file by
`tools/pcf_to_header.c`.

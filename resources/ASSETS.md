# Assets

`winxterm_256.png` and `winxterm.ico` are project-owned icon assets. `winxterm_256.png` was created for this project, is not derived from xterm, and is distributed under the WTFPL. The `.rc` resource name should stay unchanged when the icon is refreshed.

`6x13-ISO8859-1.pcf` is the checked-in fixed bitmap font source. `winxterm_font_6x13.h` is generated from that PCF file by `../tools/pcf_to_header.c` and stores only the row masks used by the production renderer.

## Startup TTF Fallback Fonts

`winxterm.rc` embeds three fixed fallback fonts as `RCDATA` resources for glyphs that are not present in the built-in 6x13 bitmap atlas:

- `ttf/Noto_Sans/static/NotoSans-Regular.ttf` as the general Unicode fallback.
- `ttf/Noto_Color_Emoji/NotoColorEmoji-Regular.ttf` as the emoji fallback.
- `ttf/Noto_Sans_Math/NotoSansMath-Regular.ttf` as the math-symbol fallback.

These are bundled startup resources only; they do not add runtime font switching or xterm font menu parity. The Noto font files include their SIL Open Font License text in their source directories and should be kept with any redistribution package.

Third-party asset licenses are summarized in `../THIRD_PARTY_NOTICES.md`.

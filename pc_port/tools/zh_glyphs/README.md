# Chinese glyph tools (NTSC-J fan translation)

The Chinese translation redefines the glyphs at JIS ku 16..47 in the order it
first used each character, so writing new Chinese text means finding which JIS
code draws each hanzi.

- `cnfont.py` - loads `kanji_font_cn.inc`; `glyph(ku, ten)`, SJIS<->kuten, `sheet()` to render glyphs.
- `cnmatch.py "字符"` - ranks candidate codes for each character against Windows SimSun / YaHei.
- `cn_sheet.py "字符" out.png [top]` - renders each character beside its candidates. CONFIRM BY EYE: the
  scores are only a shortlist, and a character the translation never used is simply absent.
- `gen_jpn_ui.py` - writes `../../src/lang_jpn_ui.inc` and `lang_jpn_inv.inc` from its tables and a
  `zh_proof.png` of every Chinese string as the port draws it. Add a confirmed hanzi to `CN` before using it.

Needs Pillow and a Windows install (for the reference fonts).

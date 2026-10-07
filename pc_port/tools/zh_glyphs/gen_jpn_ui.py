"""Write lang_jpn_ui.inc / lang_jpn_inv.inc and a proof sheet of the Chinese."""
import os, sys
sys.stdout.reconfigure(encoding='utf-8')
import cnfont

# Hanzi -> JIS kuten in the Chinese fan font, each confirmed from a rendered sheet.
CN = {
    '读': (24, 84), '取': (16, 32), '继': (24, 72), '续': (21, 50), '开': (20, 19), '始': (28, 40),
    '选': (29, 56), '项': (30, 78), '退': (25, 22), '出': (17, 57), '简': (37, 91), '单': (18, 41),
    '普': (41, 65), '通': (36, 44), '高': (18, 2), '难': (18, 92), '物': (42, 10), '品': (41, 42),
    '返': (29, 24), '回': (21, 45), '地': (16, 18), '图': (16, 19), '指': (16, 83), '令': (26, 59),
    '状': (20, 16), '态': (30, 72), '使': (16, 12), '用': (16, 13), '装': (18, 69), '上': (20, 31),
    '下': (17, 54), '弹': (16, 46), '说': (22, 23), '明': (44, 32), '查': (17, 87), '看': (16, 23),
    '数': (18, 70), '量': (46, 44), '燃': (23, 27), '料': (17, 35), '关': (21, 19), '可': (18, 32),
    '不': (16, 10), '这': (16, 8), '里': (16, 9), '能': (16, 11), '太': (16, 20), '暗': (16, 21),
    '了': (16, 22), '见': (16, 24),
}

# (us literal as C source, japanese, chinese, comment)
UI = [
    ('LOAD', 'ロード', '读取'),
    ('CONTINUE', 'コンティニュー', '继续'),
    ('START', 'スタート', '开始'),
    ('OPTION', 'オプション', '选项'),
    ('EXIT', '終了', '退出'),
    ('EASY', 'イージー', '简单'),
    ('NORMAL', 'ノーマル', '普通'),
    ('HARD', 'ハード', '高难'),
    ('Use', '使う', '使用'),
    ('Equip', '装備', '装上'),
    ('Unequip', '解除', '取下'),
    ('Reload', '装填', '装弹'),
    ('Detail', '調べる', '说明'),
    ('Look', '見る', '查看'),
    ('Stock:', '残り：', '数量：'),
    ('Fuel:', '燃料：', '燃料：'),
    ('==On==', '==オン==', '==开=='),
    ('==Off==', '==オフ==', '==关=='),
    ('==Use_OK==', '==使用可==', '==可用=='),
    ('==Use_OK?==', '==使用可？==', '==可用？=='),
    ('==Use_NG==', '==使用不可==', '==不可用=='),
    ("Can't_use_it_here.", 'ここでは使えない。', '这里不能使用。'),
    ('Too_dark_to_look_at\\n\\t\\tthe_item_here.', '暗くて見ることができない', '太暗了看不见。'),
]
INV = [
    ('Equipment', '所持品', '物品'),
    ('Exit', '戻る', '返回'),
    ('Option', 'オプション', '选项'),
    ('Map', 'マップ', '地图'),
    ('Command', 'コマンド', '指令'),
    ('Status', 'ステータス', '状态'),
    ('Name:', '名前：', '物品：'),
]


def octal(bs):
    """Both bytes of every SJIS pair escaped: a trail byte can be '\\'."""
    out, i = [], 0
    while i < len(bs):
        if bs[i] >= 0x80:
            out.append('\\%03o\\%03o' % (bs[i], bs[i + 1]))
            i += 2
        else:
            assert bs[i] not in b'\\"'
            out.append(chr(bs[i]))
            i += 1
    return ''.join(out)


def enc_jp(s):
    return s.encode('cp932')


def enc_zh(s):
    out = bytearray()
    for ch in s:
        if ch in CN:
            code = cnfont.kuten_to_sjis(*CN[ch])
            out += bytes([code >> 8, code & 0xFF])
        else:
            b = ch.encode('cp932')
            if len(b) == 2:
                ku, _ = cnfont.sjis_to_kuten((b[0] << 8) | b[1])
                assert not 16 <= ku <= 47, ('kanji outside the map', ch)
            out += b
    return bytes(out)


def rows(table):
    lines = []
    for us, jp, zh in table:
        lines.append('{ "%s",%s"%s",%s"%s" }, /* %s / %s */' % (
            us, ' ' * max(1, 22 - len(us)), octal(enc_jp(jp)), ' ', octal(enc_zh(zh)), jp, zh))
    return '\n'.join(lines)


HEAD_UI = '''/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Japanese and Chinese for the title menu and the inventory's command box,
 * status line and prompts. Retail NTSC-J drew all of these in ENGLISH (the
 * words are ASCII in its BODYPROG), and the Chinese patch left them alone, so
 * both columns are written here: { us, japanese, chinese }, Shift-JIS octal
 * escapes so the file stays ASCII.
 *
 * The Chinese column is NOT Chinese encoding. The fan translation redefines
 * the glyphs at JIS ku 16..47 in first-use order, so each hanzi here is the
 * JIS code whose glyph in kanji_font_cn.inc draws it -- found by matching
 * rendered Windows CJK fonts against that font and confirmed by eye. Only
 * characters the translation itself used exist; 困, 备, 卸, 详 and 称 do not,
 * which is why Hard is 高难, Equip/Unequip are 装上/取下, Detail is 说明 and
 * Stock is 数量. Punctuation and kana outside ku 16..47 draw from the shared
 * Japanese font in both.
 *
 * Widths: the command box is sized for "Unequip" (42 px) and the status line
 * starts at x=208, so every entry is at most four 12 px cells. */
'''
HEAD_INV = '''/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The inventory's box labels, looked up by Pc_JpnInventoryLabel rather than
 * the shared menu table: "Exit" and "Map" are also Options / PC Options row
 * keys there (the options-mode sentence, and the New Game start map), and
 * this screen means neither. Same { us, japanese, chinese } layout and
 * Chinese-code caveat as lang_jpn_ui.inc. */
'''

if __name__ == '__main__':
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'src')
    open(os.path.join(src, 'lang_jpn_ui.inc'), 'w', encoding='utf-8', newline='\n').write(HEAD_UI + rows(UI) + '\n')
    open(os.path.join(src, 'lang_jpn_inv.inc'), 'w', encoding='utf-8', newline='\n').write(HEAD_INV + rows(INV) + '\n')

    # Proof sheet: every Chinese string exactly as the port will draw it.
    cells = []
    for us, jp, zh in UI + INV:
        bs = enc_zh(zh)
        i = 0
        cells.append((us[:10], None))
        while i < len(bs):
            if bs[i] >= 0x81:
                ku, ten = cnfont.sjis_to_kuten((bs[i] << 8) | bs[i + 1])
                cells.append(('', cnfont.glyph(ku, ten)))
                i += 2
            else:
                cells.append((chr(bs[i]), None))
                i += 1
        while len(cells) % 12:
            cells.append(('', None))
    cnfont.sheet(cells, 'zh_proof.png', scale=2, cols=12)
    print('wrote both tables and zh_proof.png')

/* SPDX-License-Identifier: GPL-3.0-or-later */
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
{ "LOAD",                  "\203\215\201\133\203\150", "\214\362\210\276" }, /* ロード / 读取 */
{ "CONTINUE",              "\203\122\203\223\203\145\203\102\203\152\203\205\201\133", "\214\346\213\161" }, /* コンティニュー / 继续 */
{ "START",                 "\203\130\203\136\201\133\203\147", "\212\261\216\306" }, /* スタート / 开始 */
{ "OPTION",                "\203\111\203\166\203\126\203\207\203\223", "\217\167\217\354" }, /* オプション / 选项 */
{ "EXIT",                  "\217\111\227\271", "\215\125\211\170" }, /* 終了 / 退出 */
{ "EASY",                  "\203\103\201\133\203\127\201\133", "\223\233\211\307" }, /* イージー / 简单 */
{ "NORMAL",                "\203\155\201\133\203\175\203\213", "\225\201\222\312" }, /* ノーマル / 普通 */
{ "HARD",                  "\203\156\201\133\203\150", "\211\240\211\372" }, /* ハード / 高难 */
{ "Use",                   "\216\147\202\244", "\210\252\210\253" }, /* 使う / 使用 */
{ "Equip",                 "\221\225\224\365", "\211\343\212\275" }, /* 装備 / 装上 */
{ "Unequip",               "\211\360\217\234", "\210\276\211\165" }, /* 解除 / 取下 */
{ "Reload",                "\221\225\223\125", "\211\343\210\314" }, /* 装填 / 装弹 */
{ "Detail",                "\222\262\202\327\202\351", "\213\265\226\276" }, /* 調べる / 说明 */
{ "Look",                  "\214\251\202\351", "\211\227\210\265" }, /* 見る / 查看 */
{ "Stock:",                "\216\143\202\350\201\106", "\211\344\227\312\201\106" }, /* 残り： / 数量： */
{ "Fuel:",                 "\224\122\227\277\201\106", "\214\132\211\142\201\106" }, /* 燃料： / 燃料： */
{ "==On==",                "==\203\111\203\223==", "==\212\261==" }, /* ==オン== / ==开== */
{ "==Off==",               "==\203\111\203\164==", "==\213\122==" }, /* ==オフ== / ==关== */
{ "==Use_OK==",            "==\216\147\227\160\211\302==", "==\211\276\210\253==" }, /* ==使用可== / ==可用== */
{ "==Use_OK?==",           "==\216\147\227\160\211\302\201\110==", "==\211\276\210\253\201\110==" }, /* ==使用可？== / ==可用？== */
{ "==Use_NG==",            "==\216\147\227\160\225\163\211\302==", "==\210\250\211\276\210\253==" }, /* ==使用不可== / ==不可用== */
{ "Can't_use_it_here.",    "\202\261\202\261\202\305\202\315\216\147\202\246\202\310\202\242\201\102", "\210\246\210\247\210\250\210\251\210\252\210\253\201\102" }, /* ここでは使えない。 / 这里不能使用。 */
{ "Too_dark_to_look_at\n\t\tthe_item_here.", "\210\303\202\255\202\304\214\251\202\351\202\261\202\306\202\252\202\305\202\253\202\310\202\242", "\210\262\210\263\210\264\210\265\210\250\210\266\201\102" }, /* 暗くて見ることができない / 太暗了看不见。 */

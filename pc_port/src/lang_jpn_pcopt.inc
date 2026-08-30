/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Japanese for the rows the PORT added: the PC Options pages and the few
 * main-menu entries that replaced retail ones. Nothing on the disc covers
 * these -- they did not exist in 1999 -- so unlike lang_jpn_menu.inc these
 * are written, not read back, and they are Shift-JIS octal escapes so the
 * file stays plain ASCII.
 *
 * JAPANESE ONLY, and Pc_JpnMenuText gates them on the Japanese glyph set.
 * The Chinese translation redefines the glyphs at JIS ku 16..47 -- the whole
 * level-1 kanji block -- so drawing a Japanese string under it renders every
 * kanji as an unrelated Chinese character. English is the honest fallback
 * there until someone recovers the code->character map of that font. */
{ "PC_Options",          "\202\157\202\142\203\111\203\166\203\126\203\207\203\223", }, /* ＰＣオプション */
{ "Voice",               "\203\173\203\103\203\130\211\271\227\312", }, /* ボイス音量 */
{ "Language",            "\214\276\214\352",                      }, /* 言語 */
{ "Resolution",          "\211\360\221\234\223\170",              }, /* 解像度 */
{ "Window_Mode",         "\203\105\203\102\203\223\203\150\203\105\203\202\201\133\203\150", }, /* ウィンドウモード */
{ "VSync",               "\220\202\222\274\223\257\212\372",      }, /* 垂直同期 */
{ "Texture_Filter",      "\203\145\203\116\203\130\203\140\203\203\225\342\212\324", }, /* テクスチャ補間 */
{ "Antialiasing",        "\203\101\203\223\203\140\203\107\203\103\203\212\203\101\203\130", }, /* アンチエイリアス */
{ "Post_Process",        "\203\174\203\130\203\147\203\107\203\164\203\106\203\116\203\147", }, /* ポストエフェクト */
{ "Tone_Mapping",        "\203\147\201\133\203\223\203\175\203\142\203\163\203\223\203\117", }, /* トーンマッピング */
{ "Map",                 "\212\112\216\156\203\175\203\142\203\166", }, /* 開始マップ */
{ "Flashlight",          "\211\371\222\206\223\144\223\224",      }, /* 懐中電灯 */
{ "Beam_Intensity",      "\214\365\202\314\213\255\202\263",      }, /* 光の強さ */
{ "Beam_Size",           "\214\365\202\314\215\114\202\263",      }, /* 光の広さ */
{ "Fog_Strength",        "\226\266\202\314\224\132\202\263",      }, /* 霧の濃さ */
{ "Disable_Culling",     "\203\112\203\212\203\223\203\117\226\263\214\370", }, /* カリング無効 */
{ "FPS_Limit",           "\202\145\202\157\202\162\220\247\214\300", }, /* ＦＰＳ制限 */
{ "FMV_Movie_Vol",       "\203\200\201\133\203\162\201\133\211\271\227\312", }, /* ムービー音量 */
{ "Crosshair",           "\217\306\217\200\225\134\216\246",      }, /* 照準表示 */
{ "2D_Controls",         "\202\121\202\143\221\200\215\354",      }, /* ２Ｄ操作 */
{ "Mouse_Sensitivity",   "\203\175\203\105\203\130\212\264\223\170", }, /* マウス感度 */
{ "Pad_Sensitivity",     "\203\160\203\142\203\150\212\264\223\170", }, /* パッド感度 */
{ "First_Person_FOV",    "\210\352\220\154\217\314\216\213\226\354", }, /* 一人称視野 */
{ "Third_Person_FOV",    "\216\117\220\154\217\314\216\213\226\354", }, /* 三人称視野 */
{ "Invert_Mouse_Y",      "\203\175\203\105\203\130\202\170\224\275\223\135", }, /* マウスＹ反転 */
{ "Invert_Pad_Y",        "\203\160\203\142\203\150\202\170\224\275\223\135", }, /* パッドＹ反転 */
{ "Bullet_Decals",       "\222\145\215\255\202\360\216\143\202\267", }, /* 弾痕を残す */
{ "Aim_Assist",          "\217\306\217\200\225\342\217\225",      }, /* 照準補助 */
{ "Aim_Zoom",            "\217\306\217\200\203\131\201\133\203\200", }, /* 照準ズーム */
{ "OTS_Aim_In_TPS",      "\214\250\211\172\202\265\217\306\217\200", }, /* 肩越し照準 */
{ "Camera_Collision",    "\203\112\203\201\203\211\217\325\223\313", }, /* カメラ衝突 */
{ "Minimap",             "\203\176\203\152\203\175\203\142\203\166", }, /* ミニマップ */
{ "Minimap_Scale",       "\203\175\203\142\203\166\221\345\202\253\202\263", }, /* マップ大きさ */
{ "Minimap_Corner",      "\203\175\203\142\203\166\210\312\222\165", }, /* マップ位置 */
{ "Minimap_Opacity",     "\203\175\203\142\203\166\223\247\211\337\223\170", }, /* マップ透過度 */
{ "Minimap_Reqs_Map",    "\222\156\220\175\216\346\223\276\214\343\202\314\202\335", }, /* 地図取得後のみ */
{ "Low_HP_Glow",         "\221\314\227\315\214\170\215\220\225\134\216\246", }, /* 体力警告表示 */
{ "Next_Page",           "\216\237\202\314\203\171\201\133\203\127", }, /* 次のページ */
{ "Prev_Page",           "\221\117\202\314\203\171\201\133\203\127", }, /* 前のページ */
{ "Back",                "\226\337\202\351",                      }, /* 戻る */
{ "Windowed",            "\203\105\203\102\203\223\203\150\203\105", }, /* ウィンドウ */
{ "Fullscreen",          "\203\164\203\213\203\130\203\116\203\212\201\133\203\223", }, /* フルスクリーン */
{ "Borderless",          "\203\173\201\133\203\137\201\133\203\214\203\130", }, /* ボーダーレス */
{ "Auto",                "\216\251\223\256",                      }, /* 自動 */
{ "Classic",             "\203\116\203\211\203\126\203\142\203\116", }, /* クラシック */
{ "Modern",              "\203\202\203\137\203\223",              }, /* モダン */
{ "Bilinear",            "\203\157\203\103\203\212\203\152\203\101", }, /* バイリニア */
{ "Trilinear",           "\203\147\203\211\203\103\203\212\203\152\203\101", }, /* トライリニア */
{ "Dither",              "\203\146\203\102\203\125",              }, /* ディザ */
{ "Scanlines",           "\221\226\215\270\220\374",              }, /* 走査線 */
{ "Vignette",            "\216\374\225\323\214\270\214\365",      }, /* 周辺減光 */
{ "Color_Grade",         "\220\106\222\262\225\342\220\263",      }, /* 色調補正 */
{ "Film_Grain",          "\203\164\203\102\203\213\203\200\203\117\203\214\203\103\203\223", }, /* フィルムグレイン */
{ "Sharpen",             "\203\126\203\203\201\133\203\166",      }, /* シャープ */
{ "Cinematic",           "\203\126\203\154\203\175",              }, /* シネマ */
{ "Filmic",              "\203\164\203\102\203\213\203\200\222\262", }, /* フィルム調 */
{ "Reinhard",            "\203\211\203\103\203\223\203\156\203\213\203\147", }, /* ラインハルト */
{ "Square",              "\216\154\212\160",                      }, /* 四角 */
{ "Circle",              "\211\176",                              }, /* 円 */
{ "External",            "\212\117\225\224",                      }, /* 外部 */
{ "In_Game",             "\203\121\201\133\203\200\223\340",      }, /* ゲーム内 */
{ "Both",                "\227\274\225\373",                      }, /* 両方 */
/* Layout overrides, not new rows: these keys ALSO exist in
 * lang_jpn_menu.inc, and Pc_JpnMenuText checks this table first so the short
 * form wins in Japanese. Retail words each of these as a full sentence, sized
 * for a screen the port has since re-laid-out, and every label origin here is
 * x=121 at 12 px per glyph:
 *
 *   BGM_Volume  "BGMのボリュームを調節します"      15 glyphs -> 301, bar at 212
 *   SE_Volume   "SEのボリュームを調節します"       14 glyphs -> 289, bar at 212
 *   Vibration   "　アナログコントローラの振動設定：" 16 glyphs -> 313, value at 232
 *
 * so all three drew straight through what sits to their right. Chinese keeps
 * the disc strings: they are 6 to 8 glyphs, they fit, and they are real. */
{ "BGM_Volume",          "\202\141\202\146\202\154\211\271\227\312", }, /* ＢＧＭ音量 */
{ "SE_Volume",           "\202\162\202\144\211\271\227\312",      }, /* ＳＥ音量 */
{ "Vibration",           "\220\125\223\256\220\335\222\350\201\106", }, /* 振動設定： */
/* ---- Inventory ---------------------------------------------------------
 * The item command menu and the status line under it. Retail NTSC-J draws
 * these in ENGLISH -- the words are ASCII in the JP BODYPROG's rodata at
 * 0x800262E8 (commands) and 0x80027C78 (status), byte-identical to the USA
 * disc -- and the Chinese patch left them alone too, so there is nothing to
 * read back and both halves are written here. Japanese only, same gate as the
 * rest of this file.
 *
 * "On"/"Off" are NOT here: those keys already resolve to オン/オフ through the
 * disc table, which the options screen feeds. The decorated "==On==" forms the
 * status line uses are different keys and do need their own entries.
 *
 * Widths are checked against what they replace -- the command box is sized for
 * "Unequip" (42 px) and the status line starts at x=208 -- which is why Reload
 * is 装填 (24 px) rather than リロード (48 px). */
{ "Use",                 "\216\147\202\244",                            }, /* 使う */
{ "Equip",               "\221\225\224\365",                            }, /* 装備 */
{ "Unequip",             "\211\360\217\234",                            }, /* 解除 */
{ "Reload",              "\221\225\223\125",                            }, /* 装填 */
{ "Detail",              "\222\262\202\327\202\351",                    }, /* 調べる */
{ "Look",                "\214\251\202\351",                            }, /* 見る */
{ "Stock:",              "\216\143\202\350\201\106",                    }, /* 残り： */
{ "Fuel:",               "\224\122\227\277\201\106",                    }, /* 燃料： */
{ "==On==",              "\075\075\203\111\203\223\075\075",            }, /* ==オン== */
{ "==Off==",             "\075\075\203\111\203\164\075\075",            }, /* ==オフ== */
{ "==Use_OK==",          "\075\075\216\147\227\160\211\302\075\075",    }, /* ==使用可== */
{ "==Use_OK?==",         "\075\075\216\147\227\160\211\302\201\110\075\075", }, /* ==使用可？== */
{ "==Use_NG==",          "\075\075\216\147\227\160\225\163\211\302\075\075", }, /* ==使用不可== */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where each menu string the port draws lives in the NTSC-J overlays.
 *
 * Retail NTSC-J draws these from VIN/OPTION.BIN and VIN/SAVELOAD.BIN, in
 * Japanese; the port compiles the decomp's US branch and so drew the English
 * literals instead. Reading them back off the disc fixes that AND gives the
 * Chinese fan translation for free: its patch is a PPF, which can only replace
 * bytes in place, so its strings sit at exactly these offsets.
 *
 * `len` 0 means the string is NUL-terminated. A non-zero `len` is a
 * fixed-width field inside a longer blob -- the game slices several values out
 * of one string, and the shorter language pads with the SJIS ideographic space
 * (0x8140), so the field boundaries are identical in both. Offsets verified
 * against both discs; see pc_port/tools/ for the generator. */
/* file: 0 = VIN/OPTION.BIN, 1 = VIN/SAVELOAD.BIN */
{ "Next_fear", 1, 0x0004, 0 },
{ "Child's_room", 1, 0x0014, 0 },
{ "Nowhere", 1, 0x0020, 0 },
{ "Sewer", 1, 0x0028, 0 },
{ "Lighthouse", 1, 0x0030, 0 },
{ "Motel", 1, 0x0038, 0 },
{ "Bridge", 1, 0x0044, 0 },
{ "Boat", 1, 0x0054, 0 },
{ "Theme_park", 1, 0x005C, 0 },
{ "Antique_shop", 1, 0x0064, 0 },
{ "Pool_hall", 1, 0x006C, 0 },
{ "Jewelry_shop", 1, 0x007C, 0 },
{ "Director's", 1, 0x0084, 0 },
{ "Room_302", 1, 0x0090, 0 },
{ "Reception", 1, 0x00A0, 0 },
{ "Police", 1, 0x00AC, 0 },
{ "Garage", 1, 0x00B4, 0 },
{ "Church", 1, 0x00C0, 0 },
{ "Gordon", 1, 0x00C8, 0 },
{ "Doghouse", 1, 0x00D8, 0 },
{ "Infirmary", 1, 0x00E8, 0 },
{ "Store", 1, 0x00F8, 0 },
{ "Bus", 1, 0x0104, 0 },
{ "Cafe", 1, 0x010C, 0 },
{ "Anywhere", 1, 0x0114, 0 },
{ "On", 0, 0x0190, 4 },
{ "Off", 0, 0x0194, 4 },
{ "Stereo", 0, 0x0198, 8 },
{ "Monaural", 0, 0x01A0, 8 },
{ "Switch", 0, 0x0344, 8 },
{ "Press", 0, 0x034C, 8 },
{ "Reverse", 0, 0x0370, 8 },
{ "Normal", 0, 0x0378, 8 },
{ "Green", 0, 0x0380, 2 },
{ "Violet", 0, 0x0382, 2 },
{ "Black", 0, 0x0384, 2 },
{ "Self_View", 0, 0x0390, 8 },
{ "SLOT1", 1, 0x013C, 10 },
{ "SLOT2", 1, 0x0146, 10 },
{ "FILE", 1, 0x0154, 12 },
/* ---- Options + Extra Options row labels -------------------------------
 * Retail NTSC-J draws these in Japanese from the same OPTION.BIN the values
 * above come from; the port compiled the decomp's US branch and drew the
 * English literals, which is why those two screens read half-translated on a
 * JP disc. Offsets are the two pointer arrays at 0x01BC (Options, 9 entries)
 * and 0x039C (Extra Options, 8), verified against both the retail JP disc and
 * the Chinese-patched one -- so this gets Chinese for free, exactly as the
 * value strings already do. `len` trims the ideographic-space padding to the
 * longer of the two languages; Bullet_Adjust's field is followed by the x1..x6
 * value blob in the same string, so it stops at the colon. */
{ "Exit", 0, 0x0004, 28 },                            /* オプションモードを終了します */
{ "Brightness_Level", 0, 0x0030, 24 },                /* 画面の明るさを調節します */
{ "Controller_Config", 0, 0x005C, 20 },               /* 操作方法を変更します */
{ "Screen_Position", 0, 0x0088, 26 },                 /* 画面の表示位置を調節します */
{ "Vibration", 0, 0x00B4, 34 },                       /* アナログコントローラの振動設定： */
{ "Auto_Load", 0, 0x00E0, 28 },                       /* オートロードの設定： */
{ "Sound", 0, 0x010C, 20 },                           /* サウンドの設定： */
{ "BGM_Volume", 0, 0x0138, 30 },                      /* ＢＧＭのボリュームを調節します */
{ "SE_Volume", 0, 0x0164, 28 },                       /* ＳＥのボリュームを調節します */
{ "Weapon_Control", 0, 0x01E4, 18 },                  /* 武器の構えタイプ： */
{ "Blood_Color", 0, 0x0210, 14 },                     /* 血しぶきの色： */
{ "View_Control", 0, 0x023C, 18 },                    /* サーチビュー操作： */
{ "Retreat_Turn", 0, 0x0268, 14 },                    /* 後退時の旋回： */
{ "\x01W" "\x01" "a\x01l\x01k/R\x01\x01u\x01n_\x01\x01\x01\x01" "Co\x01n\x01t\x01ro\x01l", 0, 0x0294, 18 }, /* 歩行／走行の操作： */
{ "Auto_Aiming", 0, 0x02C0, 16 },                     /* 武器の自動照準： */
{ "View_Mode", 0, 0x02EC, 18 },                       /* サーチビュー効果： */
{ "Bullet_Adjust", 0, 0x0318, 12 },                   /* 弾薬量設定： */

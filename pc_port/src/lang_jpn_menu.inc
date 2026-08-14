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

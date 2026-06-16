/* map7_s03 ending-cutscene step tables (s_800ED7E0_ptr), reformatted from the
 * PSX overlay (MAP7_S03.BIN). The originals held 0x800CCxxx pointers into
 * overlay rodata, invalid on 64-bit; the zero-stub gave NULL for low cutscene
 * steps but read OFF THE END (garbage non-NULL ptr) for high steps -> the
 * ending crash. s_800ED7E0_ptr has no nested pointers, so each unique PSX
 * target is a named static and the tables point at them. Tables padded with
 * trailing NULL (consumer null-checks ptr) so an out-of-range step can't OOB.
 * HAND-GENERATED from the disc — do NOT regenerate with extract_map_data.py. */
static s_800ED7E0_ptr s_Cut_800CC320[1] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CC35C[2] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 551731, 0, -428441 }, 8192, 24576, 0, 16384, 2048, 4096, 0, 1, 0, 0, 40, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CC3D4[1] = {
    { { 592691, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 40, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CC410[3] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 592691, -40960, -428441 }, 8192, 40960, 0, 8192, 2048, 4096, 0, 0, 0, 0, 40, 0x3 },
    { { 592691, 0, -428441 }, 8192, 24576, 0, 16384, 2048, 4096, 0, 1, 0, 0, 40, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CC4C4[1] = {
    { { 572211, -40960, -407961 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 40, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CC5C8[1] = {
    { { 572211, -32768, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 40, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CC63C[2] = {
    { { 572211, -40960, -407961 }, 0, 32768, 0, 0, 2048, 6144, 0, 0, 0, 0, 409, 0x1 },
    { { 572211, -40960, -407961 }, 0, 12288, 4096, 6144, 2048, 6144, 0, 0, 0, 0, 1228, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CC6B4[5] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 572211, 0, -407961 }, 4096, 32768, 0, 16384, 2048, 4096, 0, 1, 0, 0, 122, 0x3 },
    { { 572211, -40960, -407961 }, 4096, 32768, 0, 8192, 2048, 4096, 0, 0, 0, 0, 122, 0x3 },
    { { 572211, -40960, -407961 }, 0, 32768, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x1 },
    { { 572211, -40960, -407961 }, 0, 12288, 6553, 8192, 2048, 6144, 0, 0, 0, 0, 1228, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CC7E0[5] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 410828, -40960, -571801 }, 8192, 40960, 0, 8192, 2048, 4096, 0, 0, -8192, 1, 122, 0x3 },
    { { 410828, 0, -571801 }, 8192, 40960, 0, 16384, 2048, 4096, 0, 1, -8192, 1, 122, 0x3 },
    { { 410828, -40960, -563609 }, 8192, 40960, 0, 8192, 2048, 4096, 0, 0, -8192, 1, 122, 0x1 },
    { { 410828, 0, -563609 }, 8192, 40960, 0, 16384, 2048, 4096, 0, 1, -8192, 1, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CC90C[4] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 572211, 0, -403865 }, 3276, 32768, 0, 16384, 2048, 4096, 0, 1, 0, 0, 122, 0x3 },
    { { 572211, -40960, -407961 }, 2048, 32768, 0, 8192, 2048, 4096, 0, 0, 0, 0, 122, 0x3 },
    { { 572211, -40960, -407961 }, 0, 12288, 6553, 7782, 2048, 6144, 0, 0, 0, 0, 1228, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CC9FC[4] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 410828, 0, -571801 }, 8192, 40960, 0, 16384, 2048, 4096, 0, 1, 0, 0, 40, 0x3 },
    { { 410828, -40960, -571801 }, 2048, 32768, 0, 4096, 2048, 8192, 0, 0, 0, 0, 8192, 0x3 },
    { { 410828, -40960, -571801 }, 0, 12288, 0, 6553, 2048, 8192, 0, 0, 0, 0, 1228, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CCAEC[1] = {
    { { 410828, -40960, -571801 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 409, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCB28[4] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 410828, 0, -571801 }, 8192, 40960, 0, 16384, 2048, 4096, 0, 1, 0, 0, 122, 0x3 },
    { { 410828, -40960, -571801 }, 8192, 40960, 0, 8192, 2048, 4096, 0, 0, 0, 0, 122, 0x3 },
    { { 410828, -40960, -571801 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 409, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCC18[1] = {
    { { 410828, -40960, -571801 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, -8192, 0, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCC54[1] = {
    { { 410828, 0, -571801 }, 8192, 40960, 8192, 16384, 2048, 4096, 0, 1, -8192, 0, 122, 0x2 }
};
static s_800ED7E0_ptr s_Cut_800CCD20[1] = {
    { { 572211, -40960, -407961 }, 0, 32768, 0, 0, 2048, 6144, 0, 0, 0, 0, 409, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCD5C[4] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 572211, 0, -407961 }, 4096, 32768, 0, 16384, 2048, 4096, 0, 1, 0, 0, 122, 0x3 },
    { { 572211, -40960, -407961 }, 4096, 32768, 0, 8192, 2048, 4096, 0, 0, 0, 0, 122, 0x3 },
    { { 572211, -40960, -407961 }, 0, 32768, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCE80[1] = {
    { { 572211, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCEBC[4] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 572211, 0, -428441 }, 8192, 40960, 0, 16384, 2048, 4096, 0, 1, 0, 0, 40, 0x3 },
    { { 572211, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x3 },
    { { 572211, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CCFAC[2] = {
    { { 592691, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x3 },
    { { 592691, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CD024[2] = {
    { { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0, 99, 0, 0, 0, 0x3 },
    { { 592691, -40960, -428441 }, 8192, 40960, 0, 0, 2048, 4096, 0, 0, 0, 0, 122, 0x0 }
};
static s_800ED7E0_ptr s_Cut_800CD09C[1] = {
    { { 572211, 0, -428441 }, 8192, 40960, 0, 16384, 2048, 4096, 0, 1, 0, 0, 40, 0x2 }
};

s_800ED7E0_ptr* D_800ED7E0[64] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC4C4, s_Cut_800CC410, s_Cut_800CC3D4, s_Cut_800CC3D4, s_Cut_800CC3D4, NULL, s_Cut_800CC35C, NULL, s_Cut_800CC320, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
s_800ED7E0_ptr* D_800ED8B0[64] = { NULL, NULL, NULL, NULL, NULL, NULL, s_Cut_800CC5C8, s_Cut_800CC5C8, s_Cut_800CC5C8, s_Cut_800CC5C8, s_Cut_800CC5C8, s_Cut_800CC5C8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
s_800ED7E0_ptr* D_800ED8EC[64] = { NULL, NULL, s_Cut_800CCC54, s_Cut_800CCC18, s_Cut_800CCC18, s_Cut_800CCC18, s_Cut_800CCC18, s_Cut_800CCB28, s_Cut_800CCAEC, s_Cut_800CCAEC, NULL, s_Cut_800CCAEC, NULL, NULL, NULL, NULL, s_Cut_800CC9FC, NULL, s_Cut_800CC90C, NULL, NULL, s_Cut_800CC7E0, NULL, NULL, s_Cut_800CC6B4, NULL, NULL, s_Cut_800CC63C, s_Cut_800CC63C, s_Cut_800CC63C, s_Cut_800CC63C, s_Cut_800CC63C, NULL, NULL, NULL, s_Cut_800CC320, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
s_800ED7E0_ptr* D_800ED984[64] = { NULL, NULL, s_Cut_800CCC54, s_Cut_800CCC18, NULL, s_Cut_800CCD5C, s_Cut_800CCD20, NULL, s_Cut_800CC320, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
s_800ED7E0_ptr* D_800ED9BC[64] = { NULL, s_Cut_800CD09C, s_Cut_800CD024, s_Cut_800CCFAC, s_Cut_800CCFAC, s_Cut_800CCFAC, s_Cut_800CCFAC, s_Cut_800CCEBC, s_Cut_800CCE80, s_Cut_800CCE80, s_Cut_800CCE80, s_Cut_800CCE80, s_Cut_800CCE80, s_Cut_800CC320, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* split_head_rodata.inc -- Split Head AI dispatch table extracted from
 * disc_extract/VIN/MAP1_S05.BIN.
 *
 * PSX g_Ai_SplitHead_ControlFuncs @ 0x800D5A68 (9 entries x 4 B). Verified
 * against sym.map1_s05.txt -- all 8 control functions resolve to existing
 * C symbols in split_head.c (Ai_SplitHead_Control_1..8). Entry 0 is NULL,
 * matching SplitHeadControl_None (handled in Ai_SplitHead_Init).
 *
 * Included at the bottom of split_head.c so each map DLL hosting a Split
 * Head (map1_s05, map1_s06) gets its own self-contained copy alongside
 * the per-DLL Ai_SplitHead_Control_N implementations. The exe import lib
 * has no visibility into DLL-resident functions, so the dispatch table
 * can only live in the same translation unit. */

void (*g_Ai_SplitHead_ControlFuncs[9])(s_SubCharacter*) = {
    /* [0] SplitHeadControl_None */ NULL,
    /* [1] */ Ai_SplitHead_Control_1,
    /* [2] */ Ai_SplitHead_Control_2,
    /* [3] */ Ai_SplitHead_Control_3,
    /* [4] */ Ai_SplitHead_Control_4,
    /* [5] */ Ai_SplitHead_Control_5,
    /* [6] */ Ai_SplitHead_Control_6,
    /* [7] */ Ai_SplitHead_Control_7,
    /* [8] */ Ai_SplitHead_Control_8,
};

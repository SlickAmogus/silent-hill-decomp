/* romper_rodata.inc -- Romper AI dispatch table extracted from
 * disc_extract/VIN/MAP2_S02.BIN.
 *
 * PSX g_Romper_ControlFuncs @ 0x800ECC14 (12 entries x 4 B). All 11
 * non-NULL targets resolve to existing C symbols in romper.c (verified
 * against sym.map2_s02.txt). Entry 0 is NULL, matching RomperControl_None.
 *
 * Included at the bottom of romper.c so each map DLL hosting a Romper
 * (map2_s02, map4_s02, map5_s01, map6_s00) gets its own copy alongside
 * the Ai_Romper_Control_* functions it dispatches to. The exe import
 * lib has no visibility into DLL-resident functions, so the dispatch
 * table can only live in the same translation unit. */

void (*g_Romper_ControlFuncs[12])(s_SubCharacter*) = {
    /* [0]  None        */ NULL,
    /* [1]              */ Romper_Control_1,
    /* [2]  WalkForward */ Romper_ControlWalkForward,
    /* [3]              */ Romper_Control_3,
    /* [4]              */ Romper_Control_4,
    /* [5]  Jump        */ Romper_ControlJump,
    /* [6]              */ Romper_Control_6,
    /* [7]              */ Romper_Control_7,
    /* [8]              */ Romper_Control_8,
    /* [9]              */ Romper_Control_9,
    /* [10]             */ Romper_Control_10,
    /* [11] GrabAttack  */ Romper_ControlGrabAttack,
};

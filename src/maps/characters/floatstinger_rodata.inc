/* floatstinger_rodata.inc -- Floatstinger AI dispatch table extracted from
 * disc_extract/VIN/MAP4_S05.BIN.
 *
 * PSX D_800D7A04 @ 0x800D7A04 (7 entries x 4 B, file offset 0xE48C with
 * load base 0x800C9578; the s16 vector data D_800D7A20 follows at +0x1C,
 * bounding the table). Raw words:
 *   0, 0, 0x800D1BF8, 0x800D2B90, 0x800D341C, 0x800D3424, 0x800D3564
 * All five non-NULL targets resolve to existing C symbols in
 * floatstinger.c (verified against sym.map4_s05.txt).
 *
 * The PSX dispatch passes no explicit argument; the handlers read the
 * floatstinger pointer from a0, which Floatstinger_ControlUpdate still
 * holds at the jalr. On PC the table is typed to take the pointer and
 * the call site passes it explicitly. func_800D341C is authored as a
 * no-arg no-op; the cast is harmless under the x64 calling convention.
 *
 * Included at the bottom of floatstinger.c so the map4_s05 DLL gets the
 * table alongside the handlers it dispatches to (same reasoning as
 * romper_rodata.inc: the exe import lib has no visibility into
 * DLL-resident functions). */

void (*D_800D7A04[7])(s_SubCharacter*) = {
    /* [0] */ NULL,
    /* [1] */ NULL,
    /* [2] */ func_800D1BF8,
    /* [3] */ func_800D2B90,
    /* [4] */ (void (*)(s_SubCharacter*))func_800D341C,
    /* [5] */ func_800D3424,
    /* [6] */ func_800D3564,
};

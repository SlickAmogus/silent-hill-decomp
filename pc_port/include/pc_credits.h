/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CREDITS_H
#define PC_CREDITS_H

/* PC-port block appended to the staff roll, and the text behind the ABOUT
 * console command. Gated by config `pc_port_credits` / console PCCREDITS. */

typedef enum
{
    PcCreditRow_Blank = 0, /* one empty scroll row */
    PcCreditRow_Header,    /* section title, centered (styled like "Executive Producer") */
    PcCreditRow_Name,      /* single centered line (styled like "Makoto Yano") */
    PcCreditRow_Pair       /* two columns, styled like the voice cast: role .... name */
} e_PcCreditRowKind;

typedef struct
{
    int         kind;
    const char* left;
    const char* right; /* PcCreditRow_Pair only */
} s_PcCreditRow;

/** Rebuild the roll's line list and set D_801E5C20 to match. Call before the
 * scroll math is derived from it (func_801E2E28 / func_801E386C). */
void PcCredits_Begin(void);

/** The line list the roll walks: vanilla, or vanilla + the PC-port block. */
char** PcCredits_List(void);

int                  PcCredits_RowCount(void);
const s_PcCreditRow* PcCredits_Row(int idx);

#endif

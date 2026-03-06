/*
 * include_asm.h - PC port override
 *
 * INCLUDE_ASM is disabled on PC (SKIP_ASM is defined).
 * Any remaining assembly stubs need C replacements.
 */
#ifndef _INCLUDE_ASM_H
#define _INCLUDE_ASM_H

/* Always skip ASM on PC */
#ifndef INCLUDE_ASM
#define INCLUDE_ASM(FOLDER, NAME)
#endif

#ifndef INCLUDE_RODATA
#define INCLUDE_RODATA(FOLDER, NAME)
#endif

#endif

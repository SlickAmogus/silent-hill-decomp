/*
 * inline_no_dmpsx.h - PC port override
 *
 * On PSX, this replaces GTE macros with raw coprocessor opcodes (.word).
 * On PC, PsyCross inline_c.h already provides C implementations of all
 * GTE operations, so this file is a no-op.
 */
#ifndef _INLINE_NO_DMPSX_H_
#define _INLINE_NO_DMPSX_H_

#include <inline_c.h>

#endif

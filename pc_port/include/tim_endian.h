/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tim_endian.h - byte-swap a TIM in place after it is read off the disc.
 *
 * Big-endian consoles only (360, PS3). On little-endian hosts the entry point
 * still exists but is a no-op, so call sites need no #ifdef of their own.
 */
#ifndef TIM_ENDIAN_H
#define TIM_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Converts a TIM buffer from its on-disc little-endian form to host order.
 * Idempotent: safe to call again on a buffer that has already been converted,
 * which matters because a TIM buffer can be re-parsed without being re-read. */
void Tim_SwapForBigEndian(void* addr);

#ifdef __cplusplus
}
#endif

#endif /* TIM_ENDIAN_H */

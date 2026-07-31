/*
 * stb_image_impl.c - the REAL stb_image PNG decoder, compiled once for the Xbox
 * port. This is the single STB_IMAGE_IMPLEMENTATION translation unit in the Xbox
 * build (hires_override.c and tex_pack.c, the PC port's other impl/callers, are
 * excluded in Makefile.nxdk). It replaces the NULL-returning stubs that used to
 * live in xbox_pcfeature_stubs.c, so ra_badge_xbox.c (RA unlock badges) and
 * pc_decals.c (optional loose decal PNG) get a working stbi_load_from_memory /
 * stbi_failure_reason.
 *
 * Build tuning for NXDK (clang + pdclib, 32-bit, no worker threads):
 *   STBI_ONLY_PNG        - RA badges + decals are PNG; drops the JPEG/BMP/GIF/etc
 *                          code paths (smaller, fewer libc deps).
 *   STBI_NO_STDIO        - memory-only decode; no fopen path (we feed HTTP bytes).
 *   STBI_NO_HDR / _LINEAR- no float HDR path -> no <math.h> (ldexp/pow) dependency.
 *   STBI_NO_SIMD         - no x86 SSE2 intrinsics; plain C only.
 *   STBI_NO_THREAD_LOCALS- the failure-reason string becomes a plain static (no
 *                          __thread TLS, which pdclib/nxdk does not provide). The
 *                          RA client is strictly single-threaded, so this is safe.
 *   STBI_ASSERT = no-op  - never abort() on a malformed image; the callers already
 *                          treat a NULL return as "skip" (badge/decal disabled).
 *
 * No STBI_PNG_ZLIB_DECODE hook is defined here, so stb uses its own built-in
 * inflate (self-contained; the PC port's miniz hook is a perf-only optimization
 * for its thousand-PNG texture packs, irrelevant to a one-off 64x64 badge).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)

#include "stb_image.h"

/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_ra_http.h - blocking HTTP used by the RetroAchievements client.
 *
 * Implemented in pc_ra_http.c, which is the only translation unit allowed to
 * include <windows.h> (it collides with the PSX headers). Call from the RA
 * worker thread only — it blocks.
 */
#ifndef PC_RA_HTTP_H
#define PC_RA_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GET when `post` is NULL/empty, otherwise form-encoded POST. On success
 * *out_body is a malloc'd NUL-terminated buffer the caller frees. Returns the
 * HTTP status code, or 0 if the request could not be made. */
/* Resolve one of the exe's own exported globals by name (NULL if absent). */
void* Pc_RaSymbolLookup(const char* name);

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* PC_RA_HTTP_H */

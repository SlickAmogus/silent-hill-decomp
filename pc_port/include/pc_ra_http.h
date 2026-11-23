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

/* GET straight into the file at `path`, for a body too large to hold in memory.
 * *got (optional) follows the bytes written. Returns the HTTP status; anything
 * but 200 means the file was removed. Blocks: call from a worker thread. */
int Pc_RaHttpDownloadFile(const char* url, const char* path, volatile long long* got);

#ifdef __cplusplus
}
#endif

#endif /* PC_RA_HTTP_H */

/*
 * net_xbox.h - Xbox networking + plaintext-HTTP transport for RetroAchievements.
 *
 * Shared contract between the RA-net transport (net_xbox.c) and the RA-client
 * layer that calls it. Keep this header dependency-free (plain C prototypes) so
 * BOTH sides can include it: net_xbox.c pairs it with the lwIP stack, while the
 * RA-client side includes it alongside the game/PSX headers -- and lwIP's
 * headers (htons/RECT/byte collisions) must NEVER leak in through here.
 */
#ifndef NET_XBOX_H
#define NET_XBOX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the network stack up: nxNetInit(NULL) (DHCP, blocks up to ~10s), logs
 * the leased IP. Returns 0 on success, <0 on failure. Idempotent: a no-op that
 * returns 0 once the stack is already up. Because nxNetInit spins up the lwIP
 * tcpip thread it must run exactly once, from a single (boot) thread -- call
 * this during init, before any worker thread issues a request. */
int Net_XboxBringUp(void);

/* Non-zero once the stack is up (a successful Net_XboxBringUp has run). */
int Net_XboxIsUp(void);

/* Blocking plain-HTTP/1.0 request. url = "http://host[:port]/path".
 *   post == NULL/""  -> GET
 *   post != ""       -> POST with body (application/x-www-form-urlencoded)
 * On success allocates *out_body (the response BODY only, NUL-terminated; the
 * caller frees it with free()) and sets *out_len to its length. Returns the
 * HTTP status code, or 0 on any transport failure (stack down, bad URL, DNS,
 * connect, send, or no response). All reads are time-bounded. */
int Net_XboxHttpRequest(const char* url, const char* post, char** out_body, int* out_len);

#ifdef __cplusplus
}
#endif

#endif /* NET_XBOX_H */

/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_http.c - blocking HTTP for the conscious worker thread.
 *
 * Isolated for the same reason as pc_ra_http.c: <windows.h> and the PSX
 * headers cannot share a translation unit, so this file includes the platform
 * stack and nothing from the game.
 */

#include <stdlib.h>
#include <string.h>

#include "pc_conscious.h"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>

static wchar_t* Http_Wide(const char* s)
{
    int      n;
    wchar_t* w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

int PcCon_Http(const char* method, const char* url, const char* headers,
               const void* body, unsigned long bodyLen,
               char** outBody, unsigned long* outLen)
{
    HINTERNET      session = NULL, connect = NULL, request = NULL;
    URL_COMPONENTS uc;
    wchar_t        host[256], path[2048];
    wchar_t*       wUrl  = NULL;
    wchar_t*       wHdr  = NULL;
    wchar_t*       wMeth = NULL;
    DWORD          status = 0, statusLen = (DWORD)sizeof(DWORD);
    char*          buf = NULL;
    unsigned long  len = 0;

    *outBody = NULL;
    *outLen  = 0;

    wUrl  = Http_Wide(url);
    wMeth = Http_Wide(method ? method : "GET");
    if (!wUrl || !wMeth)
        goto out;

    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = (DWORD)(sizeof(path) / sizeof(path[0]));
    /* lpszExtraInfo left NULL: the query string then stays on the path, which
     * is what WinHttpOpenRequest wants. */
    if (!WinHttpCrackUrl(wUrl, 0, 0, &uc))
        goto out;

    session = WinHttpOpen(L"SilentHillPC-Conscious/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        goto out;
    /* A local model on a cold start can take a while to answer the first
     * request; the cloud ones answer in a few seconds. */
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 90000);

    connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect)
        goto out;
    request = WinHttpOpenRequest(connect, wMeth, path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
        goto out;

    if (headers && headers[0])
        wHdr = Http_Wide(headers);

    if (!WinHttpSendRequest(request, wHdr ? wHdr : WINHTTP_NO_ADDITIONAL_HEADERS,
                            wHdr ? (DWORD)-1L : 0,
                            body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
                            (DWORD)bodyLen, (DWORD)bodyLen, 0) ||
        !WinHttpReceiveResponse(request, NULL))
        goto out;

    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
    for (;;)
    {
        DWORD avail = 0, got = 0;
        char* grown;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
            break;
        grown = (char*)realloc(buf, len + avail + 1);
        if (!grown)
            break;
        buf = grown;
        if (!WinHttpReadData(request, buf + len, avail, &got) || got == 0)
            break;
        len += got;
    }
    if (!buf)
        buf = (char*)calloc(1, 1);
    if (buf)
        buf[len] = '\0';

out:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    free(wUrl);
    free(wHdr);
    free(wMeth);
    *outBody = buf;
    *outLen  = len;
    return (int)status;
}

#else

int PcCon_Http(const char* method, const char* url, const char* headers,
               const void* body, unsigned long bodyLen,
               char** outBody, unsigned long* outLen)
{
    (void)method; (void)url; (void)headers; (void)body; (void)bodyLen;
    *outBody = NULL;
    *outLen  = 0;
    return 0;
}

#endif

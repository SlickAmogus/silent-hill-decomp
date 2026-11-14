/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_ra_http.c - blocking HTTP for the RetroAchievements client.
 *
 * Deliberately isolated from the rest of the port: <windows.h> and the PSX
 * headers cannot coexist (RECT16 vs tagRECT, `byte`, EnterCriticalSection), so
 * this file includes the platform stack and NOTHING from the game. The caller
 * (pc_retroachievements.c) owns threading and never includes windows.h.
 *
 * WinHTTP on Windows (ships with the OS), libcurl elsewhere. Without either,
 * requests fail cleanly and RA stays dormant.
 */
/* glibc only declares RTLD_DEFAULT under _GNU_SOURCE, and the macro has to beat
 * the first system header to it (they all pull in features.h). macOS declares it
 * unconditionally, which is why only the Linux build broke. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_ra_http.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* Resolve one of the exe's own exported globals by name. The port links with
 * -Wl,--export-all-symbols, so every non-static global (2500+) is in the export
 * table -- which lets the achievement layer rebuild the PSX address map from the
 * decomp symbol files instead of hand-mapping globals. */
void* Pc_RaSymbolLookup(const char* name)
{
#if defined(_WIN32)
    static HMODULE self;
    if (!self) self = GetModuleHandleA(NULL);
    return self ? (void*)GetProcAddress(self, name) : NULL;
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

#if defined(_WIN32)

#include <winhttp.h>

/* The body goes to `sink` when one is given, else to *out_body. */
static int Ra_Transfer(const char* url, const char* post, char** out_body, size_t* out_len, FILE* sink,
                       volatile long long* got)
{
    HINTERNET      session = NULL, connect = NULL, request = NULL;
    URL_COMPONENTS uc;
    wchar_t        wUrl[2048], host[256], path[1536];
    const int      isPost = (post && post[0]);
    DWORD          status = 0, statusLen = (DWORD)sizeof(DWORD);
    char*          body = NULL;
    size_t         bodyLen = 0;

    *out_body = NULL;
    *out_len  = 0;

    if (!url || !url[0])
        return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl,
                            (int)(sizeof(wUrl) / sizeof(wUrl[0]))) == 0)
        return 0;

    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!WinHttpCrackUrl(wUrl, 0, 0, &uc))
        return 0;

    session = WinHttpOpen(L"SilentHillPC/1.0",
                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return 0;

    connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (connect)
    {
        const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connect, isPost ? L"POST" : L"GET", path, NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }

    if (request)
    {
        const wchar_t* ctype = L"Content-Type: application/x-www-form-urlencoded\r\n";
        const DWORD    postLen = isPost ? (DWORD)strlen(post) : 0;
        BOOL ok = WinHttpSendRequest(request,
                                     isPost ? ctype : WINHTTP_NO_ADDITIONAL_HEADERS,
                                     isPost ? (DWORD)-1L : 0,
                                     isPost ? (LPVOID)post : WINHTTP_NO_REQUEST_DATA,
                                     postLen, postLen, 0);
        if (ok)
            ok = WinHttpReceiveResponse(request, NULL);

        if (ok)
        {
            WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                                WINHTTP_NO_HEADER_INDEX);
            for (;;)
            {
                DWORD avail = 0, nread = 0;
                char* grown;
                if (!WinHttpQueryDataAvailable(request, &avail))
                {
                    /* A transfer cut short must not pass for a complete file. */
                    if (sink)
                        status = 0;
                    break;
                }
                if (avail == 0)
                    break;
                if (sink)
                {
                    char  chunk[16384];
                    DWORD want = avail < (DWORD)sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
                    if (!WinHttpReadData(request, chunk, want, &nread) || nread == 0 ||
                        fwrite(chunk, 1, nread, sink) != nread)
                    {
                        status = 0;
                        break;
                    }
                    bodyLen += nread;
                    if (got)
                        *got = (long long)bodyLen;
                    continue;
                }
                grown = (char*)realloc(body, bodyLen + avail + 1);
                if (!grown)
                    break;
                body = grown;
                if (!WinHttpReadData(request, body + bodyLen, avail, &nread) || nread == 0)
                    break;
                bodyLen += nread;
            }
            if (body)
                body[bodyLen] = '\0';
        }
        WinHttpCloseHandle(request);
    }

    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    *out_body = body;
    *out_len  = bodyLen;
    return (int)status;
}

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
{
    return Ra_Transfer(url, post, out_body, out_len, NULL, NULL);
}

int Pc_RaHttpDownloadFile(const char* url, const char* path, volatile long long* got)
{
    char*  body = NULL;
    size_t len  = 0;
    FILE*  f    = fopen(path, "wb");
    int    status;

    if (f == NULL)
        return 0;
    status = Ra_Transfer(url, NULL, &body, &len, f, got);
    if (fclose(f) != 0)
        status = 0;
    free(body);
    if (status != 200)
        remove(path);
    return status;
}

#elif defined(SH_RA_HAVE_CURL)

#include <curl/curl.h>

typedef struct { char* data; size_t len; } s_RaBuf;

static size_t Pc_RaCurlWrite(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    s_RaBuf*     buf   = (s_RaBuf*)userdata;
    const size_t total = size * nmemb;
    char*        grown = (char*)realloc(buf->data, buf->len + total + 1);
    if (!grown)
        return 0;
    buf->data = grown;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
{
    CURL*   curl;
    s_RaBuf buf = { NULL, 0 };
    long    status = 0;

    *out_body = NULL;
    *out_len  = 0;

    curl = curl_easy_init();
    if (!curl)
        return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Pc_RaCurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SilentHillPC/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    if (post && post[0])
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);

    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_easy_cleanup(curl);
    *out_body = buf.data;
    *out_len  = buf.len;
    return (int)status;
}

typedef struct { FILE* f; volatile long long* got; long long n; } s_RaFileSink;

static size_t Pc_RaCurlWriteFile(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    s_RaFileSink* sink  = (s_RaFileSink*)userdata;
    const size_t  total = size * nmemb;
    if (fwrite(ptr, 1, total, sink->f) != total)
        return 0;
    sink->n += (long long)total;
    if (sink->got)
        *sink->got = sink->n;
    return total;
}

int Pc_RaHttpDownloadFile(const char* url, const char* path, volatile long long* got)
{
    CURL*        curl;
    s_RaFileSink sink;
    long         status = 0;

    sink.f   = fopen(path, "wb");
    sink.got = got;
    sink.n   = 0;
    if (sink.f == NULL)
        return 0;

    curl = curl_easy_init();
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Pc_RaCurlWriteFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SilentHillPC/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        /* A mod can take far longer than any fixed timeout allows: give up on
         * a stalled transfer instead. */
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        if (curl_easy_perform(curl) == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
    }
    if (fclose(sink.f) != 0)
        status = 0;
    if (status != 200)
        remove(path);
    return (int)status;
}

#else

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
{
    (void)url; (void)post;
    *out_body = NULL;
    *out_len  = 0;
    return 0;
}

int Pc_RaHttpDownloadFile(const char* url, const char* path, volatile long long* got)
{
    (void)url; (void)path; (void)got;
    return 0;
}

#endif

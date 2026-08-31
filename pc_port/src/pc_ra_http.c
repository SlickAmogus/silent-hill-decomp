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

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
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
                DWORD avail = 0, got = 0;
                char* grown;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
                    break;
                grown = (char*)realloc(body, bodyLen + avail + 1);
                if (!grown)
                    break;
                body = grown;
                if (!WinHttpReadData(request, body + bodyLen, avail, &got) || got == 0)
                    break;
                bodyLen += got;
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

#elif defined(__ANDROID__)

/* The NDK ships neither WinHTTP nor libcurl, which is the only reason
 * RetroAchievements was ever desktop-only -- rcheevos is libc-only. Java's
 * HttpURLConnection needs no third-party library and brings the platform trust
 * store with it, so retroachievements.org TLS works with nothing bundled.
 *
 * The class reference is cached in JNI_OnLoad and NOT looked up here. FindClass
 * on a thread the JVM attached for native code resolves against the SYSTEM
 * class loader, which cannot see an app's own classes -- and this runs on the
 * RA worker thread, exactly such a thread. JNI_OnLoad runs while the app's
 * loader is current, so that is where the lookup has to happen. */
#include <jni.h>
#include <SDL.h>

static jclass    s_raHttpClass;
static jmethodID s_raHttpRequest;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env = NULL;
    jclass  local;

    (void)reserved;

    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_VERSION_1_6;

    local = (*env)->FindClass(env, "com/silenthill/port/RaHttp");
    if (local != NULL)
    {
        s_raHttpClass   = (jclass)(*env)->NewGlobalRef(env, local);
        s_raHttpRequest = (*env)->GetStaticMethodID(
            env, s_raHttpClass, "request",
            "(Ljava/lang/String;Ljava/lang/String;)[B");
        (*env)->DeleteLocalRef(env, local);
    }
    else
    {
        (*env)->ExceptionClear(env);
    }

    return JNI_VERSION_1_6;
}

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
{
    JNIEnv*     env;
    jstring     jUrl, jPost;
    jbyteArray  jResult;
    jsize       n;
    jbyte*      raw;
    int         status = 0;

    *out_body = NULL;
    *out_len  = 0;

    if (s_raHttpClass == NULL || s_raHttpRequest == NULL)
        return 0;

    /* Attaches this thread if it is not already, which the RA worker is not. */
    env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (env == NULL)
        return 0;

    jUrl  = (*env)->NewStringUTF(env, url);
    jPost = (post != NULL && post[0]) ? (*env)->NewStringUTF(env, post) : NULL;

    jResult = (jbyteArray)(*env)->CallStaticObjectMethod(env, s_raHttpClass,
                                                      s_raHttpRequest, jUrl, jPost);

    if ((*env)->ExceptionCheck(env))
    {
        (*env)->ExceptionClear(env);
        jResult = NULL;
    }

    if (jUrl  != NULL) (*env)->DeleteLocalRef(env, jUrl);
    if (jPost != NULL) (*env)->DeleteLocalRef(env, jPost);

    if (jResult == NULL)
        return 0;

    /* Four bytes of status, big-endian, then the body verbatim. Bytes rather
     * than text the whole way: badge art is PNG, and a string conversion turns
     * every byte that is not valid UTF-8 into U+FFFD -- the body still arrives
     * non-empty, so the fetch looks fine and the decoder just refuses it. */
    n = (*env)->GetArrayLength(env, jResult);

    if (n >= 4)
    {
        raw = (*env)->GetByteArrayElements(env, jResult, NULL);

        if (raw != NULL)
        {
            size_t len = (size_t)(n - 4);
            char*  buf = (char*)malloc(len + 1);

            status = ((int)(unsigned char)raw[0] << 24) |
                     ((int)(unsigned char)raw[1] << 16) |
                     ((int)(unsigned char)raw[2] << 8)  |
                      (int)(unsigned char)raw[3];

            /* NUL-terminated for the JSON callers, which read it as a string;
             * out_len carries the true length for the binary ones. */
            if (buf != NULL)
            {
                if (len)
                    memcpy(buf, raw + 4, len);
                buf[len]  = ' ';
                *out_body = buf;
                *out_len  = len;
            }

            (*env)->ReleaseByteArrayElements(env, jResult, raw, JNI_ABORT);
        }
    }

    (*env)->DeleteLocalRef(env, jResult);
    return status;
}

/* The options row calls this; it returns at once and the dialog runs on the UI
 * thread, because SDL drives the game loop on the calling thread. */
void Android_ShowRetroAchievementsLogin(void)
{
    JNIEnv*   env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jclass    cls;
    jmethodID mid;

    if (env == NULL)
        return;

    cls = (*env)->FindClass(env, "com/silenthill/port/SilentHillActivity");
    if (cls == NULL)
    {
        (*env)->ExceptionClear(env);
        return;
    }

    mid = (*env)->GetStaticMethodID(env, cls, "showRetroAchievementsLogin", "()V");
    if (mid != NULL)
        (*env)->CallStaticVoidMethod(env, cls, mid);
    else
        (*env)->ExceptionClear(env);

    (*env)->DeleteLocalRef(env, cls);
}

/* Called back from the dialog's Sign in button, on the UI thread. */
JNIEXPORT void JNICALL
Java_com_silenthill_port_SilentHillActivity_nativeRaLogin(JNIEnv* env, jclass cls,
                                                          jstring jUser, jstring jPass)
{
    extern int Pc_Ra_BeginPasswordLogin(const char* username, const char* password);

    const char* user = (jUser != NULL) ? (*env)->GetStringUTFChars(env, jUser, NULL) : NULL;
    const char* pass = (jPass != NULL) ? (*env)->GetStringUTFChars(env, jPass, NULL) : NULL;

    (void)cls;

    if (user != NULL && pass != NULL)
        Pc_Ra_BeginPasswordLogin(user, pass);

    if (user != NULL) (*env)->ReleaseStringUTFChars(env, jUser, user);
    if (pass != NULL) (*env)->ReleaseStringUTFChars(env, jPass, pass);
}

#else

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
{
    (void)url; (void)post;
    *out_body = NULL;
    *out_len  = 0;
    return 0;
}

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Discord IPC transport (see pc_discord_ipc.h). Isolated TU: the Windows path
 * pulls in <windows.h>, whose `byte` typedef clashes with the decomp's, so no
 * game/decomp headers may be included here (same split as pc_crash.c).
 *
 * Framing is handled by the caller (pc_discord.c); this layer only opens the
 * endpoint and moves raw bytes. Reads are non-blocking; writes block (frames are
 * tiny and Discord drains them immediately). */

#include "pc_discord_ipc.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

long long ShDiscordIpc_NowUnix(void)
{
    return (long long)time(NULL);
}

#if defined(SH_UWP)

/* An app container may not open another process's named pipe, so there is no
 * Discord client to reach. Never connecting leaves the rest of presence idle. */
int ShDiscordIpc_Pid(void) { return 0; }
int ShDiscordIpc_Connect(void) { return 0; }
int ShDiscordIpc_IsOpen(void) { return 0; }
int ShDiscordIpc_Write(const void* data, unsigned int len) { (void)data; (void)len; return 0; }
int ShDiscordIpc_Read(void* buf, unsigned int cap) { (void)buf; (void)cap; return 0; }
void ShDiscordIpc_Close(void) {}

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int ShDiscordIpc_Pid(void)
{
    return (int)GetCurrentProcessId();
}

static HANDLE s_pipe = INVALID_HANDLE_VALUE;

int ShDiscordIpc_Connect(void)
{
    int i;
    if (s_pipe != INVALID_HANDLE_VALUE)
        return 1;
    for (i = 0; i < 10; i++)
    {
        char   path[64];
        HANDLE h;
        snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            s_pipe = h;
            return 1;
        }
    }
    return 0;
}

int ShDiscordIpc_IsOpen(void)
{
    return s_pipe != INVALID_HANDLE_VALUE;
}

int ShDiscordIpc_Write(const void* data, unsigned int len)
{
    DWORD wrote = 0;
    if (s_pipe == INVALID_HANDLE_VALUE)
        return 0;
    if (len == 0)
        return 1;
    if (!WriteFile(s_pipe, data, (DWORD)len, &wrote, NULL) || wrote != len)
    {
        ShDiscordIpc_Close();
        return 0;
    }
    return 1;
}

int ShDiscordIpc_Read(void* buf, unsigned int cap)
{
    DWORD avail = 0, got = 0;
    if (s_pipe == INVALID_HANDLE_VALUE)
        return -1;
    /* Peek so a frameless tick never blocks the render thread. */
    if (!PeekNamedPipe(s_pipe, NULL, 0, NULL, &avail, NULL))
    {
        ShDiscordIpc_Close();
        return -1;
    }
    if (avail == 0)
        return 0;
    if (avail > cap)
        avail = cap;
    if (!ReadFile(s_pipe, buf, avail, &got, NULL))
    {
        ShDiscordIpc_Close();
        return -1;
    }
    return (int)got;
}

void ShDiscordIpc_Close(void)
{
    if (s_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }
}

#else /* POSIX */

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#else
#include <sys/syscall.h>
#endif

/* Never call close() here. PsyCross's libapi.c defines the PSX memory-card
 * open/close/read/write under those exact names, hidden so they cannot
 * interpose other libraries -- but every close() linked into the SAME image
 * (the Android libmain.so, the iOS app, the Linux exe) still binds to it, and
 * it returns -1 for any handle outside its 16-slot table without touching the
 * real descriptor. Each connect attempt below opens one socket per candidate
 * path (30+), so every ten seconds without Discord leaked 30 descriptors and
 * an Android 8 process hit its 1024 limit at ~318 s: new window buffers could
 * not be allocated, eglSwapBuffers failed with EGL_BAD_SURFACE and the screen
 * froze on the last frame while the game ran on. dlsym cannot see hidden
 * symbols, and the raw syscall bypasses symbol binding entirely. */
static int ipc_close(int fd)
{
#if defined(__APPLE__)
    static int (*s_realClose)(int) = NULL;
    if (s_realClose == NULL)
        s_realClose = (int (*)(int))dlsym(RTLD_DEFAULT, "close");
    return s_realClose != NULL ? s_realClose(fd) : -1;
#else
    return (int)syscall(SYS_close, fd);
#endif
}

int ShDiscordIpc_Pid(void)
{
    return (int)getpid();
}

static int s_sock = -1;

static int ipc_try(const char* dir, int idx)
{
    struct sockaddr_un addr;
    int    fd;
    int    need;
    if (dir == NULL || dir[0] == '\0')
        return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    need = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/discord-ipc-%d", dir, idx);
    if (need < 0 || (size_t)need >= sizeof(addr.sun_path))
        return 0;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        ipc_close(fd);
        return 0;
    }
    s_sock = fd;
    return 1;
}

int ShDiscordIpc_Connect(void)
{
    /* Discord's socket lives under the runtime dir; Flatpak/Snap nest it. */
    const char* bases[] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"),
                            getenv("TMP"), getenv("TEMP"), "/tmp" };
    const char* subs[]  = { "", "/app/com.discordapp.Discord", "/snap.discord" };
    size_t b, s;
    int    i;
    if (s_sock >= 0)
        return 1;
    for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++)
    {
        if (bases[b] == NULL || bases[b][0] == '\0')
            continue;
        for (s = 0; s < sizeof(subs) / sizeof(subs[0]); s++)
        {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s%s", bases[b], subs[s]);
            for (i = 0; i < 10; i++)
                if (ipc_try(dir, i))
                    return 1;
        }
    }
    return 0;
}

int ShDiscordIpc_IsOpen(void)
{
    return s_sock >= 0;
}

int ShDiscordIpc_Write(const void* data, unsigned int len)
{
    const char*  p   = (const char*)data;
    unsigned int off = 0;
    if (s_sock < 0)
        return 0;
    while (off < len)
    {
        ssize_t n = send(s_sock, p + off, len - off, 0);
        if (n > 0)
        {
            off += (unsigned int)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        ShDiscordIpc_Close();
        return 0;
    }
    return 1;
}

int ShDiscordIpc_Read(void* buf, unsigned int cap)
{
    ssize_t n;
    if (s_sock < 0)
        return -1;
    n = recv(s_sock, buf, cap, MSG_DONTWAIT);
    if (n > 0)
        return (int)n;
    if (n == 0)
    {
        ShDiscordIpc_Close(); /* peer closed */
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    ShDiscordIpc_Close();
    return -1;
}

void ShDiscordIpc_Close(void)
{
    if (s_sock >= 0)
    {
        ipc_close(s_sock);
        s_sock = -1;
    }
}

#endif

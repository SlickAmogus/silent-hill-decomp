#ifndef PC_DISCORD_IPC_H
#define PC_DISCORD_IPC_H

/* Discord IPC transport: a Windows named pipe (\\.\pipe\discord-ipc-N) or a
 * POSIX AF_UNIX socket ($XDG_RUNTIME_DIR/discord-ipc-N). Isolated in its own TU
 * (pc_discord_ipc.c) because the Windows path needs <windows.h>, whose `byte`
 * typedef clashes with the decomp's — the same reason pc_crash.c is split out.
 * No game/decomp headers may appear here or in that TU. */

int  ShDiscordIpc_Connect(void);                          /* 1 = a discord-ipc endpoint opened */
int  ShDiscordIpc_IsOpen(void);
int  ShDiscordIpc_Write(const void* data, unsigned int len); /* 1 = wrote all; 0 = failed (auto-closes) */
int  ShDiscordIpc_Read(void* buf, unsigned int cap);         /* >=0 bytes read (non-blocking); <0 = closed */
void ShDiscordIpc_Close(void);

/* time()/getpid() live here too: they pull in <time.h>/<unistd.h>, and this TU
 * is already isolated from the decomp headers whose typedefs those can clash
 * with (the game-facing pc_discord.c stays free of libc system headers). */
long long ShDiscordIpc_NowUnix(void); /* seconds since the Unix epoch */
int       ShDiscordIpc_Pid(void);     /* current process id */

#endif /* PC_DISCORD_IPC_H */

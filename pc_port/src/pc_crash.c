/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Crash telemetry. The SH_DBG log is fully buffered (64 KB), so on an
 * access violation the last lines — the ones that matter — die with the
 * process. This SEH filter writes the faulting address as module+offset
 * (mappable straight to a function with nm/objdump on the exe or map DLL)
 * PLUS a full call-stack backtrace (same module+offset form), then flushes
 * the log and lets the normal crash path continue so debuggers/WER still see
 * the exception.
 *
 * The backtrace walks x64 frames from the exception context itself using
 * RtlLookupFunctionEntry + RtlVirtualUnwind (no dbghelp/symbols needed), so
 * the log alone names the calling chain — resolve any SilentHillPC.exe /
 * mapX_sYY.dll frame with:  addr2line -f -e <module> 0x<offset>
 *
 * Isolated in its own TU: windows.h defines `byte`, which conflicts with
 * the decomp's typedef in include/decomp/types.h. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern FILE* g_ShDebugLog;

#ifdef _WIN32
#include <windows.h>

static void Sh_LogFrame(int idx, DWORD64 pc)
{
    HMODULE mod = NULL;
    char    modName[MAX_PATH] = "?";
    const char* base = modName;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char*)(uintptr_t)pc, &mod) && mod) {
        const char* slash;
        GetModuleFileNameA(mod, modName, sizeof(modName));
        slash = strrchr(modName, '\\');
        if (slash) base = slash + 1;
    }

    fprintf(g_ShDebugLog, "[CRASH]   #%02d %p  %s+0x%llX\n",
            idx, (void*)(uintptr_t)pc, base,
            (unsigned long long)(pc - (DWORD64)(uintptr_t)mod));
}

static void Sh_CrashBacktrace(EXCEPTION_POINTERS* ep)
{
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT ctx = *ep->ContextRecord;
    int     i;

    fprintf(g_ShDebugLog, "[CRASH] backtrace (resolve with: addr2line -f -e <module> 0x<offset>):\n");

    for (i = 0; i < 48; i++) {
        DWORD64           pc = ctx.Rip;
        DWORD64           imageBase = 0;
        PRUNTIME_FUNCTION rf;

        if (pc == 0)
            break;

        Sh_LogFrame(i, pc);

        rf = RtlLookupFunctionEntry(pc, &imageBase, NULL);
        if (rf) {
            PVOID   handlerData = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, rf,
                             &ctx, &handlerData, &establisher, NULL);
        } else {
            /* Leaf function (no unwind data): return address is at [RSP]. */
            if (ctx.Rsp == 0)
                break;
            ctx.Rip  = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        }

        if (ctx.Rip == 0)
            break;
    }
#endif
}

static LONG WINAPI Sh_CrashFilter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    HMODULE mod = NULL;
    char    modName[MAX_PATH] = "?";

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char*)rec->ExceptionAddress, &mod) && mod) {
        GetModuleFileNameA(mod, modName, sizeof(modName));
    }

    if (g_ShDebugLog) {
        fprintf(g_ShDebugLog, "[CRASH] code=0x%08lX at %p (%s+0x%llX)\n",
                (unsigned long)rec->ExceptionCode, rec->ExceptionAddress,
                modName, (unsigned long long)((char*)rec->ExceptionAddress - (char*)mod));
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            fprintf(g_ShDebugLog, "[CRASH] %s address %p\n",
                    rec->ExceptionInformation[0] == 1 ? "WRITING" :
                    rec->ExceptionInformation[0] == 8 ? "EXECUTING" : "READING",
                    (void*)rec->ExceptionInformation[1]);
        }
        Sh_CrashBacktrace(ep);
        fflush(g_ShDebugLog);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void Sh_InstallCrashFilter(void)
{
    SetUnhandledExceptionFilter(Sh_CrashFilter);
}

#else /* !_WIN32 */

/* POSIX crash handler. This existed as a no-op, and on Android that made
 * every crash report undiagnosable: the log is fully buffered (64 KB) and the
 * only thing emptying it is a once-per-second periodic flush, so a crash
 * discards up to a second of the log -- which is exactly the second that
 * explains it. A user's sewer crash arrived as a log whose last line was an
 * ordinary room transition, with the abort message from stderr (unbuffered,
 * so it survived) and nothing in between.
 *
 * Two jobs: flush the log so the tail reaches disk, and name the frames.
 * Bionic has no backtrace()/backtrace_symbols() -- those are glibc extensions
 * the NDK never shipped -- but it does have the C++ ABI unwinder and dladdr,
 * which together give the same answer.
 *
 * The handler re-raises with the default disposition afterwards, so Android
 * still writes its own tombstone and a debugger still sees the signal. */
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <unwind.h>

typedef struct
{
    int count;
} Sh_UnwindState;

static _Unwind_Reason_Code Sh_UnwindFrame(struct _Unwind_Context* ctx, void* arg)
{
    Sh_UnwindState* st = (Sh_UnwindState*)arg;
    uintptr_t       pc = (uintptr_t)_Unwind_GetIP(ctx);
    Dl_info         info;

    if (pc == 0 || st->count >= 40)
    {
        return _URC_END_OF_STACK;
    }

    /* Module + offset, not the raw address: the load base is randomised, so
     * an absolute pointer means nothing in a report. The offset resolves with
     *   addr2line -f -e libmain.so 0x<offset>
     * against the matching build's unstripped .so. */
    if (dladdr((void*)pc, &info) && info.dli_fname != NULL)
    {
        const char* slash = strrchr(info.dli_fname, '/');
        const char* base  = slash ? slash + 1 : info.dli_fname;
        uintptr_t   off   = pc - (uintptr_t)info.dli_fbase;

        if (info.dli_sname != NULL)
        {
            fprintf(g_ShDebugLog, "[CRASH]   #%02d %s+0x%lx  (%s)\n",
                    st->count, base, (unsigned long)off, info.dli_sname);
        }
        else
        {
            fprintf(g_ShDebugLog, "[CRASH]   #%02d %s+0x%lx\n",
                    st->count, base, (unsigned long)off);
        }
    }
    else
    {
        fprintf(g_ShDebugLog, "[CRASH]   #%02d 0x%lx\n",
                st->count, (unsigned long)pc);
    }

    st->count++;
    return _URC_NO_REASON;
}

static void Sh_CrashSignal(int sig)
{
    static volatile sig_atomic_t s_inHandler = 0;

    /* A fault inside the handler must die quietly rather than recurse. */
    if (s_inHandler)
    {
        _exit(1);
    }
    s_inHandler = 1;

    if (g_ShDebugLog != NULL)
    {
        Sh_UnwindState st;

        st.count = 0;
        fprintf(g_ShDebugLog, "[CRASH] signal %d -- backtrace follows\n", sig);
        _Unwind_Backtrace(Sh_UnwindFrame, &st);
        fflush(g_ShDebugLog);
    }

    /* Default disposition, then re-raise: Android writes its tombstone and the
     * process dies the way it would have. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void Sh_InstallCrashFilter(void)
{
    static const int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    struct sigaction sa;
    unsigned         i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = Sh_CrashSignal;
    sigemptyset(&sa.sa_mask);
    /* NODEFER so the recursion guard above sees a second fault rather than
     * the handler deadlocking; RESETHAND so the re-raise is not caught again. */
    sa.sa_flags = SA_NODEFER | SA_RESETHAND;

    for (i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++)
    {
        sigaction(kSignals[i], &sa, NULL);
    }
}

#endif /* _WIN32 */

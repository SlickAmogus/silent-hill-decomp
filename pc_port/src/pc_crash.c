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
#include <windows.h>

extern FILE* g_ShDebugLog;

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

/* Crash telemetry. The SH_DBG log is fully buffered (64 KB), so on an
 * access violation the last lines — the ones that matter — die with the
 * process. This SEH filter writes the faulting address as module+offset
 * (mappable straight to a function with nm/objdump on the exe or map DLL)
 * and flushes the log, then lets the normal crash path continue so
 * debuggers/WER still see the exception.
 *
 * Isolated in its own TU: windows.h defines `byte`, which conflicts with
 * the decomp's typedef in include/decomp/types.h. */
#include <stdio.h>
#include <windows.h>

extern FILE* g_ShDebugLog;

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
        fflush(g_ShDebugLog);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void Sh_InstallCrashFilter(void)
{
    SetUnhandledExceptionFilter(Sh_CrashFilter);
}

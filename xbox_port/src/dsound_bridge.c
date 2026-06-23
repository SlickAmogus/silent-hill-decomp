/* dsound_bridge.c — Bridge between RXDK's dsound.lib (MSVC) and nxdk (clang)
 *
 * dsound.lib was compiled by MSVC with __stdcall Win32 APIs (symbol: _Foo@N).
 * nxdk compiles Win32 wrappers as __cdecl (symbol: _Foo).
 * This file provides __stdcall versions that call nxdk's kernel APIs directly,
 * plus stubs for Xbox APIs and MSVC runtime symbols that dsound.lib needs.
 */

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ========================================================================
 * SECTION 1: __stdcall Win32 API wrappers
 * dsound.lib expects _CreateEventA@16 etc. nxdk only has _CreateEventA.
 * We implement them here with __stdcall, calling kernel APIs directly.
 * ======================================================================== */

/* Forward-declare nxdk's cdecl implementations.  We call through to them
 * (they're in nxdk/lib/winapi .obj files) — the compiler generates correct
 * cdecl call sites here and stdcall entry points for dsound.lib. */

/* Sync primitives */
HANDLE __stdcall CreateEventA_stdcall(
    LPSECURITY_ATTRIBUTES lpEventAttributes,
    BOOL bManualReset, BOOL bInitialState, LPCSTR lpName)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE hEvent = NULL;
    NTSTATUS status;

    oa.RootDirectory = NULL;
    oa.Attributes = 0;
    oa.ObjectName = NULL;

    status = NtCreateEvent(&hEvent, &oa,
        bManualReset ? NotificationEvent : SynchronizationEvent,
        bInitialState);
    return NT_SUCCESS(status) ? hEvent : NULL;
}
/* Alias so the linker resolves dsound's _CreateEventA@16 */
__asm__(".globl _CreateEventA@16\n_CreateEventA@16 = _CreateEventA_stdcall@16");

BOOL __stdcall SetEvent_stdcall(HANDLE hEvent)
{
    return NT_SUCCESS(NtSetEvent(hEvent, NULL));
}
__asm__(".globl _SetEvent@4\n_SetEvent@4 = _SetEvent_stdcall@4");

DWORD __stdcall WaitForSingleObject_stdcall(HANDLE hHandle, DWORD dwMilliseconds)
{
    LARGE_INTEGER timeout;
    PLARGE_INTEGER pTimeout = NULL;

    if (dwMilliseconds != 0xFFFFFFFF) { /* INFINITE */
        timeout.QuadPart = -(LONGLONG)dwMilliseconds * 10000LL;
        pTimeout = &timeout;
    }

    NTSTATUS status = KeWaitForSingleObject(
        (PVOID)hHandle, UserRequest, KernelMode, FALSE, pTimeout);

    if (status == STATUS_SUCCESS) return 0; /* WAIT_OBJECT_0 */
    if (status == STATUS_TIMEOUT) return 0x102; /* WAIT_TIMEOUT */
    return 0xFFFFFFFF; /* WAIT_FAILED */
}
__asm__(".globl _WaitForSingleObject@8\n_WaitForSingleObject@8 = _WaitForSingleObject_stdcall@8");

void __stdcall Sleep_stdcall(DWORD dwMilliseconds)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -(LONGLONG)dwMilliseconds * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
__asm__(".globl _Sleep@4\n_Sleep@4 = _Sleep_stdcall@4");

BOOL __stdcall SwitchToThread_stdcall(void)
{
    LARGE_INTEGER interval;
    interval.QuadPart = 0;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
    return TRUE;
}
__asm__(".globl _SwitchToThread@0\n_SwitchToThread@0 = _SwitchToThread_stdcall@0");

/* Handle management */
BOOL __stdcall CloseHandle_stdcall(HANDLE hObject)
{
    return NT_SUCCESS(NtClose(hObject));
}
__asm__(".globl _CloseHandle@4\n_CloseHandle@4 = _CloseHandle_stdcall@4");

/* Thread management */
typedef DWORD (__stdcall *LPTHREAD_START_ROUTINE_STDCALL)(LPVOID);

/* Thread trampoline — dsound uses __stdcall thread procs */
typedef struct {
    LPTHREAD_START_ROUTINE_STDCALL proc;
    LPVOID param;
} ThreadCtx;

static void ThreadTrampoline(PVOID ctx)
{
    ThreadCtx tc = *(ThreadCtx *)ctx;
    free(ctx);
    tc.proc(tc.param);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

HANDLE __stdcall CreateThread_stdcall(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE_STDCALL lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId)
{
    (void)lpThreadAttributes;
    (void)dwCreationFlags;
    (void)lpThreadId;

    ThreadCtx *ctx = (ThreadCtx *)malloc(sizeof(ThreadCtx));
    if (!ctx) return NULL;
    ctx->proc = lpStartAddress;
    ctx->param = lpParameter;

    HANDLE hThread = NULL;
    NTSTATUS status = PsCreateSystemThreadEx(
        &hThread,           /* ThreadHandle */
        0,                  /* ThreadExtensionSize */
        dwStackSize ? dwStackSize : 65536, /* KernelStackSize */
        0,                  /* TlsDataSize */
        NULL,               /* ThreadId */
        (PKSTART_ROUTINE)ThreadTrampoline,  /* StartRoutine */
        ctx,                /* StartContext */
        FALSE,              /* CreateSuspended */
        FALSE,              /* DebuggerThread */
        NULL);              /* SystemRoutine */

    if (!NT_SUCCESS(status)) {
        free(ctx);
        return NULL;
    }
    return hThread;
}
__asm__(".globl _CreateThread@24\n_CreateThread@24 = _CreateThread_stdcall@24");

void __stdcall ExitThread_stdcall(DWORD dwExitCode)
{
    (void)dwExitCode;
    PsTerminateSystemThread(STATUS_SUCCESS);
}
__asm__(".globl _ExitThread@4\n_ExitThread@4 = _ExitThread_stdcall@4");

/* Error handling */
static DWORD g_lastError = 0;

DWORD __stdcall GetLastError_stdcall(void)
{
    return g_lastError;
}
__asm__(".globl _GetLastError@0\n_GetLastError@0 = _GetLastError_stdcall@0");

/* File I/O — dsound.lib uses these for WMA file loading (we don't need WMA,
 * but the symbols must resolve). Forward to nxdk's implementations. */

/* Import nxdk's cdecl file functions */
extern HANDLE CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
extern BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
extern BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
extern BOOL SetEndOfFile(HANDLE);
extern DWORD SetFilePointer(HANDLE, LONG, PLONG, DWORD);
extern DWORD GetFileSize(HANDLE, LPDWORD);
extern BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
extern BOOL CancelIo(HANDLE);

HANDLE __stdcall CreateFileA_stdcall(LPCSTR a, DWORD b, DWORD c,
    LPSECURITY_ATTRIBUTES d, DWORD e, DWORD f, HANDLE g)
{ return CreateFileA(a, b, c, d, e, f, g); }
__asm__(".globl _CreateFileA@28\n_CreateFileA@28 = _CreateFileA_stdcall@28");

BOOL __stdcall ReadFile_stdcall(HANDLE a, LPVOID b, DWORD c, LPDWORD d, LPOVERLAPPED e)
{ return ReadFile(a, b, c, d, e); }
__asm__(".globl _ReadFile@20\n_ReadFile@20 = _ReadFile_stdcall@20");

BOOL __stdcall WriteFile_stdcall(HANDLE a, LPCVOID b, DWORD c, LPDWORD d, LPOVERLAPPED e)
{ return WriteFile(a, b, c, d, e); }
__asm__(".globl _WriteFile@20\n_WriteFile@20 = _WriteFile_stdcall@20");

BOOL __stdcall SetEndOfFile_stdcall(HANDLE a)
{ return SetEndOfFile(a); }
__asm__(".globl _SetEndOfFile@4\n_SetEndOfFile@4 = _SetEndOfFile_stdcall@4");

DWORD __stdcall SetFilePointer_stdcall(HANDLE a, LONG b, PLONG c, DWORD d)
{ return SetFilePointer(a, b, c, d); }
__asm__(".globl _SetFilePointer@16\n_SetFilePointer@16 = _SetFilePointer_stdcall@16");

DWORD __stdcall GetFileSize_stdcall(HANDLE a, LPDWORD b)
{ return GetFileSize(a, b); }
__asm__(".globl _GetFileSize@8\n_GetFileSize@8 = _GetFileSize_stdcall@8");

BOOL __stdcall GetOverlappedResult_stdcall(HANDLE a, LPOVERLAPPED b, LPDWORD c, BOOL d)
{ return GetOverlappedResult(a, b, c, d); }
__asm__(".globl _GetOverlappedResult@16\n_GetOverlappedResult@16 = _GetOverlappedResult_stdcall@16");

BOOL __stdcall CancelIo_stdcall(HANDLE a)
{
    (void)a;
    return TRUE; /* Stub — dsound uses this for WMA async I/O */
}
__asm__(".globl _CancelIo@4\n_CancelIo@4 = _CancelIo_stdcall@4");


/* ========================================================================
 * SECTION 2: Xbox API stubs
 * ======================================================================== */

/* XMemAlloc/XMemFree — dispatch to contiguous or heap memory.
 * Bit 31 of dwAttributes = MemoryType: 0=heap, 1=physical (contiguous).
 * The APU DMA engine needs physically contiguous pages, so we must use
 * MmAllocateContiguousMemory for those allocations.
 * MmAllocateContiguousMemory/MmFreeContiguousMemory are declared in
 * <xboxkrnl/xboxkrnl.h> (already included above). */

extern void xbox_log(const char *fmt, ...); /* from xbox_startup.c */

void * __stdcall XMemAlloc_stub(DWORD dwSize, DWORD dwAttributes)
{
    void *p;
    if (dwAttributes & 0x80000000) {
        /* Physical (contiguous) memory for DMA */
        p = MmAllocateContiguousMemory(dwSize);
        xbox_log("XMemAlloc: phys %u bytes attr=0x%08x -> %p\n",
                    (unsigned)dwSize, (unsigned)dwAttributes, p);
        return p;
    }
    p = malloc(dwSize);
    xbox_log("XMemAlloc: heap %u bytes attr=0x%08x -> %p\n",
                (unsigned)dwSize, (unsigned)dwAttributes, p);
    return p;
}
__asm__(".globl _XMemAlloc@8\n_XMemAlloc@8 = _XMemAlloc_stub@8");

void __stdcall XMemFree_stub(void *pAddress, DWORD dwAttributes)
{
    if (!pAddress) return;
    if (dwAttributes & 0x80000000) {
        MmFreeContiguousMemory(pAddress);
    } else {
        free(pAddress);
    }
}
__asm__(".globl _XMemFree@8\n_XMemFree@8 = _XMemFree_stub@8");

/* XGetAudioFlags — return stereo (start simple, add AC3/surround later) */
DWORD __stdcall XGetAudioFlags_stub(void)
{
    return 0; /* XC_AUDIO_FLAGS_STEREO = 0 */
}
__asm__(".globl _XGetAudioFlags@0\n_XGetAudioFlags@0 = _XGetAudioFlags_stub@0");

/* Section loading stubs — dsound uses these for DSP effect images */
HANDLE __stdcall XGetSectionHandleA_stub(LPCSTR lpSectionName)
{
    xbox_log("XGetSectionHandleA: '%s' -> NULL\n",
                lpSectionName ? lpSectionName : "(null)");
    return NULL; /* No sections — we don't use custom DSP images */
}
__asm__(".globl _XGetSectionHandleA@4\n_XGetSectionHandleA@4 = _XGetSectionHandleA_stub@4");

DWORD __stdcall XGetSectionSize_stub(HANDLE hSection)
{
    (void)hSection;
    return 0;
}
__asm__(".globl _XGetSectionSize@4\n_XGetSectionSize@4 = _XGetSectionSize_stub@4");

LPVOID __stdcall XLoadSectionByHandle_stub(HANDLE hSection)
{
    (void)hSection;
    return NULL;
}
__asm__(".globl _XLoadSectionByHandle@4\n_XLoadSectionByHandle@4 = _XLoadSectionByHandle_stub@4");

BOOL __stdcall XFreeSectionByHandle_stub(HANDLE hSection)
{
    (void)hSection;
    return TRUE;
}
__asm__(".globl _XFreeSectionByHandle@4\n_XFreeSectionByHandle@4 = _XFreeSectionByHandle_stub@4");


/* ========================================================================
 * SECTION 3: MSVC C runtime stubs
 * dsound.lib was compiled with MSVC and uses intrinsic/runtime symbols.
 * ======================================================================== */

/* __SEH_prolog / __SEH_epilog — Structured Exception Handling.
 * These are in libcmt.lib from RXDK. Link that instead of reimplementing.
 * __ftol2, __CIpow, __CIsinh, __fpclass — same, from libcmt.lib.
 * We add libcmt.lib to the link line in the Makefile. */

/* __fltused — MSVC linker flag indicating floating-point is used.
 * nxdk's libxboxrt already provides this. */

/* __iob — MSVC stdio FILE array.
 * dsound.lib references this for WMA debug output (printf/fprintf).
 * Provide a minimal 3-element array. dsound.lib only uses it for
 * debug/WMA output which we don't exercise.
 * Named __iob directly so the linker finds it (C-mangled to ___iob). */
typedef struct { char _placeholder[64]; } MSVC_FILE;
MSVC_FILE __iob[3];

/* clock() — dsound.lib uses this for timing in WMA decoder */
/* nxdk's pdclib should provide this, but alias just in case */

/* DirectSound global data (g_dwDirectSound*, g_fDirectSound*, etc.)
 * are defined in dsound.lib itself (globals.obj). No stubs needed. */

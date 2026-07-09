/*
 * crash_xbox.c - Unhandled-exception telemetry for the Xbox port.
 *
 * The developer cannot see the screen: on a fault the only artifact is
 * D:\silenthill.log, and without a handler the kernel's fatal path eats the
 * buffered tail of the log (the 25x GameState 10<->11 transition-loop log from
 * 2026-06 ends "cleanly" for exactly this reason). The Xbox kernel is an NT
 * derivative: CPU exceptions are dispatched through the SEH chain rooted at
 * FS:[0] (KPCR.NtTib.ExceptionList), newest frame first. We register a frame
 * FIRST (in main(), before MainLoop), so it sits at the BOTTOM of the chain:
 * every later-registered frame is asked before ours, meaning anything that
 * reaches us is effectively unhandled. We log code/EIP/registers + a bounded
 * EBP-chain backtrace, flush the log to disk, then return
 * ExceptionContinueSearch so the kernel's fatal screen still appears (the log
 * is already safe on the HDD by then).
 *
 * WHY NOT __try/__except: msvc_compat.c deliberately stubs _except_handler3
 * (a no-op continue-search for the prebuilt dsound objects) and it shadows
 * nxdk's real one at link time, so compiler-generated SEH scopes would
 * silently never run their filters. A raw EXCEPTION_REGISTRATION with our own
 * handler depends only on the kernel dispatcher and bypasses that entirely.
 *
 * STACK RESIDENCY: NT's RtlDispatchException validates each registration
 * record against the current thread's stack bounds and aborts the walk for
 * records outside them. The record therefore MUST live on the faulting
 * thread's stack: Crash_InstallSehFrame takes a pointer to caller-stack
 * storage, and main() (which never returns) owns the game thread's frame.
 *
 * EIP MAPPING: nxdk links main.exe with "-fixed -base:0x00010000" and CXBE
 * keeps that base, so runtime EIP == link-time VA. With the linker map
 * (Makefile.nxdk adds -map:bin/default.map) an EIP resolves directly to a
 * function name; "xbe+offset" is the same value rebased for convenience.
 */
#include <xboxkrnl/xboxkrnl.h> /* EXCEPTION_RECORD, CONTEXT, MmIsAddressValid */
#include <excpt.h>             /* EXCEPTION_REGISTRATION, EXCEPTION_DISPOSITION */
#include <stdint.h>

#include "sh_log.h"

extern void SH_DebugLogFlush(void); /* sh_log_xbox.c (not declared in sh_log.h) */

#define XBE_BASE            0x00010000u
#define STATUS_ACCESS_VIOLATION 0xC0000005u

/* Re-entrancy latch: if logging itself faults, the nested dispatch reaches us
 * again; bail immediately so we never recurse. */
static volatile int s_inCrashHandler = 0;

/* One-shot latch: log only the FIRST fault in a run in full. Later faults
 * (cascades after we continue-search) get a single terse line. */
static volatile int s_faultCount = 0;

static int Crash_PtrReadable(uint32_t addr)
{
    return MmIsAddressValid((PVOID)addr) &&
           MmIsAddressValid((PVOID)(addr + 7));
}

static EXCEPTION_DISPOSITION __cdecl Crash_UnhandledHandler(
    EXCEPTION_RECORD* rec, void* frame, CONTEXT* ctx, void* dispatcherCtx)
{
    (void)frame;
    (void)dispatcherCtx;

    /* Unwind passes traverse the same chain; only log the dispatch pass. */
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        return ExceptionContinueSearch;

    if (s_inCrashHandler)
        return ExceptionContinueSearch;
    s_inCrashHandler = 1;

    s_faultCount++;
    if (s_faultCount > 1)
    {
        SH_DBG("[FATAL] fault #%d code=%08lx eip=%08lx (suppressed detail)",
               s_faultCount, (unsigned long)rec->ExceptionCode,
               (unsigned long)(uintptr_t)rec->ExceptionAddress);
        SH_DebugLogFlush();
        s_inCrashHandler = 0;
        return ExceptionContinueSearch;
    }

    {
        uint32_t eip = ctx ? ctx->Eip : (uint32_t)(uintptr_t)rec->ExceptionAddress;

        SH_DBG("[FATAL] code=%08lx flags=%08lx eip=%08lx (xbe+%08lx) esp=%08lx ebp=%08lx",
               (unsigned long)rec->ExceptionCode,
               (unsigned long)rec->ExceptionFlags,
               (unsigned long)eip,
               (unsigned long)(eip - XBE_BASE),
               ctx ? (unsigned long)ctx->Esp : 0ul,
               ctx ? (unsigned long)ctx->Ebp : 0ul);

        if (ctx)
        {
            SH_DBG("[FATAL] eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx",
                   (unsigned long)ctx->Eax, (unsigned long)ctx->Ebx,
                   (unsigned long)ctx->Ecx, (unsigned long)ctx->Edx,
                   (unsigned long)ctx->Esi, (unsigned long)ctx->Edi);
        }

        if (rec->ExceptionCode == STATUS_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        {
            SH_DBG("[FATAL] access violation: %s addr=%08lx",
                   rec->ExceptionInformation[0] ? "WRITE" : "READ",
                   (unsigned long)rec->ExceptionInformation[1]);
        }

        /* Bounded EBP-chain backtrace. Every dereference is gated on
         * MmIsAddressValid so a garbage EBP cannot double-fault us; the
         * monotonic check stops loops. Return addresses are only printed if
         * they look like XBE code. */
        if (ctx)
        {
            uint32_t ebp   = ctx->Ebp;
            int      depth = 0;

            while (depth < 24)
            {
                uint32_t next;
                uint32_t ret;

                if ((ebp & 3) || !Crash_PtrReadable(ebp))
                    break;

                next = ((volatile uint32_t*)ebp)[0];
                ret  = ((volatile uint32_t*)ebp)[1];

                if (ret >= XBE_BASE && ret < 0x04000000u)
                {
                    SH_DBG("[FATAL] bt[%02d] ret=%08lx (xbe+%08lx) ebp=%08lx",
                           depth, (unsigned long)ret,
                           (unsigned long)(ret - XBE_BASE),
                           (unsigned long)ebp);
                }

                if (next <= ebp) /* must walk toward higher addresses */
                    break;
                ebp = next;
                depth++;
            }
        }

        SH_DebugLogFlush();
    }

    s_inCrashHandler = 0;
    return ExceptionContinueSearch; /* kernel fatal screen is acceptable; log is on disk */
}

/*
 * Install the crash frame at the head of this thread's FS:[0] chain.
 * `stackFrame` MUST point at an EXCEPTION_REGISTRATION that lives on THIS
 * thread's stack and outlives all code to be monitored (main() owns one for
 * the game thread; main never returns). Same fs-access pattern nxdk itself
 * uses in xboxrt/vcruntime/excpt.cpp, so it is known to compile under
 * nxdk clang (i386, GAS syntax).
 */
void Crash_InstallSehFrame(void* stackFrame)
{
    EXCEPTION_REGISTRATION* reg = (EXCEPTION_REGISTRATION*)stackFrame;

    reg->handler = (void*)Crash_UnhandledHandler;

    __asm__ __volatile__(
        "movl %%fs:0, %%eax\n\t"
        "movl %%eax, (%0)\n\t"  /* reg->prev = old chain head          */
        "movl %0, %%fs:0\n\t"   /* chain head = reg (we are newest for */
        :                       /* now; later frames stack above us)   */
        : "r"(reg)
        : "eax", "memory");

    SH_DBG("[SH-XBOX] SEH crash logger installed (frame=%p handler=%p)",
           (void*)reg, reg->handler);
}

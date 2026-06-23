/* pc_crash_posix.c — no-op crash filter stub for non-Windows platforms.
 * The actual crash telemetry (pc_crash.c) is Windows-only (SEH). */
#ifndef _WIN32
void Sh_InstallCrashFilter(void) {}
#endif

/*
 * xbox_compat_stubs.c - Link-time stubs for PSX/PC HAL the game calls but the
 * Xbox port doesn't implement yet (CD-ROM, SPU audio, pad, kernel events/timers/
 * memcard, PsyCross renderer internals, and PC-only debug/config/FMV helpers).
 *
 * C links by symbol NAME, so these intentionally use simplified signatures —
 * callers (compiled against the real psyq/PsyX prototypes) push their args and a
 * cdecl stub safely ignores them. Each returns a benign "no-op / success-ish"
 * value so MainLoop can run as far as possible; real behavior comes later
 * (CD->BIN-on-HDD, SPU->Xbox audio, pad->USB, etc.).
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* --- PSX libcd (CD-ROM): real BIN-on-HDD reader moved to cd_xbox.c --------- */

/* --- PSX libspu (sound): now implemented for real in audio_xbox.c (software
 * SPU mixer) + dsound_xbox.c (DirectSound hardware output). The no-op stubs
 * that used to live here were removed so the linker resolves the real ones. -- */

/* --- PSX libpad (controller): moved to pad_xbox.c (fills the PSX pad buffer) - */

/* --- PSX kernel: root counters / stack only. The event system (OpenEvent/
 * TestEvent/DeliverEvent/...) and the whole memory card (InitCARD, _card_*,
 * and the "buXX:" open/read/write/firstfile/erase/format file API) are now
 * implemented for real in mcard_xbox.c (E:\UDATA-backed 0.MCD card image). --*/
int  SetRCnt(void)     { return 1; }
int  StartRCnt(void)   { return 1; }
int  StopRCnt(void)    { return 1; }
void SetSp(void)       { }

/* --- PsyCross renderer internals (no GL renderer on Xbox) ------------------*/
void PsyX_EndScene(void)         { }
void PsyX_UpdateInput(void)      { }
void PsyX_GetScreenSize(int* w, int* h) { if (w) *w = 640; if (h) *h = 480; }
void PsyX_SetNextPrimSz(void)    { }
void PsyX_SetNextPrimPgxp(void)  { }
void PsyX_CaptureGteDepths(void) { }   /* called by the addPrim() macro */
void GR_DirectUploadVRAMRegion(void) { }
/* map7_s03's ending cutscene toggles PsyCross's OpenAL ADSR emulation around
 * the credits BGM. The Xbox audio HAL (dsound) has no such switch — report
 * "enabled" and ignore the set. */
int  PsyX_SPUAL_GetAdsrEnabled(void) { return 1; }
void PsyX_SPUAL_SetAdsrEnabled(int on) { (void)on; }

/* --- PC-only HAL (excluded source files); no-op on Xbox --------------------*/
void DbgOverlay_Update(void)            { }
void DbgOverlay_Render(void)            { }
/* FMV_Play: real STR/MDEC + XA player in fmv_xbox.c. */
void HiresOverride_RegisterFromTim(void){ }
void PcConfig_SaveMapName(void)         { }
void Pc_ConsoleApplyPendingFlags(void)  { }
void Pc_ConsoleFmvUpdate(void)          { }
void Pc_PlayWarningScreen(void)         { }
void Pc_QuickSaveLoadUpdate(void)       { }
int  PC_PlayerManualReloadRequested(void){ return 0; }
int  PC_Tick30HzReady(void)             { return 0; }
/* XaPlayer_* (XA voice/cutscene streaming): real implementation in xa_xbox.c. */
void CollVis_CaptureCylinder(void)      { }
void CollVis_CaptureHit(void)           { }
void CollVis_CaptureSeg(void)           { }
void CollVis_ClearCell(void)            { }

/* --- map overlay registry: real implementation moved to map_xbox.c ---------
 * The old blind stubs here (FindByName()=0, Load()=no-op) made every overlay
 * request "succeed" while keeping map0_s00's header active — the root cause of
 * the end-of-intro infinite transition loop. map_xbox.c now answers truthfully
 * and logs every request. */

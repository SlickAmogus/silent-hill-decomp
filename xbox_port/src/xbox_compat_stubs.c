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

/* --- PSX libcd (CD-ROM) ----------------------------------------------------*/
int   CdControl(void)    { return 1; }
int   CdControlB(void)   { return 1; }
void* CdIntToPos(int i, void* p) { (void)i; return p; }
int   CdRead(void)       { return 1; }
int   CdReadSync(void)   { return 0; }   /* 0 = transfer complete */
void* CdSearchFile(void) { return 0; }   /* file not found (no disc yet) */
int   CdSync(void)       { return 2; }   /* 2 = CdlComplete */

/* --- PSX libspu (sound) ----------------------------------------------------*/
void SpuInit(void)                  { }
void SpuQuit(void)                  { }
void SpuInitMalloc(void)            { }
u32  SpuSetTransferStartAddr(void)  { return 0; }
u32  SpuSetTransferMode(void)       { return 0; }
u32  SpuWrite(void)                 { return 0; }
int  SpuIsTransferCompleted(void)   { return 1; }   /* 1 = done */
void SpuSetKey(void)                { }
void SpuSetKeyOnWithAttr(void)      { }
void SpuSetVoiceAttr(void)          { }
void SpuGetVoiceAttr(void)          { }
u32  SpuGetKeyStatus(void)          { return 0; }
void SpuSetCommonAttr(void)         { }
u32  SpuSetReverb(void)             { return 0; }
void SpuSetReverbModeParam(void)    { }
u32  SpuSetReverbVoice(void)        { return 0; }
u32  SpuReserveReverbWorkArea(void) { return 1; }
void SpuClearReverbWorkArea(void)   { }

/* --- PSX libpad (controller): moved to pad_xbox.c (fills the PSX pad buffer) - */

/* --- PSX kernel: events / root counters / stack / memcard / BIOS files -----*/
u32  OpenEvent(void)   { return 0; }
int  CloseEvent(void)  { return 1; }
int  EnableEvent(void) { return 1; }
int  DisableEvent(void){ return 1; }
int  TestEvent(void)   { return 0; }   /* 0 = not fired */
int  SetRCnt(void)     { return 1; }
int  StartRCnt(void)   { return 1; }
int  StopRCnt(void)    { return 1; }
void SetSp(void)       { }
int  InitCARD(void)    { return 1; }
int  StartCARD(void)   { return 1; }
int  _card_clear(void) { return 1; }
int  _card_info(void)  { return 1; }
int  _card_load(void)  { return 1; }
int  _card_write(void) { return 1; }
int  _new_card(void)   { return 0; }
int  erase(void)       { return 0; }
int  firstfile(void)   { return 0; }
int  format(void)      { return 0; }
int  nextfile(void)    { return 0; }

/* --- PsyCross renderer internals (no GL renderer on Xbox) ------------------*/
void PsyX_EndScene(void)         { }
void PsyX_UpdateInput(void)      { }
void PsyX_GetScreenSize(int* w, int* h) { if (w) *w = 640; if (h) *h = 480; }
void PsyX_SetNextPrimSz(void)    { }
void PsyX_SetNextPrimPgxp(void)  { }
void PsyX_CaptureGteDepths(void) { }   /* called by the addPrim() macro */
void GR_DirectUploadVRAMRegion(void) { }

/* --- PC-only HAL (excluded source files); no-op on Xbox --------------------*/
void DbgOverlay_Update(void)            { }
void DbgOverlay_Render(void)            { }
int  FMV_Play(void)                     { return 0; }
void HiresOverride_RegisterFromTim(void){ }
void PcConfig_SaveMapName(void)         { }
void Pc_ConsoleApplyPendingFlags(void)  { }
void Pc_ConsoleFmvUpdate(void)          { }
void Pc_PlayWarningScreen(void)         { }
void Pc_QuickSaveLoadUpdate(void)       { }
int  PC_PlayerManualReloadRequested(void){ return 0; }
int  PC_Tick30HzReady(void)             { return 0; }
void XaPlayer_Play(void)                { }
void XaPlayer_Stop(void)                { }
void XaPlayer_Update(void)              { }
void XaPlayer_SetVolume(void)           { }
void CollVis_CaptureCylinder(void)      { }
void CollVis_CaptureHit(void)           { }
void CollVis_CaptureSeg(void)           { }
void CollVis_ClearCell(void)            { }

/* --- map overlay registry (PC map_registry.c excluded) ---------------------
 * The game only static-links map0_s00; these return its fixed identity. */
int         MapRegistry_Count(void)           { return 1; }
int         MapRegistry_FindByName(void)       { return 0; }   /* MapIdx_MAP0_S00 */
const char* MapRegistry_GetName(void)          { return "map0_s00"; }
const char* MapRegistry_GetDescription(void)   { return "Otherworld Alley"; }
int         MapRegistry_IsExactCellArena(void) { return 0; }
void        MapRegistry_Load(void)             { }

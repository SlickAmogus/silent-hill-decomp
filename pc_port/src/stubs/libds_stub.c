/*
 * libds_stub.c - CD-ROM library (alternate) stub implementations
 *
 * Used by the STREAM overlay for FMV playback.
 * On PC, these will be replaced with file I/O.
 */
#include "common.h"
#include <psyq/libds.h>

int DsInit(void) { return 1; }
int DsReset(void) { return 1; }
int DsCommand(unsigned char com, DslLOC *param, unsigned char count) { (void)com; (void)param; (void)count; return 0; }
int DsPacket(unsigned char *result) { (void)result; return 0; }
int DsSync(int mode, unsigned char *result) { (void)mode; (void)result; return 0; }
int DsReady(int mode, unsigned char *result) { (void)mode; (void)result; return 1; }
int DsControl(unsigned char com, unsigned char *param, unsigned char *result) { (void)com; (void)param; (void)result; return 0; }
int DsControlF(unsigned char com, unsigned char *param) { (void)com; (void)param; return 0; }
int DsControlB(unsigned char com, unsigned char *param, unsigned char *result) { (void)com; (void)param; (void)result; return 0; }
int DsRead(DslLOC *pos, int sectors, unsigned long *buf, int mode) { (void)pos; (void)sectors; (void)buf; (void)mode; return 0; }
int DsRead2(unsigned long *buf, int sectors) { (void)buf; (void)sectors; return 0; }
int DsReadSync(unsigned char *result) { (void)result; return 0; }
int DsSearchFile(DslFILE *fp, char *name) { (void)fp; (void)name; return 0; }
void DsFlush(void) { }
int DsDataSync(int mode) { (void)mode; return 0; }
int DsGetSector(void *madr, int size) { (void)madr; (void)size; return 0; }
int DsGetSector2(void *madr, int size) { (void)madr; (void)size; return 0; }
int DsMix(unsigned char *param, unsigned char *result) { (void)param; (void)result; return 0; }
int DsGetDiskType(void) { return DslCdromFormat; }
DslCB DsReadyCallback(DslCB func) { (void)func; return NULL; }
DslCB DsSyncCallback(DslCB func) { (void)func; return NULL; }

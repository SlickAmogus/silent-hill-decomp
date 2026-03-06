/*
 * libds.h - PSY-Q CD-ROM library (alternate) stub for PC port
 *
 * libds is an alternate CD-ROM library used by the STREAM overlay.
 * On PC, CD operations are replaced with file I/O.
 */
#ifndef _PSYQ_COMPAT_LIBDS_H
#define _PSYQ_COMPAT_LIBDS_H

#include <kernel.h>

/* CD-ROM Mode */
#define DslModeStream   0x100
#define DslModeStream2  0x120
#define DslModeSpeed    0x80
#define DslModeRT       0x40
#define DslModeSize1    0x20
#define DslModeSize0    0x10
#define DslModeSF       0x08
#define DslModeRept     0x04
#define DslModeAP       0x02
#define DslModeDA       0x01

/* Status */
#define DslStatPlay       0x80
#define DslStatSeek       0x40
#define DslStatRead       0x20
#define DslStatShellOpen  0x10
#define DslStatSeekError  0x04
#define DslStatStandby    0x02
#define DslStatError      0x01

#define DslStatNoDisk     0x01
#define DslOtherFormat    0x02
#define DslCdromFormat    0x04

/* Commands */
#define DslNop       0x01
#define DslSetloc    0x02
#define DslPlay      0x03
#define DslForward   0x04
#define DslBackward  0x05
#define DslReadN     0x06
#define DslStandby   0x07
#define DslStop      0x08
#define DslPause     0x09
#define DslMute      0x0B
#define DslDemute    0x0C
#define DslSetfilter 0x0D
#define DslSetmode   0x0E
#define DslGetTD     0x14
#define DslGetTN     0x13
#define DslReadS     0x1B
#define DslGetlocL   0x10
#define DslGetlocP   0x11

typedef struct {
    unsigned char minute;
    unsigned char second;
    unsigned char sector;
    unsigned char track;
} DslLOC;

typedef struct {
    DslLOC  pos;
    char    name[16];
    long    size;
} DslFILE;

typedef void (*DslCB)(unsigned char, unsigned char*);

#ifdef __cplusplus
extern "C" {
#endif

extern int  DsInit(void);
extern int  DsReset(void);
extern int  DsCommand(unsigned char com, DslLOC *param, unsigned char count);
extern int  DsPacket(unsigned char *result);
extern int  DsSync(int mode, unsigned char *result);
extern int  DsReady(int mode, unsigned char *result);
extern int  DsControl(unsigned char com, unsigned char *param, unsigned char *result);
extern int  DsControlF(unsigned char com, unsigned char *param);
extern int  DsControlB(unsigned char com, unsigned char *param, unsigned char *result);
extern int  DsRead(DslLOC *pos, int sectors, unsigned long *buf, int mode);
extern int  DsRead2(unsigned long *buf, int sectors);
extern int  DsReadSync(unsigned char *result);
extern int  DsSearchFile(DslFILE *fp, char *name);
extern void DsFlush(void);
extern int  DsDataSync(int mode);
extern int  DsGetSector(void *madr, int size);
extern int  DsGetSector2(void *madr, int size);
extern int  DsMix(unsigned char *param, unsigned char *result);
extern int  DsGetDiskType(void);
extern DslCB DsReadyCallback(DslCB func);
extern DslCB DsSyncCallback(DslCB func);

#ifdef __cplusplus
}
#endif

#endif /* _PSYQ_COMPAT_LIBDS_H */

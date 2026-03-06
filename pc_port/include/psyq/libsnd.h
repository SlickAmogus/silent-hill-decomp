/*
 * libsnd.h - PSY-Q Sound Library stub for PC port
 *
 * Silent Hill uses Konami's custom libsd/libsmf sound system rather than
 * the standard PSY-Q libsnd directly, but libsnd types are still referenced.
 * Implementations are in pc_port/src/stubs/libsnd_stub.c
 */
#ifndef _PSYQ_COMPAT_LIBSND_H
#define _PSYQ_COMPAT_LIBSND_H

#include <types.h>

/* Macros */
#define SSPLAY_INFINITY  0
#define SS_NOTICK        0x1000
#define SS_NOTICK0       0
#define SS_TICK60        1
#define SS_TICK240       2
#define SS_TICK120       3
#define SS_TICK50        4
#define SS_TICKVSYNC     5
#define SS_TICKMODE_MAX  6
#define SSPLAY_PAUSE     0
#define SSPLAY_PLAY      1
#define SS_SOFF          0
#define SS_SON           1
#define SS_MIX           0
#define SS_REV           1
#define SS_SERIAL_A      0
#define SS_SERIAL_B      1
#define SS_MUTE_OFF      0
#define SS_MUTE_ON       1
#define SS_IMEDIATE      0
#define SS_IMMEDIATE     0
#define SS_WAIT_COMPLETED 1

#define SS_REV_TYPE_OFF      0
#define SS_REV_TYPE_ROOM     1
#define SS_REV_TYPE_STUDIO_A 2
#define SS_REV_TYPE_STUDIO_B 3
#define SS_REV_TYPE_STUDIO_C 4
#define SS_REV_TYPE_HALL     5
#define SS_REV_TYPE_SPACE    6
#define SS_REV_TYPE_ECHO     7
#define SS_REV_TYPE_DELAY    8
#define SS_REV_TYPE_PIPE     9

#define SS_SEQ_TABSIZ 176

#define SND_VOLL  1
#define SND_VOLR  2
#define SND_ADSR1 4
#define SND_ADSR2 8
#define SND_ADDR  16
#define SND_PITCH 32

#ifndef NULL
#define NULL 0
#endif

/* VAB structures */
typedef struct VabHdr {
    long           form;
    long           ver;
    long           id;
    unsigned long  fsize;
    unsigned short reserved0;
    unsigned short ps;
    unsigned short ts;
    unsigned short vs;
    unsigned char  mvol;
    unsigned char  pan;
    unsigned char  attr1;
    unsigned char  attr2;
    unsigned long  reserved1;
} VabHdr;

typedef struct ProgAtr {
    unsigned char tones;
    unsigned char mvol;
    unsigned char prior;
    unsigned char mode;
    unsigned char mpan;
    char          reserved0;
    short         attr;
    unsigned long reserved1;
    unsigned long reserved2;
} ProgAtr;

typedef struct VagAtr {
    unsigned char  prior;
    unsigned char  mode;
    unsigned char  vol;
    unsigned char  pan;
    unsigned char  center;
    unsigned char  shift;
    unsigned char  min;
    unsigned char  max;
    unsigned char  vibW;
    unsigned char  vibT;
    unsigned char  porW;
    unsigned char  porT;
    unsigned char  pbmin;
    unsigned char  pbmax;
    unsigned char  reserved1;
    unsigned char  reserved2;
    unsigned short adsr1;
    unsigned short adsr2;
    short          prog;
    short          vag;
    short          reserved[4];
} VagAtr;

typedef struct {
    unsigned short left;
    unsigned short right;
} SndVolume;

typedef struct SndVolume2 {
    short left;
    short right;
} SndVolume2;

typedef struct SndRegisterAttr {
    SndVolume2 volume;
    short      pitch;
    short      mask;
    short      addr;
    short      adsr1;
    short      adsr2;
} SndRegisterAttr;

typedef struct SndVoiceStats {
    short          vagId;
    short          vabId;
    unsigned short pitch;
    short          note;
    short          tone;
    short          prog_num;
    short          prog_actual;
    short          vol;
    short          pan;
} SndVoiceStats;

typedef void (*SsMarkCallbackProc)(short, short, short);

/* Function call table */
#define CC_NUMBER     0
#define CC_BANKCHANGE 1
#define CC_DATAENTRY  2
#define CC_MAINVOL    3
#define CC_PANPOT     4
#define CC_EXPRESSION 5
#define CC_DAMPER     6
#define CC_NRPN1      7
#define CC_NRPN2      8
#define CC_RPN1       9
#define CC_RPN2       10
#define CC_EXTERNAL   11
#define CC_RESETALL   12

typedef struct {
    void (*noteon)();
    void (*programchange)();
    void (*pitchbend)();
    void (*metaevent)();
    void (*control[13])();
    void (*ccentry[20])();
} _SsFCALL;

#ifdef __cplusplus
extern "C" {
#endif

/* VAB operations */
extern short SsVabOpenHead(unsigned char*, short);
extern short SsVabOpenHeadSticky(unsigned char*, short, unsigned long);
extern short SsVabTransBody(unsigned char*, short);
extern short SsVabTransBodyPartly(unsigned char*, unsigned long, short);
extern short SsVabTransfer(unsigned char*, unsigned char*, short, short);
extern short SsVabTransCompleted(short);
extern void  SsVabClose(short);

/* System */
extern void  SsInit(void);
extern void  SsSetTickMode(long);
extern void  SsStart(void);
extern void  SsEnd(void);
extern void  SsQuit(void);
extern void  SsSeqCalledTbyT(void);

/* Sequencer */
extern short SsSeqOpen(unsigned long*, short);
extern void  SsSeqPlay(short, char, short);
extern void  SsSeqPause(short);
extern void  SsSeqReplay(short);
extern void  SsSeqStop(short);
extern void  SsSeqSetVol(short, short, short);
extern void  SsSeqClose(short);

/* Volume */
extern void  SsSetMVol(short, short);
extern void  SsGetMVol(SndVolume*);

/* Utilities */
extern short SsUtKeyOn(short, short, short, short, short, short, short);
extern short SsUtKeyOff(short, short, short, short, short);
extern void  SsUtReverbOn(void);
extern void  SsUtReverbOff(void);
extern short SsUtSetReverbType(short);
extern void  SsUtSetReverbDepth(short, short);
extern void  SsUtAllKeyOff(short);

extern _SsFCALL SsFCALL;

#ifdef __cplusplus
}
#endif

#endif /* _PSYQ_COMPAT_LIBSND_H */

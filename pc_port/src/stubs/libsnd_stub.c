/*
 * libsnd_stub.c - Sound library stub implementations
 *
 * Silent Hill uses Konami's custom sound system (libsd/libsmf) rather than
 * the standard PSY-Q libsnd directly. However, some libsnd types and functions
 * are still referenced. These stubs provide minimal implementations.
 *
 * The actual sound system will be implemented through the Konami SD/SMF layer.
 */
#include "common.h"
#include <psyq/libsnd.h>

_SsFCALL SsFCALL;

void SsInit(void) { }
void SsSetTickMode(long mode) { (void)mode; }
void SsStart(void) { }
/* SsEnd defined in bodyprog/libsd/smf_snd.c */
void SsQuit(void) { }
void SsSeqCalledTbyT(void) { }

short SsVabOpenHead(unsigned char* addr, short vab_id) { (void)addr; (void)vab_id; return 0; }
short SsVabOpenHeadSticky(unsigned char* addr, short vab_id, unsigned long size) { (void)addr; (void)vab_id; (void)size; return 0; }
short SsVabTransBody(unsigned char* addr, short vab_id) { (void)addr; (void)vab_id; return 0; }
short SsVabTransBodyPartly(unsigned char* addr, unsigned long size, short vab_id) { (void)addr; (void)size; (void)vab_id; return 0; }
short SsVabTransfer(unsigned char* h, unsigned char* b, short vab_id, short flag) { (void)h; (void)b; (void)vab_id; (void)flag; return 0; }
short SsVabTransCompleted(short flag) { (void)flag; return 1; }
void  SsVabClose(short vab_id) { (void)vab_id; }

short SsSeqOpen(unsigned long* addr, short vab_id) { (void)addr; (void)vab_id; return 0; }
void  SsSeqPlay(short seq_id, char play_mode, short vol) { (void)seq_id; (void)play_mode; (void)vol; }
void  SsSeqPause(short seq_id) { (void)seq_id; }
void  SsSeqReplay(short seq_id) { (void)seq_id; }
void  SsSeqStop(short seq_id) { (void)seq_id; }
void  SsSeqSetVol(short seq_id, short voll, short volr) { (void)seq_id; (void)voll; (void)volr; }
void  SsSeqClose(short seq_id) { (void)seq_id; }

/* SsSetMVol defined in bodyprog/libsd/smf_snd.c */
void  SsGetMVol(SndVolume* vol) { if (vol) { vol->left = 0; vol->right = 0; } }

short SsUtKeyOn(short a, short b, short c, short d, short e, short f, short g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return 0; }
short SsUtKeyOff(short a, short b, short c, short d, short e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void  SsUtReverbOn(void) { }
void  SsUtReverbOff(void) { }
short SsUtSetReverbType(short type) { (void)type; return 0; }
void  SsUtSetReverbDepth(short left, short right) { (void)left; (void)right; }
/* SsUtAllKeyOff defined in bodyprog/libsd/smf_snd.c */

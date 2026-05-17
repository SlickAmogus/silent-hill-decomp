/*
 * libgs.h - PSY-Q Graphics System stub for PC port
 *
 * PsyCross does not include libgs, so we provide the types and function
 * declarations here. Implementations are in pc_port/src/stubs/libgs_stub.c
 */
#ifndef _PSYQ_COMPAT_LIBGS_H
#define _PSYQ_COMPAT_LIBGS_H

#include <libgpu.h>
#include <libgte.h>

#ifndef NULL
#define NULL 0
#endif

typedef unsigned char PACKET;

#define GsDivMODE_NDIV 0
#define GsDivMODE_DIV  1
#define GsLMODE_NORMAL 0
#define GsLMODE_FOG    1
#define GsLMODE_LOFF   2

#define PSBANK 0x80000000
#define ZRESOLUTION 0x3fff
#define WORLD NULL
#define SCREEN ((GsCOORDINATE2 *)0x0001)

/* GTE function call tables for TMD rendering */
typedef struct {
    void (*f3[2][3])();
    void (*nf3[2])();
    void (*g3[2][3])();
    void (*ng3[2])();
    void (*tf3[2][3])();
    void (*ntf3[2])();
    void (*tg3[2][3])();
    void (*ntg3[2])();
    void (*f4[2][3])();
    void (*nf4[2])();
    void (*g4[2][3])();
    void (*ng4[2])();
    void (*tf4[2][3])();
    void (*ntf4[2])();
    void (*tg4[2][3])();
    void (*ntg4[2])();
    void (*f3g[2][3])();
    void (*g3g[2][3])();
    void (*f4g[2][3])();
    void (*g4g[2][3])();
} _GsFCALL;

typedef struct {
    VECTOR  scale;
    SVECTOR rotate;
    VECTOR  trans;
} GsCOORD2PARAM;

typedef struct _GsCOORDINATE2 {
    unsigned long flg;
    MATRIX  coord;
    MATRIX  workm;
    GsCOORD2PARAM *param;
    struct _GsCOORDINATE2 *super;
    struct _GsCOORDINATE2 *sub;
} GsCOORDINATE2;

typedef struct {
    MATRIX  view;
    GsCOORDINATE2 *super;
} GsVIEW2;

typedef struct {
    long    vpx, vpy, vpz;
    long    vrx, vry, vrz;
    long    rz;
    GsCOORDINATE2 *super;
} GsRVIEW2;

typedef struct {
    int     vx, vy, vz;
    unsigned char r, g, b;
} GsF_LIGHT;

/* GsOT_TAG must match PsyCross OT_TAG layout for linked list compatibility.
 * On PSX this was 4 bytes (24-bit pointer + 8-bit count).
 * With PsyCross extended pointers, OT_TAG is 12 bytes (packed).
 * Game code statically allocates GsOT_TAG arrays assuming PSX sizes,
 * so we dynamically reallocate OT arrays at init time. */
typedef OT_TAG GsOT_TAG;

typedef struct {
    unsigned long length;
    GsOT_TAG *org;
    unsigned long offset;
    unsigned long point;
    GsOT_TAG *tag;
} GsOT;

/* GPU primitive command codes */
#define GPU_COM_F3    0x20
#define GPU_COM_TF3   0x24
#define GPU_COM_G3    0x30
#define GPU_COM_TG3   0x34
#define GPU_COM_F4    0x28
#define GPU_COM_TF4   0x2c
#define GPU_COM_G4    0x38
#define GPU_COM_TG4   0x3c
#define GPU_COM_NF3   0x21
#define GPU_COM_NTF3  0x25
#define GPU_COM_NG3   0x31
#define GPU_COM_NTG3  0x35
#define GPU_COM_NF4   0x29
#define GPU_COM_NTF4  0x2d
#define GPU_COM_NG4   0x39
#define GPU_COM_NTG4  0x3d

typedef struct {
    unsigned long attribute;
    GsCOORDINATE2 *coord2;
    unsigned long *tmd;
    unsigned long id;
} GsDOBJ2;

typedef struct {
    unsigned long attribute;
    GsCOORDINATE2 *coord2;
    unsigned long *pmd;
    unsigned long *base;
    unsigned long *sv;
    unsigned long id;
} GsDOBJ3;

typedef struct {
    unsigned long attribute;
    GsCOORDINATE2 *coord2;
    unsigned long *tmd;
    unsigned long id;
    unsigned long *pri;
} GsDOBJ4;

typedef struct {
    unsigned long attribute;
    GsCOORDINATE2 *coord2;
    unsigned long *tmd;
    unsigned long id;
    unsigned long *pri;
    unsigned long *sv;
} GsDOBJ5;

#define GsDOBJ GsDOBJ2

typedef unsigned long VERT;

/* GsSPRITE - Sprite rendering structure */
typedef struct {
    unsigned long attribute;
    short   x, y;
    unsigned short w, h;
    unsigned short tpage;
    unsigned char u, v;
    short   cx, cy;
    unsigned char r, g, b;
    short   mx, my;
    short   scalex, scaley;
    long    rotate;
} GsSPRITE;

/* TMD_STRUCT - TMD model data structure */
typedef struct TMD_STRUCT {
    unsigned long *vertop;    /* vertex top address of TMD format */
    unsigned long  vern;      /* the number of vertex of TMD format */
    unsigned long *nortop;    /* normal top address of TMD format */
    unsigned long  norn;      /* the number of normal of TMD format */
    unsigned long *primtop;   /* primitive top address of TMD format */
    unsigned long  primn;     /* the number of primitives of TMD format */
    unsigned long  scale;     /* the scale factor of TMD format */
} TMD_STRUCT;

#ifdef __cplusplus
extern "C" {
#endif

/* Packet pointer - used for primitive allocation */
extern PACKET* GsOUT_PACKET_P;

/* Global matrices */
extern MATRIX GsWSMATRIX;
extern MATRIX GsWSMATRIX_ORG;
extern MATRIX GsIDMATRIX;
extern MATRIX GsIDMATRIX2;

/* Global GS state */
extern unsigned long GsLMODE, GsLIGNR, GsLIOFF, GsZOVER, GsBACKC, GsNDIV;

/* GsDISPENV/GsDRAWENV - global display/draw environment variables */
extern DISPENV GsDISPENV;
extern DRAWENV GsDRAWENV;

/* Initialization */
extern void GsInitGraph(int x, int y, int mode, int a, int b);
extern void GsInitGraph2(unsigned short x, unsigned short y, unsigned short intmode, unsigned short dith, unsigned short vrammode);
extern void GsDefDispBuff2(unsigned short x0, unsigned short y0, unsigned short x1, unsigned short y1);
extern void GsInit3D(void);
extern void GsInitVcount(void);
extern int  GsGetVcount(void);
extern void GsClearVcount(void);

/* Display buffer management */
extern void GsSwapDispBuff(void);
extern int  GsGetActiveBuff(void);

/* Ordering table operations */
extern void GsDrawOt(GsOT *ot);
extern void GsClearOt(int offset, int point, GsOT *ot);
extern void GsSortClear(unsigned char r, unsigned char g, unsigned char b, GsOT *ot);

/* Lighting */
extern void GsSetAmbient(long r, long g, long b);
extern void GsSetFlatLight(int id, GsF_LIGHT *lt);
extern void GsSetLightMode(int mode);
extern void GsSetLightMatrix(MATRIX *m);

/* TMD primitive packet layouts — must match on-disk TMD format.
 * Copied from include/psyq/libgs.h so pc_port code can parse TMD streams. */
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_short n0,v0; u_short v1,v2; }                                                   TMD_P_F3;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_short n0,v0; u_short n1,v1; u_short n2,v2; }                                     TMD_P_G3;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_short v0,v1;  u_short v2,p; }                                                    TMD_P_NF3;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_char r1,g1,b1,p1; u_char r2,g2,b2,p2; u_short v0,v1; u_short v2,p; }              TMD_P_NG3;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_short n0,v0; u_short v1,v2; u_short v3,p; }                                      TMD_P_F4;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_short n0,v0; u_short n1,v1; u_short n2,v2; u_short n3,v3; }                      TMD_P_G4;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_short v0,v1;  u_short v2,v3; }                                                   TMD_P_NF4;
typedef struct { u_char out, in, dummy, cd; u_char r0,g0,b0,code; u_char r1,g1,b1,p1; u_char r2,g2,b2,p2; u_char r3,g3,b3,p3; u_short v0,v1; u_short v2,v3; } TMD_P_NG4;

typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p; u_char r0,g0,b0,code; u_short v0,v1; u_short v2,p2; }            TMD_P_NTF3;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p; u_char r0,g0,b0,code; u_char r1,g1,b1,p1; u_char r2,g2,b2,p2; u_short v0,v1; u_short v2,p3; } TMD_P_NTG3;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p; u_short n0,v0; u_short v1,v2; }              TMD_P_TF3;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p; u_short n0,v0; u_short n1,v1; u_short n2,v2; } TMD_P_TG3;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p0; u_char tu3,tv3; u_short p1; u_short n0,v0; u_short v1,v2; u_short v3,p2; } TMD_P_TF4;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p0; u_char tu3,tv3; u_short p1; u_short n0,v0; u_short n1,v1; u_short n2,v2; u_short n3,v3; } TMD_P_TG4;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p0; u_char tu3,tv3; u_short p1; u_char r0,g0,b0,code; u_short v0,v1; u_short v2,v3; } TMD_P_NTF4;
typedef struct { u_char out, in, dummy, cd; u_char tu0,tv0; u_short clut; u_char tu1,tv1; u_short tpage; u_char tu2,tv2; u_short p0; u_char tu3,tv3; u_short p1; u_char r0,g0,b0,code; u_char r1,g1,b1,p2; u_char r2,g2,b2,p3; u_char r3,g3,b3,p4; u_short v0,v1; u_short v2,v3; } TMD_P_NTG4;

/* TMD cache — 64-bit compatible TMD parsing */
extern void GsMapModelingData(unsigned long *p);
extern struct TMD_STRUCT* GsGetTMDObject(unsigned long *base, int index);
extern void GsLinkObject4(unsigned long tmd_base, GsDOBJ2 *objp, int n);
extern void GsLinkObject4_PC(struct TMD_STRUCT *tmd, GsDOBJ2 *obj);
extern void GsSortObject4J(GsDOBJ2 *obj, GsOT *ot, int shift, unsigned long *scratch);
 
/* TMD rendering functions — lit + fog */
extern void GsTMDfastF3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastF4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastTF3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastTG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastTF4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastTG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);

/* TMD rendering functions — no light */
extern void GsTMDfastNF3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNG3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNF4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNG4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNTF3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNTG3(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNTF4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
extern void GsTMDfastNTG4(void* op, VERT* vp, PACKET* pk, int n, int shift, GsOT* ot, unsigned long* scratch);
 

/* View */
extern void GsSetView2(GsVIEW2 *v);
extern void GsSetRefView2(GsRVIEW2 *rv);
extern void GsSetProjection(long h);

/* Object rendering */
extern void GsSortObject4(GsDOBJ2 *obj, GsOT *ot, int shift, unsigned long *scratch);
extern void GsGetLw(GsCOORDINATE2 *coord, MATRIX *m);
extern void GsGetLs(GsCOORDINATE2 *coord, MATRIX *m);
extern void GsGetLws(GsCOORDINATE2 *coord, MATRIX *lw, MATRIX *ls);

/* Priority */
extern void SetPriority(PACKET*, int, int);

#ifdef __cplusplus
}
#endif

#endif /* _PSYQ_COMPAT_LIBGS_H */

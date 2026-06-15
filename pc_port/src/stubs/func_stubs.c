/* Auto-generated function stubs for unresolved symbols */

#include <libgte.h>
#include <libgpu.h>
#include <inline_c.h>
#include <string.h>

/* ReadLightMatrix - read GTE light matrix (control regs 8-12) into MATRIX */
void ReadLightMatrix(MATRIX *m)
{
    unsigned int r;
    r = CFC2(8);  m->m[0][0] = (short)(r & 0xFFFF); m->m[0][1] = (short)(r >> 16);
    r = CFC2(9);  m->m[0][2] = (short)(r & 0xFFFF); m->m[1][0] = (short)(r >> 16);
    r = CFC2(10); m->m[1][1] = (short)(r & 0xFFFF); m->m[1][2] = (short)(r >> 16);
    r = CFC2(11); m->m[2][0] = (short)(r & 0xFFFF); m->m[2][1] = (short)(r >> 16);
    r = CFC2(12); m->m[2][2] = (short)(r & 0xFFFF);
}

/* PSX TIM image API - OpenTIM/ReadTIM */
static u_long* g_timAddr = 0;

int OpenTIM(u_long* addr)
{
    g_timAddr = addr;
    /* Validate TIM header: magic 0x10, version 0x00 */
    if ((*(u_int*)addr & 0xff) != 0x10) return 0;
    return 1;
}

TIM_IMAGE* ReadTIM(TIM_IMAGE* timimg)
{
    u_int* rtim;
    if (!g_timAddr || !timimg) return 0;

    rtim = (u_int*)g_timAddr;

    timimg->mode = rtim[1];
    rtim += 2;

    /* CLUT present? */
    if (timimg->mode & 0x8) {
        timimg->crect = (RECT16*)&rtim[1];
        timimg->caddr = (u_int*)&rtim[3];
        rtim += rtim[0] >> 2;
    } else {
        timimg->crect = 0;
        timimg->caddr = 0;
    }

    timimg->prect = (RECT16*)&rtim[1];
    timimg->paddr = (u_int*)&rtim[3];

    return timimg;
}

/* SDL_main - SDL2main.a calls this; redirect to our actual main */
extern int main(int argc, char* argv[]);
int SDL_main(int argc, char* argv[]) { return main(argc, argv); }

void CdMix() { }
void CdRead2() { }
void CdReset() { }
/* LoadAverageCol: GTE color interpolation — output = (v0*p0 + v1*p1) >> 12
 * v0, v1, v2 are RGB byte triplets; p0, p1 are Q12 fixed-point weights. */
void LoadAverageCol(unsigned char *v0, unsigned char *v1, long p0, long p1, unsigned char *v2) {
    int r = (v0[0] * p0 + v1[0] * p1) >> 12;
    int g = (v0[1] * p0 + v1[1] * p1) >> 12;
    int b = (v0[2] * p0 + v1[2] * p1) >> 12;
    v2[0] = r > 255 ? 255 : (r < 0 ? 0 : r);
    v2[1] = g > 255 ? 255 : (g < 0 ? 0 : g);
    v2[2] = b > 255 ? 255 : (b < 0 ? 0 : b);
}
/* SpuGetVoiceAttr is now provided by PsyCross's libspu.c (real
 * implementation that copies attrs from g_SpuVoices). The previous
 * no-op stub left attr.pitch=0, which made libsd's per-frame radio
 * update pass pitch=0 and our SetVoiceAttr handler pause the OpenAL
 * source — radio static played for one frame and went silent. */
/* WorldObject_D_800D7FF0..80E0 (map4_s01 antique shop world-object work
 * structs): were mistakenly stubbed HERE as empty FUNCTIONS — the symbols
 * landed in .text, and Map_WorldObjectsInit's first
 * WorldObject_PlacementInit write faulted on the read-only code page
 * (user crash map4_s01.dll+0x7915 writing +0xE580). They are runtime-
 * written BSS data; real definitions now live in data_stubs.c. */
/* func_8005B62C: removed — upstream now has real implementation */
/* func_8005CD38: real implementation in pc_port/src/combat_target.c */
/* func_800692A4: paper-map tile renderer — real C body now lives in
 * src/bodyprog/bodyprog_mapscreen_80066D90.c under SH_PC_PORT */
/* func_8009E198: real C body in src/bodyprog/lib_unk/lib_8009E198.c under SH_PC_PORT */
void gte_ldsxy0() { }
void gte_ldv3c() { }
void gte_stsxy3_g3() { }
void gte_stsxy3c() { }
void gte_stsz3c() { }
void OuterProduct12() { }
void SetDrawOffset(DR_OFFSET* p, u_short* ofs) { (void)p; (void)ofs; }
void SetMulRotMatrix(MATRIX* m) { (void)m; }
void SetPolyG3(POLY_G3* p) { (void)p; }
VECTOR* Square0(VECTOR* v0, VECTOR* v1) {
    v1->vx = v0->vx * v0->vx;
    v1->vy = v0->vy * v0->vy;
    v1->vz = v0->vz * v0->vz;
    return v1;
}
int Lzc(long val) {
    /* Leading zero count */
    int count = 0;
    if (val == 0) return 32;
    if (val < 0) val = ~val;
    while (!(val & 0x80000000)) { count++; val <<= 1; }
    return count;
}

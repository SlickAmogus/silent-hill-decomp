/* Auto-generated function stubs for unresolved symbols */

#include <libgte.h>
#include <libgpu.h>
#include <string.h>

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
void LoadAverageCol() { }
void SpuGetVoiceAttr() { }
void WorldObject_D_800D7FF0() { }
void WorldObject_D_800D8020() { }
void WorldObject_D_800D8050() { }
void WorldObject_D_800D8070() { }
void WorldObject_D_800D8090() { }
void WorldObject_D_800D80B0() { }
void WorldObject_D_800D80E0() { }
void func_8005B62C() { }
void func_8005CD38() { }
void func_800692A4() { }
void func_8009E198() { }
void gte_ldsxy0() { }
void gte_ldv3c() { }
void gte_stsxy3_g3() { }
void gte_stsxy3c() { }
void gte_stsz3c() { }
void OuterProduct12() { }
void SetDrawOffset(DR_OFFSET* p, u_short* ofs) { (void)p; (void)ofs; }
void SetDrawStp(DR_STP* p, int pbw) { (void)p; (void)pbw; }
void SetMulRotMatrix(MATRIX* m) { (void)m; }
void SetPolyG3(POLY_G3* p) { (void)p; }
void Square0() { }
int Lzc(long val) {
    /* Leading zero count */
    int count = 0;
    if (val == 0) return 32;
    if (val < 0) val = ~val;
    while (!(val & 0x80000000)) { count++; val <<= 1; }
    return count;
}

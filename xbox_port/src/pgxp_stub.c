/*
 * pgxp_stub.c - PGXP (perspective-correct texturing) is OFF on the Xbox port.
 *
 * PsyCross's GTE (PsyX_GTE.cpp) references these PGXP symbols, which are
 * implemented in the GL renderer (PsyX_GPU.cpp) that we do not build on Xbox.
 * Provide no-op stubs so the GTE links; g_PsxUsePgxp = 0 keeps the PGXP code
 * paths in PsyX_GTE.cpp dormant at runtime (they are runtime-gated, not
 * compiled out). Revisit if/when perspective-correct texturing is wanted on
 * NV2A.
 */

int g_PsxUsePgxp = 0;

void PGXP_PushVertex(int sx, int sy, float fx, float fy, float fw)
{
    (void)sx; (void)sy; (void)fx; (void)fy; (void)fw;
}

void PGXP_MapPut(void* addr, float x, float y, float w)
{
    (void)addr; (void)x; (void)y; (void)w;
}

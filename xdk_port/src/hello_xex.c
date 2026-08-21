/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NOTE: builds against the Microsoft Xbox 360 XDK, which the repo does NOT
 * contain and must never contain. build_xex.bat locates it via %XEDK%.
 */
/*
 * hello_xex.c - first XEX: prove toolchain -> imagexex -> console, and run
 * the SAME display experiment the libXenon path could not deliver cleanly.
 *
 * The kernel/D3D path replaces everything that fought us bare-metal: no
 * EDRAM programming of our own, no resolve-copy registers, no framebuffer
 * aliasing -- D3D owns all of it, the same way every retail game did.
 *
 * Phase 1 (~10s): Clear+Present solid GREEN.
 * Phase 2 (~10s): solid RED.
 * Then it flips colours per second forever. Any of that on screen = the
 * whole XEX pipeline works, and the game HAL port proceeds on D3D.
 */
#include <xtl.h>

int main(void)
{
    IDirect3D9*           d3d;
    IDirect3DDevice9*     dev;
    D3DPRESENT_PARAMETERS pp;
    unsigned              frame = 0;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth        = 1280;
    pp.BackBufferHeight       = 720;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.FrontBufferFormat      = D3DFMT_LE_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, NULL,
                            D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);

    for (;;) {
        D3DCOLOR c;
        if (frame < 600)       c = D3DCOLOR_XRGB(0, 255, 0);   /* green */
        else if (frame < 1200) c = D3DCOLOR_XRGB(255, 0, 0);   /* red   */
        else                   c = ((frame / 60) & 1) ? D3DCOLOR_XRGB(0, 0, 255)
                                                      : D3DCOLOR_XRGB(255, 255, 0);
        IDirect3DDevice9_Clear(dev, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, c, 1.0f, 0);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        frame++;
    }
    return 0;
}

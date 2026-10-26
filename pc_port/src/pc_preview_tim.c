/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_preview_tim.h"
#include <stdint.h>
#include <stdlib.h>
static unsigned rd16(const unsigned char* p) {return p[0]|(unsigned)p[1]<<8;}
static uint32_t rd32(const unsigned char* p) {return rd16(p)|(uint32_t)rd16(p+2)<<16;}

unsigned char* Pc_PreviewTim_Decode(const unsigned char* b,size_t size,
                                   unsigned clut,int* width,int* height)
{
    if(!b || !width || !height || size<20 || rd32(b)!=0x10)return NULL;
    unsigned flags=rd32(b+4),mode=flags&7;
    if(!(flags&8) || mode>1)return NULL;
    size_t paletteBlock=rd32(b+8);
    if(paletteBlock<12 || paletteBlock>size-8 || size-8-paletteBlock<12)return NULL;
    unsigned px=rd16(b+12),py=rd16(b+14),pw=rd16(b+16),ph=rd16(b+18);
    unsigned colors=mode?256:16,cx=(clut&63)*16,cy=clut>>6;
    if(!pw || !ph || cx<px || cy<py || cy-py>=ph || cx-px>pw || colors>pw-(cx-px) ||
       (size_t)pw*ph>(paletteBlock-12)/2)return NULL;
    const unsigned char* palette=b+20+((size_t)(cy-py)*pw+cx-px)*2;
    size_t offset=8+paletteBlock,block=rd32(b+offset);
    if(block<12 || block>size-offset)return NULL;
    unsigned words=rd16(b+offset+8),h=rd16(b+offset+10),w=words*(mode?2:4);
    if(!w || !h || w>2048 || h>2048 || (size_t)words*h>(block-12)/2)return NULL;
    const unsigned char* pixels=b+offset+12;
    unsigned char* out=malloc((size_t)w*h*4);
    if(!out)return NULL;
    for(size_t i=0;i<(size_t)w*h;i++) {
        unsigned index=mode?pixels[i]:(pixels[i/2]>>((i%2)*4))&15;
        unsigned color=rd16(palette+index*2);
        out[i*4]=(color&31)*255/31;
        out[i*4+1]=((color>>5)&31)*255/31;
        out[i*4+2]=((color>>10)&31)*255/31;
        out[i*4+3]=color?255:0;
    }
    *width=(int)w;*height=(int)h;return out;
}

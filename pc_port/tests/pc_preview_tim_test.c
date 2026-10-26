/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "pc_preview_tim.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static void wr(unsigned char* p,unsigned v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int main(void)
{
    unsigned char b[68]={0};int w=0,h=0;
    wr(b,16);wr(b+4,8);wr(b+8,44);b[16]=16;b[18]=1;
    b[22]=31;wr(b+52,16);b[60]=2;b[62]=1;b[64]=0x11;
    unsigned char* rgba=Pc_PreviewTim_Decode(b,sizeof(b),0,&w,&h);
    assert(rgba && w==8 && h==1 && rgba[0]==255 && rgba[3]==255 && rgba[11]==0);free(rgba);
    assert(!Pc_PreviewTim_Decode(b,sizeof(b),64,&w,&h));
    for(size_t n=0;n<sizeof(b);n++)assert(!Pc_PreviewTim_Decode(b,n,0,&w,&h));
    wr(b+52,0xffffffff);assert(!Pc_PreviewTim_Decode(b,sizeof(b),0,&w,&h));
    puts("preview TIM palette/transparency/bounds: PASS");return 0;
}

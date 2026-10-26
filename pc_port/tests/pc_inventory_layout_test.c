/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_inventory_layout.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return EXIT_FAILURE;}}while(0)
int main(void)
{
    const int sizes[][2]={{640,480},{1280,720},{1920,1080},{3440,1440},{2392,1080},{600,900}};
    for(unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++) {
        PcInventoryPanel p[7];float w=sizes[n][0],h=sizes[n][1],x,y;
        CHECK(Pc_InventoryLayout(w,h,p)==6);
        for(int i=0;i<6;i++) {
            CHECK(p[i].w>0 && p[i].h>0 && p[i].x>=0 && p[i].y>=0);
            CHECK(p[i].x+p[i].w<=w+.01 && p[i].y+p[i].h<=h+.01);
            if(i==5)continue; /* Navigation has three individually spaced targets. */
            CHECK(Pc_InventoryPointer(p[i].x+p[i].w*.5f,p[i].y+p[i].h*.5f,w,h,&x,&y));
            CHECK(fabsf(x-(p[i].sx+p[i].sw*.5f))<.0001f);
            CHECK(fabsf(y-(p[i].sy+p[i].sh*.5f))<.0001f);
        }
        for(int i=0;i<3;i++) {
            CHECK(Pc_InventoryPointer(w*(.215f+i*.285f),h*.945f,w,h,&x,&y));
            CHECK(fabsf(x-(64.f+i*96.f)/320.f)<.0001f);
            CHECK(fabsf(y-421.f/448.f)<.0001f);
        }
        CHECK(!Pc_InventoryPointer(w*.35f,h*.945f,w,h,&x,&y));
        CHECK(!Pc_InventoryPointer(-1,-1,w,h,&x,&y));
        CHECK(Pc_InventoryCommandRow(p[2].x+p[2].w*.5f,h*.18f,w,h,2)==0);
        CHECK(Pc_InventoryCommandRow(p[2].x+p[2].w*.5f,h*.265f,w,h,2)==1);
        CHECK(Pc_InventoryCommandRow(p[2].x+p[2].w*.5f,h*.265f,w,h,1)==-1);
        CHECK(Pc_InventoryCommandRow(0,0,w,h,2)==-1);
    }
    puts("PASS inventory panel bounds and inverse hit-testing at six aspect ratios");
    return 0;
}

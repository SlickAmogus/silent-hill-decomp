/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "pc_preview_mesh.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void wr(unsigned char* p,unsigned n)
{p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
int main(void)
{
    unsigned char b[128]={0};PcPreviewMesh mesh={0};
    b[0]='0';b[1]=6;b[8]=1;wr(b+12,20);
    b[28]=1;wr(b+32,36);
    b[36]=1;b[37]=4;wr(b+40,60);wr(b+44,80);wr(b+48,96);
    b[60]=12;b[61]=34;b[62]=128;b[67]=2;
    b[72]=0;b[73]=1;b[74]=2;b[75]=3;
    b[84]=10;b[90]=20;b[92]=10;b[94]=20;
    assert(Pc_PreviewMesh_Decode(b,sizeof(b),0,&mesh));
    assert(mesh.vertexCount==6 && mesh.maximum[0]==10 && mesh.minimum[1]==-20);
    assert(mesh.textureCoordinates[0]==12 && mesh.textureCoordinates[1]==34 &&
        mesh.textureCoordinates[2]==2 && mesh.textureCoordinates[3]==128);
    assert(!memcmp(mesh.positions+6,mesh.positions+9,3*sizeof(float)));
    assert(!Pc_PreviewMesh_Decode(b,sizeof(b),0,&mesh));
    Pc_PreviewMesh_Free(&mesh);
    b[75]=255;assert(Pc_PreviewMesh_Decode(b,sizeof(b),0,&mesh));
    assert(mesh.vertexCount==3);Pc_PreviewMesh_Free(&mesh);
    b[74]=4;assert(!Pc_PreviewMesh_Decode(b,sizeof(b),0,&mesh));b[74]=2;
    wr(b+44,0xfffffff0);assert(!Pc_PreviewMesh_Decode(b,sizeof(b),0,&mesh));
    wr(b+44,80);b[29]=1;assert(!Pc_PreviewMesh_Decode(b,sizeof(b),0,&mesh));b[29]=0;
    for(unsigned n=0;n<104;n++)assert(!Pc_PreviewMesh_Decode(b,n,0,&mesh));
    /* Mutate offsets, counts and primitive bytes in a bounded synthetic model.
     * Under ASan/UBSan neither accepted nor rejected inputs may escape bounds. */
    unsigned seed=12345;
    for(unsigned i=0;i<10000;i++) {
        unsigned char mutated[sizeof(b)];memcpy(mutated,b,sizeof(b));
        seed=seed*1664525u+1013904223u;unsigned at=seed%sizeof(b);
        seed=seed*1664525u+1013904223u;mutated[at]=(unsigned char)(seed>>24);
        Pc_PreviewMesh_Decode(mutated,sizeof(mutated),0,&mesh);Pc_PreviewMesh_Free(&mesh);
    }
    puts("preview mesh bounds/triangles/quads/ownership: PASS");
    return 0;
}

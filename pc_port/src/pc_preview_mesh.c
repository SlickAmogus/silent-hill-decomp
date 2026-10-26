/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_preview_mesh.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned rd16(const unsigned char* p) { return p[0] | (unsigned)p[1]<<8; }
static uint32_t rd32(const unsigned char* p)
{ return rd16(p) | (uint32_t)rd16(p+2)<<16; }
static int extent(size_t size, size_t offset, size_t count, size_t stride)
{ return offset<=size && count<=(size-offset)/stride; }
void Pc_PreviewMesh_Free(PcPreviewMesh* mesh)
{
    if(!mesh)return;
    free(mesh->positions);free(mesh->textureCoordinates);memset(mesh,0,sizeof(*mesh));
}

int Pc_PreviewMesh_Decode(const unsigned char* lm, size_t size, unsigned model,
                          PcPreviewMesh* out)
{
    PcPreviewMesh result={0};size_t triangles=0,write=0;
    if(!lm || !out || out->positions || out->textureCoordinates || size<20 || size>4*1024*1024 ||
       lm[0]!='0' || lm[1]!=6 || lm[2]!=0 || model>=lm[8])return 0;
    size_t models=rd32(lm+12);
    if(!extent(size,models,lm[8],16))return 0;
    const unsigned char* header=lm+models+model*16;
    /* The diagnostic deliberately refuses models relying on an external
     * vertex window; the production skinned importer must handle those later. */
    if(header[9]!=0 || header[10]!=0 || (header[11]&1))return 0;
    size_t meshes=rd32(header+12);
    if(!extent(size,meshes,header[8],24))return 0;
    for(unsigned m=0;m<header[8];m++) {
        const unsigned char* mesh=lm+meshes+m*24;
        unsigned count=mesh[0],vertices=mesh[1];
        size_t prim=rd32(mesh+4),xy=rd32(mesh+8),z=rd32(mesh+12);
        if(!extent(size,prim,count,20) || !extent(size,xy,vertices,4) ||
           !extent(size,z,vertices,2))return 0;
        for(unsigned i=0;i<count;i++) {
            const unsigned char* indices=lm+prim+i*20+12;
            unsigned corners=indices[3]==255?3:4;
            for(unsigned k=0;k<corners;k++)if(indices[k]>=vertices)return 0;
            triangles+=corners-2;
        }
    }
    if(!triangles || triangles>65536)return 0;
    result.positions=malloc(triangles*9*sizeof(float));
    if(!result.positions)return 0;
    result.textureCoordinates=malloc(triangles*12*sizeof(float));
    if(!result.textureCoordinates){Pc_PreviewMesh_Free(&result);return 0;}
    for(unsigned m=0;m<header[8];m++) {
        const unsigned char* mesh=lm+meshes+m*24;
        size_t prim=rd32(mesh+4),xy=rd32(mesh+8),z=rd32(mesh+12);
        for(unsigned i=0;i<mesh[0];i++) {
            const unsigned char* indices=lm+prim+i*20+12;
            /* PSX quads use strip order 0,1,2 / 2,1,3, not a polygon fan. */
            static const unsigned strip[6]={0,1,2,2,1,3};
            unsigned n=indices[3]==255?3:6;
            for(unsigned k=0;k<n;k++) {
                unsigned v=indices[strip[k]];
                const unsigned char* primitive=lm+prim+i*20;
                static const unsigned uvOffsets[4]={0,4,8,10};
                unsigned uv=uvOffsets[strip[k]];
                result.textureCoordinates[write*4]=primitive[uv];
                result.textureCoordinates[write*4+1]=primitive[uv+1];
                result.textureCoordinates[write*4+2]=primitive[7]&127;
                result.textureCoordinates[write*4+3]=rd16(primitive+2);
                float p[3]={(int16_t)rd16(lm+xy+v*4),
                    -(float)(int16_t)rd16(lm+xy+v*4+2),(int16_t)rd16(lm+z+v*2)};
                for(unsigned a=0;a<3;a++) {
                    result.positions[write*3+a]=p[a];
                    if(!write || p[a]<result.minimum[a])result.minimum[a]=p[a];
                    if(!write || p[a]>result.maximum[a])result.maximum[a]=p[a];
                }
                write++;
            }
        }
    }
    result.vertexCount=write;*out=result;return 1;
}

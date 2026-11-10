/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The stock (ILM) characters for Character Select's preview, read straight
 * off the disc: each body part's vertices placed by the rest pose of the
 * character's own ANM (keyframe 0), textured from its TIM with the palette
 * row each primitive names. The layout is the launcher's IlmObjConverter.cs
 * (pc_port/tools/ilm_obj.py) export path, kept to what a preview needs. */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bodyprog/chara/chara.h"
#include "main/fileinfo.h"
#include "lang_text.h"
#include "pc_config.h"
#include "pc_ilm_preview.h"
#include "pc_playas.h"
#include "sh_log.h"

#define IP_MAX_BONES 64
#define IP_POOL      256

static unsigned ip_u16(const u8* d, int o) { return (unsigned)(d[o] | (d[o + 1] << 8)); }
static unsigned ip_u32(const u8* d, int o) { return (unsigned)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((unsigned)d[o + 3] << 24)); }
static int      ip_s16(const u8* d, int o) { return (s16)ip_u16(d, o); }

/* A whole file by its file-table index: the loose copy in gamedata/load when
 * there is one (mods retexture TIMs), else the disc. */
static u8* ip_read(int idx, int* outSize)
{
    char        name[32], path[96], folder[16];
    const char* f;
    size_t      len;
    FILE*       fp;
    u8*         buf;

    *outSize = 0;
    if (idx <= 0 || idx >= FS_FILE_COUNT)
        return NULL;
    Fs_GetFileName(name, idx);
    f = g_FilePaths[g_FileTable[idx].pathIdx];
    if (f[0] == '\\')
        f++;
    len = strlen(f);
    while (len > 0 && f[len - 1] == '\\')
        len--;
    if (len >= sizeof(folder))
        len = sizeof(folder) - 1;
    memcpy(folder, f, len);
    folder[len] = '\0';
    snprintf(path, sizeof(path), "gamedata/load/%s/%s", folder, name);
    if (g_PcConfig.allowLooseFiles && (fp = fopen(path, "rb")) != NULL)
    {
        long n;

        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        buf = (n > 0) ? (u8*)malloc((size_t)n) : NULL;
        if (buf != NULL && fread(buf, 1, (size_t)n, fp) == (size_t)n)
        {
            fclose(fp);
            *outSize = (int)n;
            return buf;
        }
        free(buf);
        fclose(fp);
    }
    *outSize = (int)g_FileTable[idx].blockCount * 256;
    return Pc_LangReadDiscFile(g_FileTable[idx].startSector, (unsigned)*outSize);
}

unsigned char* Pc_IlmPreview_ReadFile(int idx, int* outSize)
{
    return ip_read(idx, outSize);
}

/* ---- ANM rest pose (Anim_BoneInit + Vw_CoordHierarchyMatrixCompute) ------ */

typedef struct
{
    int R[9];
    int T[3];
    int done;
} IpBone;

static int ip_anm_rest(const u8* a, int size, IpBone* out, int maxBones)
{
    int dataOff, rotCount, transCount, kfSize, boneCount, scale, rootY, i, pass;

    if (size < 0x14)
        return 0;
    dataOff    = (int)ip_u16(a, 0);
    rotCount   = a[2];
    transCount = a[3];
    kfSize     = (int)ip_u16(a, 4);
    boneCount  = a[6];
    scale      = a[0x12];
    rootY      = a[0x13];
    if (boneCount <= 0 || boneCount > maxBones || scale >= 16 || rotCount * 9 + transCount * 3 != kfSize ||
        dataOff + kfSize > size)
        return 0;

    for (i = 0; i < boneCount; i++)
        out[i].done = 0;
    /* The root never moves: its keyframe translation is root motion. */
    memset(&out[0], 0, sizeof(out[0]));
    out[0].R[0] = out[0].R[4] = out[0].R[8] = 4096;
    out[0].done = 1;
    /* Parents before children whatever the bind table's order: resolve in
     * passes until nothing changes. */
    for (pass = 0; pass < boneCount; pass++)
    {
        int progress = 0;

        for (i = 1; i < boneCount; i++)
        {
            const u8* b = a + 0x14 + i * 6;
            int       parent = (s8)b[0], rIdx = (s8)b[1], tIdx = (s8)b[2];
            int       R[9], T[3], k;

            if (out[i].done)
                continue;
            if (parent >= 0 && parent < boneCount && parent != i && !out[parent].done)
                continue;
            if (tIdx < 0)
                for (k = 0; k < 3; k++)
                    T[k] = (s8)b[3 + k] << scale;
            else
            {
                for (k = 0; k < 3; k++)
                    T[k] = (s8)a[dataOff + tIdx * 3 + k] << scale;
                if (tIdx == 0)
                    T[1] -= rootY;
            }
            if (rIdx < 0)
            {
                memset(R, 0, sizeof(R));
                R[0] = R[4] = R[8] = 4096;
            }
            else
                for (k = 0; k < 9; k++)
                    R[k] = (s8)a[dataOff + transCount * 3 + rIdx * 9 + k] << 5;

            if (parent >= 0 && parent < boneCount && parent != i)
            {
                const IpBone* p = &out[parent];
                int           r, c;

                for (r = 0; r < 3; r++)
                    for (c = 0; c < 3; c++)
                        out[i].R[r * 3 + c] = (int)(((long long)p->R[r * 3] * R[c] + (long long)p->R[r * 3 + 1] * R[3 + c] +
                                                     (long long)p->R[r * 3 + 2] * R[6 + c]) >> 12);
                for (r = 0; r < 3; r++)
                    out[i].T[r] = (int)(((long long)p->R[r * 3] * T[0] + (long long)p->R[r * 3 + 1] * T[1] +
                                         (long long)p->R[r * 3 + 2] * T[2]) >> 12) + p->T[r];
            }
            else
            {
                memcpy(out[i].R, R, sizeof(R));
                memcpy(out[i].T, T, sizeof(T));
            }
            out[i].done = 1;
            progress = 1;
        }
        if (!progress)
            break;
    }
    return boneCount;
}

/* ---- TIM ---------------------------------------------------------------- */

typedef struct
{
    int       w, h;          /* pixels */
    int       bpp;           /* 4 or 8 */
    int       clutW, clutRows;
    const u8* clut;
    const u8* pix;
} IpTim;

static int ip_tim(const u8* t, int size, IpTim* out)
{
    unsigned flags;
    int      o = 8;

    memset(out, 0, sizeof(*out));
    if (size < 8 || ip_u32(t, 0) != 0x10)
        return 0;
    flags = ip_u32(t, 4);
    out->bpp = ((flags & 3) == 0) ? 4 : ((flags & 3) == 1) ? 8 : 0;
    if (out->bpp == 0 || !(flags & 8))
        return 0;
    if (o + 12 > size)
        return 0;
    out->clutW    = (int)ip_u16(t, o + 8);
    out->clutRows = (int)ip_u16(t, o + 10);
    out->clut     = t + o + 12;
    o += (int)ip_u32(t, o);
    if (o + 12 > size)
        return 0;
    out->w   = (int)ip_u16(t, o + 8) * (out->bpp == 4 ? 4 : 2);
    out->h   = (int)ip_u16(t, o + 10);
    out->pix = t + o + 12;
    if (out->pix + (out->w * out->h * out->bpp) / 8 > t + size)
        return 0;
    return out->w > 0 && out->h > 0 && out->clutRows > 0;
}

/* The sheet decoded through one palette row, RGBA; colour 0 is a hole. */
static u8* ip_tim_rgba(const IpTim* tim, int row)
{
    u8* out = (u8*)malloc((size_t)tim->w * tim->h * 4);
    int x, y;

    if (out == NULL)
        return NULL;
    if (row < 0 || row >= tim->clutRows)
        row = 0;
    for (y = 0; y < tim->h; y++)
        for (x = 0; x < tim->w; x++)
        {
            int      idx;
            unsigned c;
            u8*      px = out + ((size_t)y * tim->w + x) * 4;

            if (tim->bpp == 4)
            {
                u8 b = tim->pix[(y * tim->w + x) / 2];
                idx = (x & 1) ? (b >> 4) : (b & 15);
            }
            else
                idx = tim->pix[y * tim->w + x];
            if (idx >= tim->clutW)
                idx = 0;
            c = ip_u16(tim->clut, (row * tim->clutW + idx) * 2);
            px[0] = (u8)((c & 31) << 3);
            px[1] = (u8)(((c >> 5) & 31) << 3);
            px[2] = (u8)(((c >> 10) & 31) << 3);
            px[3] = (c == 0) ? 0 : 255;
        }
    return out;
}

/* ---- ILM ---------------------------------------------------------------- */

typedef struct
{
    int   valid;
    float x, y, z; /* placed by its owning part's rest matrix */
} IpVert;

static int ip_bone_of(const u8* name)
{
    if (name[0] >= '0' && name[0] <= '9' && name[1] >= '0' && name[1] <= '9')
        return (name[0] - '0') * 10 + (name[1] - '0');
    return 0;
}

static int ip_hidden(const char* part, const char* const* hide)
{
    int i;

    for (i = 0; hide != NULL && hide[i] != NULL; i++)
        if (strncmp(part, hide[i], 8) == 0)
            return 1;
    return 0;
}

void Pc_IlmPreview_Free(IlmPreview* p)
{
    int i;

    free(p->stream);
    for (i = 0; i < p->matCount; i++)
        free(p->rgba[i]);
    memset(p, 0, sizeof(*p));
}

int Pc_IlmPreview_Build(int charaId, IlmPreview* out)
{
    u8*         ilm = NULL;
    u8*         anm = NULL;
    u8*         tim = NULL;
    int         ilmSize, anmSize, timSize, modelCount, modelHdrs, order, matsP, matCount;
    IpBone      bones[IP_MAX_BONES];
    int         boneCount;
    IpTim       sheet;
    IpVert      pool[IP_POOL];
    int         rowOf[IP_MAX_MAT];
    int         i, pass;
    float*      s;
    int         cap, n;
    const char* const* hide;

    memset(out, 0, sizeof(*out));
    if (charaId <= 0 || charaId >= Chara_Count)
        return 0;
    ilm = ip_read(CHARA_FILE_INFOS[charaId].modelFileIdx, &ilmSize);
    anm = ip_read(CHARA_FILE_INFOS[charaId].animFileIdx, &anmSize);
    tim = ip_read(CHARA_FILE_INFOS[charaId].textureFileIdx, &timSize);
    if (ilm == NULL || anm == NULL || tim == NULL || ilmSize < 0x14 || ilm[0] != '0' || ilm[1] != 6 ||
        !ip_tim(tim, timSize, &sheet) || (boneCount = ip_anm_rest(anm, anmSize, bones, IP_MAX_BONES)) == 0)
    {
        SH_DBG("[CHARAPREV] stock chara %d: model, rest pose or texture unreadable", charaId);
        free(ilm);
        free(anm);
        free(tim);
        return 0;
    }
    hide       = Pc_PlayAs_HiddenPartsFor(charaId);
    matCount   = ilm[3];
    matsP      = (int)ip_u32(ilm, 4);
    modelCount = ilm[8];
    modelHdrs  = (int)ip_u32(ilm, 0xC);
    order      = (int)ip_u32(ilm, 0x10);
    out->texW  = sheet.w;
    out->texH  = sheet.h;

    /* Pass 0 counts corners per palette row and fixes the materials; pass 1
     * writes them grouped by row. */
    for (i = 0; i < IP_MAX_MAT; i++)
        rowOf[i] = -1;
    cap = 0;
    s   = NULL;
    for (pass = 0; pass < 2; pass++)
    {
        int counts[IP_MAX_MAT] = { 0 }, fill[IP_MAX_MAT] = { 0 }, oi;

        if (pass == 1)
        {
            int m, at = 0;

            for (m = 0; m < out->matCount; m++)
            {
                out->mats[m].first = at;
                fill[m]            = at;
                at += out->mats[m].count;
            }
            cap = at;
            s   = (float*)malloc((size_t)cap * 8 * sizeof(float));
            if (s == NULL)
                break;
        }
        memset(pool, 0, sizeof(pool));
        /* Parts in the draw order the vertex pool is filled in: a part reads
         * what an earlier part left there wherever they meet (the seams). */
        for (oi = 0; oi < modelCount; oi++)
        {
            int       mi = (order + modelCount <= ilmSize) ? ilm[order + oi] : oi;
            int       hb, meshHdrs, meshCount, vOff, k, skip;
            const u8* name;
            const IpBone* bone;

            if (mi >= modelCount)
                mi = oi;
            hb        = modelHdrs + mi * 16;
            name      = ilm + hb;
            meshCount = ilm[hb + 8];
            vOff      = ilm[hb + 9];
            meshHdrs  = (int)ip_u32(ilm, hb + 0xC);
            bone      = &bones[ip_bone_of(name) < boneCount ? ip_bone_of(name) : 0];
            skip      = ip_hidden((const char*)name, hide);
            for (k = 0; k < meshCount; k++)
            {
                int mb = meshHdrs + k * 24;
                int primCount = ilm[mb], vertCount = ilm[mb + 1], p, v;
                int primsP = (int)ip_u32(ilm, mb + 4), xyP = (int)ip_u32(ilm, mb + 8), zP = (int)ip_u32(ilm, mb + 0xC);

                for (v = 0; v < vertCount && vOff + v < IP_POOL; v++)
                {
                    float x = (float)ip_s16(ilm, xyP + v * 4), y = (float)ip_s16(ilm, xyP + v * 4 + 2);
                    float z = (float)ip_s16(ilm, zP + v * 2);
                    IpVert* pv = &pool[vOff + v];

                    pv->valid = 1;
                    pv->x = (bone->R[0] * x + bone->R[1] * y + bone->R[2] * z) / 4096.0f + bone->T[0];
                    pv->y = (bone->R[3] * x + bone->R[4] * y + bone->R[5] * z) / 4096.0f + bone->T[1];
                    pv->z = (bone->R[6] * x + bone->R[7] * y + bone->R[8] * z) / 4096.0f + bone->T[2];
                }
                if (skip)
                    continue;
                for (p = 0; p < primCount; p++)
                {
                    static const int uvOff[4] = { 0, 4, 8, 0xA };
                    static const int tri[3] = { 0, 1, 2 }, quad[6] = { 0, 1, 3, 0, 3, 2 };
                    int       pb = primsP + p * 20, c, mat, row, base, slot;
                    unsigned  flags = ip_u16(ilm, pb + 6);
                    int       isTri = ilm[pb + 0xF] == 0xFF;
                    const int* corners = isTri ? tri : quad;
                    int       nc = isTri ? 3 : 6;

                    mat  = (int)((flags >> 8) & 0x7F);
                    base = (mat < matCount) ? (int)(ip_u16(ilm, matsP + mat * 24 + 0x10) >> 6) : 0;
                    row  = (int)(ip_u16(ilm, pb + 2) >> 6) - base;
                    if (row < 0 || row >= sheet.clutRows)
                        row = 0;
                    for (slot = 0; slot < out->matCount; slot++)
                        if (rowOf[slot] == row)
                            break;
                    if (slot == out->matCount)
                    {
                        if (pass == 1 || out->matCount >= IP_MAX_MAT)
                            continue;
                        rowOf[slot] = row;
                        out->matCount++;
                    }
                    for (c = 0; c < 4 && c < (isTri ? 3 : 4); c++)
                        if (!pool[ilm[pb + 0xC + c]].valid)
                            break;
                    if (c < (isTri ? 3 : 4))
                        continue;
                    if (pass == 0)
                    {
                        out->mats[slot].count += nc;
                        continue;
                    }
                    {
                        float tp[6][3];
                        float nx, ny, nz, ax, ay, az, bx, by, bz, l;
                        int   t;

                        for (c = 0; c < nc; c++)
                        {
                            const IpVert* pv = &pool[ilm[pb + 0xC + corners[c]]];
                            tp[c][0] = pv->x;
                            tp[c][1] = pv->y;
                            tp[c][2] = pv->z;
                        }
                        for (t = 0; t < nc; t += 3)
                        {
                            ax = tp[t + 1][0] - tp[t][0];
                            ay = tp[t + 1][1] - tp[t][1];
                            az = tp[t + 1][2] - tp[t][2];
                            bx = tp[t + 2][0] - tp[t][0];
                            by = tp[t + 2][1] - tp[t][1];
                            bz = tp[t + 2][2] - tp[t][2];
                            nx = ay * bz - az * by;
                            ny = az * bx - ax * bz;
                            nz = ax * by - ay * bx;
                            l  = nx * nx + ny * ny + nz * nz;
                            if (l > 0.0f)
                            {
                                l = 1.0f / sqrtf(l);
                                nx *= l;
                                ny *= l;
                                nz *= l;
                            }
                            for (c = t; c < t + 3; c++)
                            {
                                unsigned uv = ip_u16(ilm, pb + uvOff[corners[c]]);
                                float*   o = s + (size_t)fill[slot] * 8;

                                o[0] = tp[c][0];
                                o[1] = tp[c][1];
                                o[2] = tp[c][2];
                                o[3] = nx;
                                o[4] = ny;
                                o[5] = nz;
                                o[6] = ((float)(uv & 0xFF) + 0.5f) / (float)sheet.w;
                                o[7] = ((float)((uv >> 8) & 0xFF) + 0.5f) / (float)sheet.h;
                                fill[slot]++;
                            }
                        }
                    }
                }
            }
        }
    }
    free(ilm);
    free(anm);

    if (s == NULL || cap == 0)
    {
        free(tim);
        free(s);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->stream  = s;
    out->corners = cap;
    out->bounds[0] = out->bounds[1] = out->bounds[2] = 1e30f;
    out->bounds[3] = out->bounds[4] = out->bounds[5] = -1e30f;
    for (n = 0; n < cap; n++)
        for (i = 0; i < 3; i++)
        {
            if (s[n * 8 + i] < out->bounds[i])     out->bounds[i]     = s[n * 8 + i];
            if (s[n * 8 + i] > out->bounds[i + 3]) out->bounds[i + 3] = s[n * 8 + i];
        }
    for (i = 0; i < out->matCount; i++)
        out->rgba[i] = ip_tim_rgba(&sheet, rowOf[i]);
    free(tim);
    SH_DBG("[CHARAPREV] stock chara %d: %d triangles, %d palette rows, sheet %dx%d", charaId, cap / 3,
           out->matCount, out->texW, out->texH);
    return 1;
}

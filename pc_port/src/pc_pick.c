/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Console object picking + per-object SCALE.
 *
 * Picking is deferred by one frame on purpose. A click arrives in the console's
 * update, which runs before the game's frame; the GTE view state a hit test
 * needs belongs to the frame the player is actually looking at, so the click is
 * parked and resolved from Pc_Pick_CharaPreDraw, which the draw path already
 * calls for every character it is about to submit. That also means only drawn
 * characters are pickable, which is what a click on the picture should mean.
 *
 * The hit test happens in prim coordinates -- the space RotTransPers outputs
 * and the space PsyCross's world ortho maps to the picture. Inverting that
 * ortho (g_PcWorldOrthoRect) rather than assuming a 320x224 frame is what keeps
 * the test honest under hfov/vfov, pillarbox, Hor+ and stretch alike.
 *
 * SCALE publishes g_PcCharaDrawScale for the character about to be drawn, and
 * the skeleton draw scales each bone final matrices about the root. It used to
 * multiply bone 0 coord instead, but bone 0 only reaches bones the ANM
 * parented, and a skeleton built from the LM can carry parts beyond that --
 * which is why the Incubator scaled only from the waist down. Nothing is left
 * in the bone coords either, so a scale can never compound.
 *
 * Collision is separate (Pc_Pick_CollScale) and does scale, except the
 * player's map-collision cylinder -- see Pc_Pick_MoveScale.
 */
#include "game.h"

#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/chara.h"
#include "bodyprog/gfx/world_object.h"

#include "pc_pick.h"
#include "pc_mod_registry.h" /* Pc_Console_Print */

#include <stdio.h>
#include <string.h>

/* PsyCross: the world pass's ortho rect in prim coordinates, and the window ->
 * picture mapping the mouse cursor already goes through. */
extern float g_PcWorldOrthoRect[4];
extern int   PsyX_MapWindowToViewport(int mx, int my, float* outFracX, float* outFracY);

/* pc_console_cmd.c: SPAWN_CHARAS reverse lookup ("GROANER"), NULL if unlisted. */
const char* Pc_Console_CharaName(s32 charaId);

#define PICK_SCALE_MIN Q12(0.05f)
#define PICK_SCALE_MAX Q12(20.0f)

static int s_selKind    = PcPick_None;
static int s_selSlot    = -1;
static s32 s_selCharaId = -1;

static s32 s_npcScale[NPC_COUNT_MAX];
/* Which character each NPC slot scale was set for. A slot is recycled on a
 * map change or a respawn, and without this the next monster to land in it
 * would inherit the previous one's size. */
static s32 s_npcScaleChara[NPC_COUNT_MAX];
static s32 s_playerScale;
static int s_scalesInit;

/* Nothing is scaled in a normal session, and the collision read sites sit in
 * hot loops, so every one of them is gated on this single test. Set when a
 * scale other than 1.0 is applied; never cleared, because a stale 1 only
 * costs the lookup it used to cost anyway. */
static int s_anyScaled;

/* Props are submitted fresh every frame, so the draw list index is not an
 * identity. A placement is static, so its model plus its packed position is:
 * that pair is what a scaled prop is remembered by. */
#define PICK_PROP_MAX 32

typedef struct
{
    const void* model;
    s32         x, y, z;
    s32         scale;
} s_PickProp;

static s_PickProp s_props[PICK_PROP_MAX];
static int        s_propCount;

static const void* s_selPropModel;
static s32         s_selPropX, s_selPropY, s_selPropZ;

/* FREEZE and HEAL state per NPC slot, each tagged with the character it was
 * recorded for so a recycled slot starts clean (same reason as
 * s_npcScaleChara). -1 = nothing recorded. */
/* NOCOLLIDE, tagged with the character it was set for exactly like
 * s_frozenChara so a recycled slot starts solid again. */
static s32 s_noColChara[NPC_COUNT_MAX];
static int s_noColPlayer;

/* Collision asks per character per frame, so keep a global fast-out: with
 * nothing phased the query must not walk the NPC array at all. */
static int s_anyNoCol;

static s32 s_frozenChara[NPC_COUNT_MAX];
static s32 s_maxHealth[NPC_COUNT_MAX];
static s32 s_maxHealthChara[NPC_COUNT_MAX];
static int s_npcStateInit;

static void NpcStateInit(void)
{
    int i;

    if (s_npcStateInit)
        return;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_frozenChara[i]    = -1;
        s_noColChara[i]     = -1;
        s_maxHealth[i]      = 0;
        s_maxHealthChara[i] = -1;
    }
    s_npcStateInit = 1;
}

/* Pending click: 0 = none, 1 = armed (waiting for a frame), 2 = frame ran. */
static int s_pendState;
static int s_pendX, s_pendY;
static int s_bestKind;
static int s_bestSlot;
static s32 s_bestCharaId;
static s32 s_bestDepth;
static const void* s_bestPropModel;
static s32 s_bestPropX, s_bestPropY, s_bestPropZ;

static void ScalesInit(void)
{
    int i;

    if (s_scalesInit)
        return;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_npcScale[i]      = Q12(1.0f);
        s_npcScaleChara[i] = -1;
    }

    s_playerScale = Q12(1.0f);
    s_scalesInit  = 1;
}

static s_PickProp* PropFind(const void* model, s32 x, s32 y, s32 z, int create)
{
    int i;

    for (i = 0; i < s_propCount; i++)
    {
        if (s_props[i].model == model && s_props[i].x == x &&
            s_props[i].y == y && s_props[i].z == z)
        {
            return &s_props[i];
        }
    }

    if (!create || s_propCount >= PICK_PROP_MAX)
        return NULL;

    s_props[s_propCount].model = model;
    s_props[s_propCount].x     = x;
    s_props[s_propCount].y     = y;
    s_props[s_propCount].z     = z;
    s_props[s_propCount].scale = Q12(1.0f);
    return &s_props[s_propCount++];
}

static s32* ScaleSlot(int kind, int slot)
{
    ScalesInit();

    if (kind == PcPick_Player)
        return &s_playerScale;
    if (kind == PcPick_Npc && slot >= 0 && slot < NPC_COUNT_MAX)
        return &s_npcScale[slot];
    if (kind == PcPick_Prop)
    {
        s_PickProp* pr = PropFind(s_selPropModel, s_selPropX, s_selPropY, s_selPropZ, 1);
        return (pr != NULL) ? &pr->scale : NULL;
    }

    return NULL;
}

static void DescribeInto(int kind, int slot, s32 charaId, char* out, int outSize)
{
    const char* name;

    if (out == NULL || outSize <= 0)
        return;

    if (kind == PcPick_Player)
    {
        snprintf(out, (size_t)outSize, "player");
        return;
    }

    if (kind == PcPick_Prop)
    {
        snprintf(out, (size_t)outSize, "prop @ (%.1f, %.1f, %.1f)",
                 s_selPropX / 256.0f, s_selPropY / 256.0f, s_selPropZ / 256.0f);
        return;
    }

    if (kind != PcPick_Npc)
    {
        snprintf(out, (size_t)outSize, "nothing");
        return;
    }

    name = Pc_Console_CharaName(charaId);
    if (name != NULL)
        snprintf(out, (size_t)outSize, "npc[%d] %s", slot, name);
    else
        snprintf(out, (size_t)outSize, "npc[%d] chara %d", slot, (int)charaId);
}

/* Project a world point to prim coordinates. 0 when it is behind the camera. */
static int ProjectWorld(q19_12 x, q19_12 y, q19_12 z, float* outX, float* outY, s32* outDepth)
{
    MATRIX  mat;
    SVECTOR v;
    s32     sxy;
    long    p;
    long    flag;
    s32     otz;

    v.vx  = 0;
    v.vy  = 0;
    v.vz  = 0;
    v.pad = 0;

    Vw_WorldScreenMatrixAtPositionGet(&mat, x, y, z);
    SetRotMatrix(&mat);
    SetTransMatrix(&mat);

    otz = RotTransPers(&v, (int*)&sxy, &p, &flag);
    if (otz <= 8)
        return 0;

    *outX     = (float)(s16)(sxy & 0xFFFF);
    *outY     = (float)(s16)((u32)sxy >> 16);
    *outDepth = otz;

    if (*outX < -4096.0f || *outX > 4096.0f || *outY < -4096.0f || *outY > 4096.0f)
        return 0;

    return 1;
}

static int CursorPrimPos(int wx, int wy, float* outX, float* outY)
{
    float fx;
    float fy;

    if (!PsyX_MapWindowToViewport(wx, wy, &fx, &fy))
        return 0;

    *outX = g_PcWorldOrthoRect[0] + fx * (g_PcWorldOrthoRect[1] - g_PcWorldOrthoRect[0]);
    *outY = g_PcWorldOrthoRect[2] + fy * (g_PcWorldOrthoRect[3] - g_PcWorldOrthoRect[2]);

    return 1;
}

void Pc_Pick_RequestAt(int windowX, int windowY)
{
    s_pendState   = 1;
    s_pendX       = windowX;
    s_pendY       = windowY;
    s_bestKind    = PcPick_None;
    s_bestSlot    = -1;
    s_bestCharaId = -1;
    s_bestDepth   = 0;
}

void Pc_Pick_Clear(int announce)
{
    if (announce)
    {
        if (s_selKind == PcPick_None)
        {
            Pc_Console_Print("nothing was selected");
        }
        else
        {
            char what[64];
            char line[96];

            DescribeInto(s_selKind, s_selSlot, s_selCharaId, what, sizeof(what));
            snprintf(line, sizeof(line), "deselected %s", what);
            Pc_Console_Print(line);
        }
    }

    s_selKind    = PcPick_None;
    s_selSlot    = -1;
    s_selCharaId = -1;
}

int Pc_Pick_CollScale(const void* charaPtr)
{
    const s_SubCharacter* chara = (const s_SubCharacter*)charaPtr;
    int                   i;

    if (!s_anyScaled || chara == NULL || !s_scalesInit)
        return Q12(1.0f);

    if (chara == &g_SysWork.playerWork.player)
        return s_playerScale;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        if (chara == &g_SysWork.npcs[i])
        {
            if (s_npcScale[i] != Q12(1.0f) && s_npcScaleChara[i] != chara->model.charaId)
                return Q12(1.0f); /* slot recycled into another character */
            return s_npcScale[i];
        }
    }

    return Q12(1.0f);
}

int Pc_Pick_MoveScale(const void* charaPtr)
{
    if (charaPtr == (const void*)&g_SysWork.playerWork.player)
        return Q12(1.0f);

    return Pc_Pick_CollScale(charaPtr);
}

int Pc_Pick_ScaleAbout(int origin, int v, int scaleQ12)
{
    return origin + (int)(((long long)(v - origin) * scaleQ12) >> 12);
}

int Pc_Pick_SelectNpc(int slot)
{
    s_SubCharacter* npc;

    if (slot < 0 || slot >= NPC_COUNT_MAX)
        return 0;

    npc = &g_SysWork.npcs[slot];
    if (npc->model.charaId == Chara_None)
        return 0;

    s_selKind    = PcPick_Npc;
    s_selSlot    = slot;
    s_selCharaId = npc->model.charaId;
    return 1;
}

int Pc_Pick_NearestNpc(void)
{
    const s_SubCharacter* hr = &g_SysWork.playerWork.player;
    int    best     = -1;
    s64    bestDist = 0;
    int    i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        s64 dx, dz, d;

        if (npc->model.charaId == Chara_None || npc->model.charaId == Chara_Harry)
            continue;
        if (npc->health <= Q12(0.0f))
            continue;

        dx = (s64)npc->position.vx - hr->position.vx;
        dz = (s64)npc->position.vz - hr->position.vz;
        d  = dx * dx + dz * dz;

        if (best < 0 || d < bestDist)
        {
            best     = i;
            bestDist = d;
        }
    }

    return best;
}

void Pc_Pick_SelectPlayer(void)
{
    s_selKind    = PcPick_Player;
    s_selSlot    = -1;
    s_selCharaId = g_SysWork.playerWork.player.model.charaId;
}

int Pc_Pick_Kind(void) { return s_selKind; }
int Pc_Pick_Slot(void) { return s_selSlot; }

void Pc_Pick_Describe(char* out, int outSize)
{
    DescribeInto(s_selKind, s_selSlot, s_selCharaId, out, outSize);
}

int Pc_Pick_SetScale(int scaleQ12)
{
    s32* slot = ScaleSlot(s_selKind, s_selSlot);

    if (slot == NULL)
        return 0;

    if (scaleQ12 < PICK_SCALE_MIN)
        scaleQ12 = PICK_SCALE_MIN;
    if (scaleQ12 > PICK_SCALE_MAX)
        scaleQ12 = PICK_SCALE_MAX;

    *slot = scaleQ12;
    if (scaleQ12 != Q12(1.0f))
        s_anyScaled = 1;
    if (s_selKind == PcPick_Npc && s_selSlot >= 0 && s_selSlot < NPC_COUNT_MAX)
        s_npcScaleChara[s_selSlot] = s_selCharaId;

    return 1;
}

int Pc_Pick_GetScale(void)
{
    s32* slot = ScaleSlot(s_selKind, s_selSlot);

    return (slot != NULL) ? *slot : Q12(1.0f);
}

void Pc_Pick_Reset(void)
{
    s_anyScaled  = 0;
    s_anyNoCol   = 0;
    s_noColPlayer = 0;
    s_propCount  = 0;
    s_scalesInit = 0;
    ScalesInit();
    s_selKind    = PcPick_None;
    s_selSlot    = -1;
    s_selCharaId = -1;
    s_pendState  = 0;
    s_npcStateInit = 0;
    NpcStateInit();
}

void* Pc_Pick_SelectedNpc(void)
{
    s_SubCharacter* npc;

    if (s_selKind != PcPick_Npc || s_selSlot < 0 || s_selSlot >= NPC_COUNT_MAX)
        return NULL;

    npc = &g_SysWork.npcs[s_selSlot];
    if (npc->model.charaId == Chara_None || npc->model.charaId != s_selCharaId)
        return NULL;

    return npc;
}

int Pc_Pick_PropPosition(int* x, int* y, int* z)
{
    if (s_selKind != PcPick_Prop)
        return 0;

    /* Placements are stored Q8; callers work in world Q12. */
    *x = (int)(s_selPropX << 4);
    *y = (int)(s_selPropY << 4);
    *z = (int)(s_selPropZ << 4);
    return 1;
}

int Pc_Pick_NpcTick(const void* npcPtr, int slot)
{
    const s_SubCharacter* npc = (const s_SubCharacter*)npcPtr;

    if (slot < 0 || slot >= NPC_COUNT_MAX || npc == NULL)
        return 0;

    NpcStateInit();

    /* Enemies set their own health on their first AI tick and there is no
     * max-health field, so the highest value seen is what HEAL restores. */
    if (s_maxHealthChara[slot] != npc->model.charaId)
    {
        s_maxHealthChara[slot] = npc->model.charaId;
        s_maxHealth[slot]      = 0;
        s_frozenChara[slot]    = -1;
    }
    if (npc->health > s_maxHealth[slot])
        s_maxHealth[slot] = npc->health;

    return s_frozenChara[slot] == npc->model.charaId;
}

int Pc_Pick_NpcMaxHealth(int slot)
{
    if (slot < 0 || slot >= NPC_COUNT_MAX || !s_npcStateInit)
        return 0;
    if (s_maxHealthChara[slot] != g_SysWork.npcs[slot].model.charaId)
        return 0;

    return s_maxHealth[slot];
}

int Pc_Pick_SetNoCollide(int on)
{
    NpcStateInit();

    if (s_selKind == PcPick_Player)
    {
        s_noColPlayer = on ? 1 : 0;
    }
    else if (s_selKind == PcPick_Npc && s_selSlot >= 0 && s_selSlot < NPC_COUNT_MAX)
    {
        if (g_SysWork.npcs[s_selSlot].model.charaId == Chara_None)
            return 0;
        s_noColChara[s_selSlot] = on ? s_selCharaId : -1;
    }
    else
    {
        /* Props are world geometry, not characters: they never run the chara
         * collision paths this flag gates, so there is nothing to phase. */
        return 0;
    }

    if (on)
        s_anyNoCol = 1;

    return 1;
}

int Pc_Pick_GetNoCollide(void)
{
    if (s_selKind == PcPick_Player)
        return s_noColPlayer;

    if (s_selKind == PcPick_Npc && s_selSlot >= 0 && s_selSlot < NPC_COUNT_MAX)
        return Pc_Pick_IsNoCollide(&g_SysWork.npcs[s_selSlot]);

    return 0;
}

int Pc_Pick_IsNoCollide(const void* charaPtr)
{
    const s_SubCharacter* chara = (const s_SubCharacter*)charaPtr;
    int                   i;

    if (!s_anyNoCol || chara == NULL)
        return 0;

    if (chara == &g_SysWork.playerWork.player)
        return s_noColPlayer;

    if (!s_npcStateInit)
        return 0;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        if (chara == &g_SysWork.npcs[i])
        {
            return s_noColChara[i] != -1 &&
                   s_noColChara[i] == chara->model.charaId;
        }
    }

    return 0;
}

int Pc_Pick_SetFrozen(int slot, int on)
{
    if (slot < 0 || slot >= NPC_COUNT_MAX || g_SysWork.npcs[slot].model.charaId == Chara_None)
        return 0;

    NpcStateInit();
    s_frozenChara[slot] = on ? g_SysWork.npcs[slot].model.charaId : -1;
    return 1;
}

int Pc_Pick_IsFrozen(int slot)
{
    if (slot < 0 || slot >= NPC_COUNT_MAX || !s_npcStateInit)
        return 0;

    return g_SysWork.npcs[slot].model.charaId != Chara_None &&
           s_frozenChara[slot] == g_SysWork.npcs[slot].model.charaId;
}

void Pc_Pick_CharaPreDraw(struct _SubCharacter* charaPtr, int slot, void* boneCoordsPtr)
{
    s_SubCharacter* chara      = (s_SubCharacter*)charaPtr;
    GsCOORDINATE2*  boneCoords = (GsCOORDINATE2*)boneCoordsPtr;
    int             kind       = (slot < 0) ? PcPick_Player : PcPick_Npc;
    s32*            scale;

    if (chara == NULL)
        return;

    /* A recycled NPC slot is a different character: drop a stale selection
     * rather than scale whatever moved in. */
    if (kind == PcPick_Npc && s_selKind == PcPick_Npc && slot == s_selSlot &&
        chara->model.charaId != s_selCharaId)
    {
        Pc_Pick_Clear(0);
    }

    scale = ScaleSlot(kind, slot);

    /* Slot recycled into a different character: its scale was not meant for
     * this one. */
    if (kind == PcPick_Npc && scale != NULL && *scale != Q12(1.0f) &&
        s_npcScaleChara[slot] != chara->model.charaId)
    {
        *scale = Q12(1.0f);
    }

    /* Published for the skeleton draw rather than multiplied into bone 0.
     *
     * Bone 0 only reaches bones the ANM parented. Anim_BoneInit parents
     * indices 1..boneCount-1, but the SKELETON is built from the LM, which can
     * carry more models than the ANM has bones (BoneHierarchy_MultiModel).
     * Those extra parts keep super == NULL, so they are not under bone 0 and a
     * bone-0 scale never reached them: the Incubator scaled from the waist down
     * and bosses whose body is mostly such parts did not appear to scale.
     *
     * Scaling at the draw also means nothing is left in the bone coords, so
     * there is no compounding and no need to track what was baked in -- which
     * is what the frozen-NPC ratio dance here existed for. A frozen NPC is not
     * re-posed, but it is still drawn, so it scales like anything else. */
    {
        extern s32 g_PcCharaDrawScale;
        g_PcCharaDrawScale = (scale != NULL) ? *scale : Q12(1.0f);
    }

    if (s_pendState != 1)
        return;

    /* Hit test: the body's projected vertical span, widened into a box about
     * half as wide as it is tall. The two span points come from the collision
     * box, whose sign convention differs per character, so take the min/max
     * rather than assuming which way is up. */
    {
        q19_12 yA  = chara->position.vy + chara->collision.box.top;
        q19_12 yB  = chara->position.vy + chara->collision.box.height;
        q19_12 yLo = (yA < yB) ? yA : yB;
        q19_12 yHi = (yA < yB) ? yB : yA;
        float  x0, y0, x1, y1;
        float  cx, cy, curX, curY;
        float  halfH, halfW;
        s32    d0, d1, depth;

        /* No usable collision span (some scripted actors): assume a body about
         * 1.6 units tall standing on the character's own position. */
        if (yHi - yLo < Q12(0.25f))
        {
            yLo = chara->position.vy - Q12(1.6f);
            yHi = chara->position.vy;
        }

        if (!ProjectWorld(chara->position.vx, yLo, chara->position.vz, &x0, &y0, &d0))
            return;
        if (!ProjectWorld(chara->position.vx, yHi, chara->position.vz, &x1, &y1, &d1))
            return;
        if (!CursorPrimPos(s_pendX, s_pendY, &curX, &curY))
            return;

        cx    = (x0 + x1) * 0.5f;
        cy    = (y0 + y1) * 0.5f;
        halfH = (y0 > y1) ? (y0 - y1) * 0.5f : (y1 - y0) * 0.5f;

        /* Width from the body cylinder rather than a fraction of the height:
         * a Groaner is a low, wide dog, and a height-derived box made it a
         * sliver that took several clicks to hit. */
        {
            q19_12 r = chara->collision.cylinder.field_2;
            float  rx, ry;
            s32    rd;

            if (r < 0)
                r = -r;
            if (r < Q12(0.3f))
                r = Q12(0.3f);

            if (ProjectWorld(chara->position.vx + r, (yLo + yHi) / 2, chara->position.vz,
                             &rx, &ry, &rd))
            {
                halfW = (rx > cx) ? (rx - cx) : (cx - rx);
            }
            else
            {
                halfW = halfH * 0.6f;
            }
        }

        /* A few units of slop so a near-miss still counts. */
        halfH += 6.0f;
        halfW += 6.0f;
        if (halfH < 10.0f)
            halfH = 10.0f;
        if (halfW < 10.0f)
            halfW = 10.0f;

        if (curX < cx - halfW || curX > cx + halfW ||
            curY < cy - halfH || curY > cy + halfH)
        {
            return;
        }

        /* Nearest hit wins when two bodies overlap on screen. */
        depth = (d0 < d1) ? d0 : d1;
        if (s_bestKind != PcPick_None && depth >= s_bestDepth)
            return;

        s_bestKind    = kind;
        s_bestSlot    = slot;
        s_bestCharaId = chara->model.charaId;
        s_bestDepth   = depth;
    }
}

void Pc_Pick_WorldObjectPreDraw(const void* worldObject, void* coordPtr)
{
    const s_WorldObject* obj   = (const s_WorldObject*)worldObject;
    GsCOORDINATE2*       coord = (GsCOORDINATE2*)coordPtr;
    s32                  x, y, z;

    if (obj == NULL || coord == NULL)
        return;
    if ((!s_anyScaled || s_propCount == 0) && s_pendState != 1)
        return; /* nothing scaled and no click waiting: props cost nothing */

    x = obj->positionX;
    y = obj->positionY;
    z = obj->positionZ;

    if (s_propCount != 0)
    {
        s_PickProp* pr = PropFind(obj->model, x, y, z, 0);
        if (pr != NULL && pr->scale != Q12(1.0f))
        {
            int i, j;
            for (i = 0; i < 3; i++)
                for (j = 0; j < 3; j++)
                    coord->coord.m[j][i] = (s16)Q12_MULT_PRECISE(pr->scale, coord->coord.m[j][i]);
            coord->flg = 0;
        }
    }

    if (s_pendState != 1)
        return;

    /* Positions here are Q8; the projection wants Q12. A prop has no
     * collision shape to size a box from, so use a fixed world-space reach
     * about its origin: click on or near the thing. */
    {
        q19_12 wx = x << 4;
        q19_12 wy = y << 4;
        q19_12 wz = z << 4;
        float  ox, oy, ex, ey, curX, curY, reach;
        s32    d0, d1;

        if (!ProjectWorld(wx, wy, wz, &ox, &oy, &d0))
            return;
        if (!ProjectWorld(wx + Q12(0.7f), wy, wz, &ex, &ey, &d1))
            return;
        if (!CursorPrimPos(s_pendX, s_pendY, &curX, &curY))
            return;

        reach = (ex > ox) ? (ex - ox) : (ox - ex);
        if (reach < 8.0f)
            reach = 8.0f;

        if (curX < ox - reach || curX > ox + reach ||
            curY < oy - reach || curY > oy + reach)
        {
            return;
        }

        /* A character under the same click wins ties: it is the more likely
         * target, and props are everywhere. */
        if (s_bestKind != PcPick_None && d0 >= s_bestDepth)
            return;

        s_bestKind  = PcPick_Prop;
        s_bestSlot  = -1;
        s_bestDepth = d0;
        s_bestPropModel = obj->model;
        s_bestPropX = x;
        s_bestPropY = y;
        s_bestPropZ = z;
    }
}

void Pc_Pick_FrameEnd(void)
{
    char what[64];
    char line[160];

    if (s_pendState == 1)
    {
        /* One frame of drawing has now had its shot at the click. */
        s_pendState = 2;
        return;
    }

    if (s_pendState != 2)
        return;

    s_pendState = 0;

    if (s_bestKind == PcPick_None)
    {
        Pc_Console_Print("nothing there to select");
        return;
    }

    s_selKind    = s_bestKind;
    s_selSlot    = s_bestSlot;
    s_selCharaId = s_bestCharaId;
    if (s_bestKind == PcPick_Prop)
    {
        s_selPropModel = s_bestPropModel;
        s_selPropX     = s_bestPropX;
        s_selPropY     = s_bestPropY;
        s_selPropZ     = s_bestPropZ;
    }

    DescribeInto(s_selKind, s_selSlot, s_selCharaId, what, sizeof(what));

    if (s_selKind == PcPick_Prop)
    {
        snprintf(line, sizeof(line), "selected %s  scale %.2f",
                 what, Pc_Pick_GetScale() / 4096.0f);
    }
    else
    {
        s_SubCharacter* c = (s_selKind == PcPick_Player)
                          ? &g_SysWork.playerWork.player
                          : &g_SysWork.npcs[s_selSlot];

        snprintf(line, sizeof(line),
                 "selected %s  hp %.1f  pos (%.1f, %.1f, %.1f)  scale %.2f",
                 what, c->health / 4096.0f,
                 c->position.vx / 4096.0f, c->position.vy / 4096.0f, c->position.vz / 4096.0f,
                 Pc_Pick_GetScale() / 4096.0f);
    }

    Pc_Console_Print(line);
}

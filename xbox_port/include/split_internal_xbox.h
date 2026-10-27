/*
 * split_internal_xbox.h - what the split_*_xbox.c files call in each other.
 * Shared game code only ever calls the hooks in split_xbox.h.
 */
#ifndef SPLIT_INTERNAL_XBOX_H
#define SPLIT_INTERNAL_XBOX_H

#include <stdint.h>
#include "gpu_nv2a.h"

/* split_render_xbox.c */
void           SplitRender_FrameStart(void);
void           SplitRender_PubSwap(void);
void           SplitRender_Reset(void);
void           SplitRender_MarkBuilt(void);
void           SplitRender_CapturePub(int view);
void           SplitRender_PublishPub(int view);
const VECTOR3* SplitRender_ViewPos(int view);
void           SplitRender_CaptureEnv2(void);
int            SplitRender_View2Begin(void);
void           SplitRender_View2End(void);
int            SplitRender_InView2(void);
void           SplitRender_ViewRect(int layout, int idx, GpuXboxView* v);
int            SplitRender_Present(uintptr_t pktLo, uintptr_t pktHi, unsigned int clearArgb);

/* split_xbox.c: the two players, wherever each one currently is (in the
 * g_SysWork slots, or in the side buffer). */
struct _SubCharacter;
int                    Split_SlotOwner(void);           /* player in the g_SysWork slots */
int                    Split_WorkOwner(void);           /* ...whose playerWork is there (differs inside a monster's update) */
int                    Split_ViewOwner(void);           /* whose camera view 1 was built from this frame */
struct _SubCharacter*  Split_PlayerChara(int p);
struct _SubCharacter*  Split_SideChara(void);           /* the player NOT in the slots */
int                    Split_P2InWorld(void);           /* Player 2 is in the world (not parked) */
int                    Split_PlayerTargetable(int p);   /* alive, in the world, not respawning */
int                    Split_PlayerNpcIdx(int p, int i);/* g_SysWork.npcIdxs[i] for that player */
unsigned short*        Split_PlayerLatch(int p);        /* that player's charaGroupFlags[3] */
int                    Split_PlayerTargetNpc(int p);    /* that player's targetNpcIdx */
void                   Split_SwapNpcSet(void);          /* exchange the AI-visible player state */

/* split_npc_xbox.c */
void SplitNpc_Reset(void);

/* split_audio_xbox.c */
void SplitAudio_Reset(void);
void SplitAudio_FrameTick(void);
void SplitAudio_ListenerCapture(int k);

#endif /* SPLIT_INTERNAL_XBOX_H */

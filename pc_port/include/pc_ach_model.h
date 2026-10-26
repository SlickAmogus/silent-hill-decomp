/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_ACH_MODEL_H
#define PC_ACH_MODEL_H

#include "pc_retroachievements.h"

/* Renderer-free model for the native achievements screen: the sorted list and
 * the LRU cache of hires pool slots holding badge art. No game headers. */

#define ACH_MAX 128
typedef struct { PcRaAch items[ACH_MAX]; int count; int unlocked; unsigned pts, ptsTotal; } AchList;
/* Sorts: unlocked first, then points ascending, then title. */
void AchList_Build(AchList* l, const PcRaAch* src, int n);
/* -1 when no entry has this badge and title. */
int  AchList_IndexOf(const AchList* l, const char* badge, const char* title);

#define ACH_BADGE_SLOTS 8
/* Free pool range is 448..509; the minimap owns 510 and 511. */
#define ACH_BADGE_SLOT_FIRST 500
typedef struct { char badge[ACH_BADGE_SLOTS][16]; unsigned age[ACH_BADGE_SLOTS]; int ready[ACH_BADGE_SLOTS]; unsigned tick; } AchBadgeCache;
/* Slot index or -1; a hit refreshes the slot's age. */
int  AchBadgeCache_Find(AchBadgeCache* c, const char* badge);
/* Least recently used slot (an empty one first), renamed, ready = 0. */
int  AchBadgeCache_Claim(AchBadgeCache* c, const char* badge);

#endif

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_ach_model.h"

#include <stdlib.h>
#include <string.h>

static int ach_cmp(const void* a, const void* b)
{
    const PcRaAch* x = (const PcRaAch*)a;
    const PcRaAch* y = (const PcRaAch*)b;

    if (!x->unlocked != !y->unlocked)
        return x->unlocked ? -1 : 1;
    if (x->points != y->points)
        return (x->points < y->points) ? -1 : 1;
    return strcmp(x->title, y->title);
}

void AchList_Build(AchList* l, const PcRaAch* src, int n)
{
    int i;

    if (n < 0) n = 0;
    if (n > ACH_MAX) n = ACH_MAX;
    l->count    = n;
    l->unlocked = 0;
    l->pts      = 0;
    l->ptsTotal = 0;
    if (n > 0) memcpy(l->items, src, (size_t)n * sizeof(l->items[0]));
    qsort(l->items, (size_t)n, sizeof(l->items[0]), ach_cmp);
    for (i = 0; i < n; i++)
    {
        l->ptsTotal += l->items[i].points;
        if (l->items[i].unlocked)
        {
            l->unlocked++;
            l->pts += l->items[i].points;
        }
    }
}

int AchList_IndexOf(const AchList* l, const char* badge, const char* title)
{
    int i;

    for (i = 0; i < l->count; i++)
    {
        if (strcmp(l->items[i].badge, badge) == 0 && strcmp(l->items[i].title, title) == 0)
            return i;
    }
    return -1;
}

int AchBadgeCache_Find(AchBadgeCache* c, const char* badge)
{
    int i;

    if (!badge || !badge[0]) return -1;
    for (i = 0; i < ACH_BADGE_SLOTS; i++)
    {
        if (strcmp(c->badge[i], badge) == 0)
        {
            c->age[i] = ++c->tick;
            return i;
        }
    }
    return -1;
}

int AchBadgeCache_Claim(AchBadgeCache* c, const char* badge)
{
    int i, best = 0;

    for (i = 0; i < ACH_BADGE_SLOTS; i++)
    {
        if (!c->badge[i][0])
        {
            best = i;
            break;
        }
        if (c->age[i] < c->age[best]) best = i;
    }
    memset(c->badge[best], 0, sizeof(c->badge[best]));
    strncpy(c->badge[best], badge ? badge : "", sizeof(c->badge[best]) - 1);
    c->age[best]   = ++c->tick;
    c->ready[best] = 0;
    return best;
}

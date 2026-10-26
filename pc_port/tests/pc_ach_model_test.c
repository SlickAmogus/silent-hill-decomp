/* pc_ach_model_test.c — achievements screen model (list order, refresh,
 * badge slot cache). */
/* NDEBUG is set by the RelWithDebInfo build; this test IS its assertions. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_ach_model.h"

static PcRaAch mk(const char* title, unsigned points, int unlocked)
{
    PcRaAch a;
    memset(&a, 0, sizeof a);
    snprintf(a.title, sizeof a.title, "%s", title);
    snprintf(a.badge, sizeof a.badge, "%s-badge", title);
    a.points   = points;
    a.unlocked = unlocked;
    return a;
}

static AchList s_list;

static void test_ach_sort(void)
{
    PcRaAch src[4];
    src[0] = mk("B", 10, 0);
    src[1] = mk("A", 5, 1);
    src[2] = mk("C", 1, 1);
    src[3] = mk("D", 3, 0);
    AchList_Build(&s_list, src, 4);
    assert(s_list.count == 4);
    assert(!strcmp(s_list.items[0].title, "C"));
    assert(!strcmp(s_list.items[1].title, "A"));
    assert(!strcmp(s_list.items[2].title, "D"));
    assert(!strcmp(s_list.items[3].title, "B"));
    assert(s_list.unlocked == 2);
    assert(s_list.pts == 6);
    assert(s_list.ptsTotal == 19);
}

static void test_ach_refresh_keeps_selection(void)
{
    PcRaAch src[4];
    int     sel;
    src[0] = mk("B", 10, 0);
    src[1] = mk("A", 5, 1);
    src[2] = mk("C", 1, 1);
    src[3] = mk("D", 3, 0);
    AchList_Build(&s_list, src, 4);
    sel = AchList_IndexOf(&s_list, "D-badge", "D");
    assert(sel == 2);

    src[3].unlocked = 1;
    AchList_Build(&s_list, src, 4);
    /* Unlocked block is now C(1), D(3), A(5). */
    assert(AchList_IndexOf(&s_list, "D-badge", "D") == 1);
    assert(!strcmp(s_list.items[1].title, "D") && s_list.items[1].unlocked);
    assert(s_list.unlocked == 3 && s_list.pts == 9);

    AchList_Build(&s_list, src, 3);
    assert(AchList_IndexOf(&s_list, "D-badge", "D") == -1);
}

static void test_badge_cache_lru(void)
{
    AchBadgeCache c;
    char          name[16];
    int           slots[ACH_BADGE_SLOTS];
    int           i, s;

    memset(&c, 0, sizeof c);
    for (i = 0; i < ACH_BADGE_SLOTS; i++)
    {
        snprintf(name, sizeof name, "b%d", i);
        slots[i] = AchBadgeCache_Claim(&c, name);
        assert(slots[i] >= 0 && slots[i] < ACH_BADGE_SLOTS);
        assert(c.ready[slots[i]] == 0);
        c.ready[slots[i]] = 1;
    }
    for (i = 0; i < ACH_BADGE_SLOTS; i++)
    {
        snprintf(name, sizeof name, "b%d", i);
        assert(AchBadgeCache_Find(&c, name) == slots[i]);
    }
    assert(AchBadgeCache_Find(&c, "b0") == slots[0]);
    /* Find above refreshed every name in order, so b0 again leaves b1 oldest. */
    s = AchBadgeCache_Claim(&c, "b8");
    assert(s == slots[1]);
    assert(c.ready[s] == 0);
    assert(AchBadgeCache_Find(&c, "b1") == -1);
    assert(AchBadgeCache_Find(&c, "b8") == s);
    assert(AchBadgeCache_Find(&c, "b0") == slots[0]);
}

static void test_badge_cache_lru_plain(void)
{
    AchBadgeCache c;
    char          name[16];
    int           slots[ACH_BADGE_SLOTS];
    int           i, s;

    memset(&c, 0, sizeof c);
    for (i = 0; i < ACH_BADGE_SLOTS; i++)
    {
        snprintf(name, sizeof name, "n%d", i);
        slots[i] = AchBadgeCache_Claim(&c, name);
    }
    assert(AchBadgeCache_Find(&c, "n0") == slots[0]);
    s = AchBadgeCache_Claim(&c, "n8");
    assert(s == slots[1]);
    assert(AchBadgeCache_Find(&c, "n1") == -1);
}

int main(void)
{
    test_ach_sort();
    test_ach_refresh_keeps_selection();
    test_badge_cache_lru();
    test_badge_cache_lru_plain();
    printf("pc_ach_model_test: all passed\n");
    return 0;
}

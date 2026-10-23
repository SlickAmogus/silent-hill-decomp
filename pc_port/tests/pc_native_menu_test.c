/* pc_native_menu_test.c — renderer-free native menu core (scroll, text fitting,
 * row page, confirm box). */
/* NDEBUG is set by the RelWithDebInfo build; this test IS its assertions. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_native_menu.h"

static int m8(const char* s) { return 8 * (int)strlen(s); }

static void test_scroll_follow(void)
{
    NmScroll s; NmScroll_Init(&s, 10, 4);
    for (int i = 0; i < 5; i++) NmScroll_Move(&s, +1, 0);
    assert(s.sel == 5 && s.top == 2);
    NmScroll_Move(&s, +10, 0); assert(s.sel == 9 && s.top == 6);
    NmScroll_Move(&s, +1, 1);  assert(s.sel == 0 && s.top == 0);
    assert(!NmScroll_CanUp(&s) && NmScroll_CanDown(&s));
}
static void test_scroll_wheel(void)
{
    NmScroll s; NmScroll_Init(&s, 10, 4);
    NmScroll_Wheel(&s, -3); assert(s.top == 3 && s.sel == 3);
    NmScroll_Wheel(&s, -99); assert(s.top == 6);
    NmScroll_SetCount(&s, 5); assert(s.top == 1 && s.sel <= 4);
}
static void test_scroll_fits(void)
{
    NmScroll s; NmScroll_Init(&s, 3, 5);
    assert(!NmScroll_CanUp(&s) && !NmScroll_CanDown(&s));
}
static void test_sanitize_ascii(void)
{
    char o[64];
    Nm_SanitizeGlyphs("What a Nightmare", o, sizeof o, 1); assert(!strcmp(o, "WHAT_A_NIGHTMARE"));
    Nm_SanitizeGlyphs("Hammerin' Harry", o, sizeof o, 1);  assert(!strcmp(o, "HAMMERIN'_HARRY"));
    Nm_SanitizeGlyphs("Say \"hi\" 100% #1!", o, sizeof o, 0); assert(!strcmp(o, "Say_'hi'_100_1!"));
}
static void test_sanitize_utf8(void)
{
    char o[64];
    Nm_SanitizeGlyphs("Caf\xC3\xA9 \xE2\x80\x93 done\xE2\x80\xA6", o, sizeof o, 0);
    assert(!strcmp(o, "Cafe_-_done..."));
    Nm_SanitizeGlyphs("It\xE2\x80\x99s \xF0\x9F\x98\x80ok", o, sizeof o, 0);
    assert(!strcmp(o, "It's_ok"));
}
static void test_sanitize_small_buffer(void)
{
    char o[6]; memset(o, 'X', sizeof o);
    Nm_SanitizeGlyphs("ABCDEFGHIJ", o, sizeof o, 0); assert(!strcmp(o, "ABCDE"));
}
static void test_fit_ellipsis(void)
{
    char o[64];
    assert(Nm_FitEllipsis("ABCDEFGHIJ", o, sizeof o, 48, m8) == 1 && !strcmp(o, "ABC..."));
    assert(Nm_FitEllipsis("ABC", o, sizeof o, 48, m8) == 0 && !strcmp(o, "ABC"));
    Nm_FitEllipsis("Use_the_Lion", o, sizeof o, 80, m8); assert(!strcmp(o, "Use_the..."));
    Nm_FitEllipsis("Use_the_Lion", o, sizeof o, 72, m8); assert(!strcmp(o, "Use_th..."));
    Nm_FitEllipsis("Use_the_Lion", o, sizeof o, 56, m8); assert(!strcmp(o, "Use..."));
}
static void test_wrap(void)
{
    char l[4][NM_LINE_MAX];
    assert(Nm_WrapLines("Complete_the_introduction_chapter", l, 4, 120, m8) == 3);
    assert(!strcmp(l[0], "Complete_the") && !strcmp(l[1], "introduction") && !strcmp(l[2], "chapter"));
    assert(Nm_WrapLines("aa_bb_cc_dd_ee", l, 2, 40, m8) == 2 && !strcmp(l[1], "cc..."));
    assert(Nm_WrapLines("ABCDEFGHIJKLMNOP", l, 4, 40, m8) == 4 && !strcmp(l[0], "ABCDE"));
}
static void test_date(void)
{
    char o[32]; Nm_FormatDate(31, 12, 2026, 18, 19, o, sizeof o); assert(!strcmp(o, "31._12._2026._18:19"));
    Nm_FormatDate(1, 2, 2027, 3, 4, o, sizeof o); assert(!strcmp(o, "01._02._2027._03:04"));
}

static int g_steps, g_lastDir, g_acts;
static void pg_step(void* ctx, int dir) { (void)ctx; g_steps++; g_lastDir = dir; }
static void pg_act(void* ctx) { (void)ctx; g_acts++; }

static void test_page(void)
{
    static const NmRow rows[3] = {
        { "Info",   NM_ROW_INFO,   NULL, NULL,    NULL   },
        { "Value",  NM_ROW_VALUE,  NULL, pg_step, NULL   },
        { "Action", NM_ROW_ACTION, NULL, NULL,    pg_act },
    };
    NmPage p = { "Page", rows, 3, NULL, {0} };
    NmInput in;
    g_steps = g_acts = 0;
    NmPage_Init(&p, 3);
    assert(p.scroll.sel == 1);

    memset(&in, 0, sizeof in); in.right = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_STEPPED && g_steps == 1 && g_lastDir == +1);
    memset(&in, 0, sizeof in); in.left = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_STEPPED && g_steps == 2 && g_lastDir == -1);
    memset(&in, 0, sizeof in); in.confirm = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_STEPPED && g_steps == 3 && g_lastDir == +1);

    memset(&in, 0, sizeof in); in.down = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_MOVED && p.scroll.sel == 2);
    memset(&in, 0, sizeof in); in.confirm = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_ACTIVATED && g_acts == 1);

    memset(&in, 0, sizeof in); in.up = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_MOVED && p.scroll.sel == 1);
    assert(NmPage_Update(&p, &in, -1) == NM_EV_MOVED && p.scroll.sel == 2);
    memset(&in, 0, sizeof in); in.down = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_MOVED && p.scroll.sel == 1);

    memset(&in, 0, sizeof in);
    assert(NmPage_Update(&p, &in, 0) == NM_EV_NONE && p.scroll.sel == 1);
    assert(NmPage_Update(&p, &in, 2) == NM_EV_MOVED && p.scroll.sel == 2);
    in.mouseLeft = 1;
    assert(NmPage_Update(&p, &in, 2) == NM_EV_ACTIVATED && g_acts == 2);

    memset(&in, 0, sizeof in); in.cancel = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_BACK);
    memset(&in, 0, sizeof in); in.mouseRight = 1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_BACK);
}
static void test_page_wheel_info(void)
{
    static const NmRow rows[5] = {
        { "A0", NM_ROW_ACTION, NULL, NULL, pg_act },
        { "I1", NM_ROW_INFO,   NULL, NULL, NULL   },
        { "I2", NM_ROW_INFO,   NULL, NULL, NULL   },
        { "A3", NM_ROW_ACTION, NULL, NULL, pg_act },
        { "A4", NM_ROW_ACTION, NULL, NULL, pg_act },
    };
    NmPage p = { "Page", rows, 5, NULL, {0} };
    NmInput in;
    NmPage_Init(&p, 2);
    assert(p.scroll.sel == 0 && p.scroll.top == 0);

    memset(&in, 0, sizeof in); in.wheel = -1;
    assert(NmPage_Update(&p, &in, -1) == NM_EV_MOVED && p.scroll.sel == 3);
    assert(p.scroll.top <= 3 && 3 < p.scroll.top + p.scroll.visible);

    memset(&in, 0, sizeof in); in.wheel = +1;
    NmPage_Update(&p, &in, -1);
    assert(p.scroll.sel >= p.scroll.top && p.scroll.sel < p.scroll.top + p.scroll.visible);
    assert(rows[p.scroll.sel].kind != NM_ROW_INFO);
}
static void test_confirm(void)
{
    NmConfirm c; NmInput in = {0}; NmConfirm_Open(&c); assert(c.open && !c.yes);
    in.left = 1; assert(NmConfirm_Update(&c, &in) == NM_CONFIRM_PENDING && c.yes);
    in = (NmInput){0}; in.confirm = 1; assert(NmConfirm_Update(&c, &in) == NM_CONFIRM_YES && !c.open);
    NmConfirm_Open(&c); in = (NmInput){0}; in.cancel = 1; assert(NmConfirm_Update(&c, &in) == NM_CONFIRM_NO);
}

enum { T_LOAD, T_CONT, T_START, T_OPTION, T_ACH, T_EXIT, T_COUNT };
#define TB(e) (1u << (e))

static void test_title_layout_retail_full(void)
{
    int order[T_COUNT];
    int n = Nm_CompactRows(TB(T_LOAD) | TB(T_CONT) | TB(T_START) | TB(T_OPTION) | TB(T_EXIT), T_COUNT, order);
    assert(n == 5 && Nm_SlotBase(n, 5) == 0);
    assert(order[0] == T_LOAD && order[1] == T_CONT && order[2] == T_START &&
           order[3] == T_OPTION && order[4] == T_EXIT);
}
static void test_title_layout_retail_nosave(void)
{
    int order[T_COUNT];
    int n = Nm_CompactRows(TB(T_START) | TB(T_OPTION) | TB(T_EXIT), T_COUNT, order);
    assert(n == 3 && Nm_SlotBase(n, 5) == 2);
    assert(order[0] == T_START && 184 + (Nm_SlotBase(n, 5) + 0) * 20 == 224);
}
static void test_title_layout_nosave_ach(void)
{
    int order[T_COUNT];
    int n = Nm_CompactRows(TB(T_START) | TB(T_OPTION) | TB(T_ACH) | TB(T_EXIT), T_COUNT, order);
    assert(n == 4 && Nm_SlotBase(n, 5) == 1);
    assert(order[0] == 2 && order[1] == 3 && order[2] == 4 && order[3] == 5);
}
static void test_title_layout_scroll(void)
{
    int      order[T_COUNT];
    NmScroll s;
    int      n = Nm_CompactRows(0x3Fu, T_COUNT, order), base;
    assert(n == 6);
    base = Nm_SlotBase(n, 5);
    assert(base == 0);
    NmScroll_Init(&s, n, n < 5 ? n : 5);
    NmScroll_Select(&s, 5);
    assert(order[5] == T_EXIT && s.top == 1);
    assert(Nm_SlotToIndex(4, base, s.top, n) == 5);
    assert(order[Nm_SlotToIndex(0, base, s.top, n)] == T_CONT);
}
static void test_title_slot_to_index_bounds(void)
{
    assert(Nm_SlotToIndex(1, 2, 0, 3) == -1);
    assert(Nm_SlotToIndex(2, 2, 0, 3) == 0);
    assert(Nm_SlotToIndex(4, 2, 0, 3) == 2);
    assert(Nm_SlotToIndex(5, 2, 0, 3) == -1);
    assert(Nm_SlotToIndex(-1, 0, 1, 6) == -1);
}

int main(void)
{
    test_scroll_follow();
    test_scroll_wheel();
    test_scroll_fits();
    test_sanitize_ascii();
    test_sanitize_utf8();
    test_sanitize_small_buffer();
    test_fit_ellipsis();
    test_wrap();
    test_date();
    test_page();
    test_page_wheel_info();
    test_confirm();
    test_title_layout_retail_full();
    test_title_layout_retail_nosave();
    test_title_layout_nosave_ach();
    test_title_layout_scroll();
    test_title_slot_to_index_bounds();
    printf("pc_native_menu_test: all passed\n");
    return 0;
}

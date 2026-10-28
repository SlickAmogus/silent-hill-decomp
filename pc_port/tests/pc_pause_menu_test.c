/* pc_pause_menu_test.c — renderer-free pause menu model. */
/* NDEBUG is set by the RelWithDebInfo build; this test IS its assertions. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_pause_menu_core.h"

static NmInput none(void) { NmInput i; memset(&i, 0, sizeof i); return i; }

/* Opens and runs the opening tick, which ignores the pointer. */
static void open_settled(PmMenu* m, PmBlock block)
{
    NmInput in = none();
    PmMenu_Open(m, block);
    assert(PmMenu_Update(m, &in, -1, -1) == PM_ACT_NONE);
}

static void test_open_resets(void)
{
    PmMenu m; PmMenu_Open(&m, PM_BLOCK_NONE);
    assert(m.sel == PM_ROW_CONTINUE && !m.confirm.open);
    assert(PmMenu_EnabledMask(&m) == 0x1F);
}

static void test_blocked_mask(void)
{
    PmMenu m;
    PmMenu_Open(&m, PM_BLOCK_BOSS);  assert(PmMenu_EnabledMask(&m) == 0x19);
    PmMenu_Open(&m, PM_BLOCK_RANDO); assert(PmMenu_EnabledMask(&m) == 0x19);
}

static void test_down_skips_disabled(void)
{
    PmMenu m; NmInput in = none();
    PmMenu_Open(&m, PM_BLOCK_BOSS);
    in.down = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_MOVED && m.sel == PM_ROW_OPTION);
    in = none(); in.up = 1; m.sel = PM_ROW_CONTINUE;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_MOVED && m.sel == PM_ROW_EXIT);
    in = none(); in.down = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_MOVED && m.sel == PM_ROW_CONTINUE);
}

static void test_confirm_rows(void)
{
    static const PmAction exp[PM_ROW_COUNT] = { PM_ACT_RESUME, PM_ACT_SAVE, PM_ACT_LOAD,
                                                PM_ACT_OPTION, PM_ACT_EXIT_ASK };
    for (int r = 0; r < PM_ROW_COUNT; r++) {
        PmMenu m; NmInput in = none();
        PmMenu_Open(&m, PM_BLOCK_NONE);
        m.sel = r; in.confirm = 1;
        assert(PmMenu_Update(&m, &in, -1, -1) == exp[r]);
        if (r == PM_ROW_EXIT) assert(m.confirm.open == 1 && m.confirm.yes == 0);
    }
}

static void test_cancel_resumes(void)
{
    for (int r = 0; r < PM_ROW_COUNT; r++) {
        PmMenu m; NmInput in = none();
        PmMenu_Open(&m, PM_BLOCK_NONE);
        m.sel = r; in.cancel = 1;
        assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_RESUME);
    }
}

static void test_hover_and_click(void)
{
    PmMenu m; NmInput in = none();
    open_settled(&m, PM_BLOCK_NONE);
    in.mouseMoved = 1;
    assert(PmMenu_Update(&m, &in, PM_ROW_LOAD, -1) == PM_ACT_MOVED && m.sel == PM_ROW_LOAD);
    in.mouseLeft = 1;
    assert(PmMenu_Update(&m, &in, PM_ROW_LOAD, -1) == PM_ACT_LOAD);
}

static void test_idle_pointer_does_not_fight_pad(void)
{
    PmMenu m; NmInput in = none();
    PmMenu_Open(&m, PM_BLOCK_NONE);
    m.sel = PM_ROW_OPTION;
    assert(PmMenu_Update(&m, &in, PM_ROW_LOAD, -1) == PM_ACT_NONE && m.sel == PM_ROW_OPTION);
}

static void test_disabled_hover_ignored(void)
{
    PmMenu m; NmInput in = none();
    open_settled(&m, PM_BLOCK_BOSS);
    in.mouseLeft = 1; in.mouseMoved = 1;
    assert(PmMenu_Update(&m, &in, PM_ROW_SAVE, -1) == PM_ACT_NONE);
    assert(m.sel == PM_ROW_CONTINUE);
}

static void test_opening_tick_ignores_pointer(void)
{
    PmMenu m; NmInput in;
    int pass;

    /* Second pass: shown again after coming back from Save/Load/Option. */
    for (pass = 0; pass < 2; pass++) {
        PmMenu_Open(&m, PM_BLOCK_NONE);
        in = none(); in.mouseOk = 1; in.mouseMoved = 1; in.mouseLeft = 1;
        assert(PmMenu_Update(&m, &in, PM_ROW_OPTION, -1) == PM_ACT_NONE);
        assert(m.sel == PM_ROW_CONTINUE);
        in = none(); in.mouseOk = 1; in.mouseMoved = 1;
        assert(PmMenu_Update(&m, &in, PM_ROW_OPTION, -1) == PM_ACT_MOVED);
        assert(m.sel == PM_ROW_OPTION);
    }

    PmMenu_Open(&m, PM_BLOCK_NONE);
    in = none(); in.confirm = 1;
    assert(PmMenu_Update(&m, &in, PM_ROW_OPTION, -1) == PM_ACT_RESUME);
    PmMenu_Open(&m, PM_BLOCK_NONE);
    in = none(); in.cancel = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_RESUME);
}

static void test_right_click_cancels(void)
{
    PmMenu m; NmInput in;

    open_settled(&m, PM_BLOCK_NONE);
    m.sel = PM_ROW_LOAD;
    in = none(); in.mouseRight = 1;
    assert(PmMenu_Update(&m, &in, PM_ROW_LOAD, -1) == PM_ACT_RESUME);

    open_settled(&m, PM_BLOCK_NONE);
    m.sel = PM_ROW_EXIT;
    in = none(); in.confirm = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_EXIT_ASK);
    m.confirm.yes = 1;
    in = none(); in.mouseRight = 1;
    assert(PmMenu_Update(&m, &in, -1, 0) == PM_ACT_EXIT_NO);
    assert(!m.confirm.open && m.sel == PM_ROW_EXIT);
}

static void ask_exit(PmMenu* m)
{
    NmInput in = none();
    PmMenu_Open(m, PM_BLOCK_NONE);
    m->sel = PM_ROW_EXIT;
    in.confirm = 1;
    assert(PmMenu_Update(m, &in, -1, -1) == PM_ACT_EXIT_ASK);
}

static void test_exit_confirm(void)
{
    PmMenu m; NmInput in;

    ask_exit(&m);
    in = none(); in.right = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_NONE && m.confirm.yes == 1);
    in = none(); in.confirm = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_EXIT_YES);

    ask_exit(&m);
    in = none(); in.confirm = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_EXIT_NO);
    assert(!m.confirm.open && m.sel == PM_ROW_EXIT);

    ask_exit(&m);
    in = none(); in.cancel = 1;
    assert(PmMenu_Update(&m, &in, -1, -1) == PM_ACT_EXIT_NO);

    ask_exit(&m);
    in = none(); in.mouseLeft = 1;
    assert(PmMenu_Update(&m, &in, -1, 0) == PM_ACT_EXIT_YES);

    ask_exit(&m);
    in = none(); in.up = 1; PmMenu_Update(&m, &in, -1, -1);
    in = none(); in.down = 1; PmMenu_Update(&m, &in, -1, -1);
    assert(m.sel == PM_ROW_EXIT && m.confirm.open);
}

static void test_apply_hover(void)
{
    NmConfirm c; NmInput in, out;

    NmConfirm_Open(&c);
    in = none(); in.mouseMoved = 1;
    out = PmConfirm_ApplyHover(&c, &in, 0);
    assert(c.yes == 1 && !out.confirm);

    in = none(); in.mouseLeft = 1;
    out = PmConfirm_ApplyHover(&c, &in, 1);
    assert(c.yes == 0 && out.confirm);

    in = none(); in.mouseLeft = 1; in.mouseMoved = 1;
    c.yes = 1;
    out = PmConfirm_ApplyHover(&c, &in, -1);
    assert(c.yes == 1 && !out.confirm);

    in = none(); in.mouseRight = 1;
    out = PmConfirm_ApplyHover(&c, &in, -1);
    assert(out.cancel && NmConfirm_Update(&c, &out) == NM_CONFIRM_NO);
}

static void test_reason(void)
{
    assert(!strcmp(PmBlock_Reason(PM_BLOCK_BOSS), "Cannot_save_during_a_boss_fight"));
    assert(!strcmp(PmBlock_Reason(PM_BLOCK_RANDO), "Cannot_save_in_a_randomizer_run"));
    assert(PmBlock_Reason(PM_BLOCK_NONE) == NULL);
}

int main(void)
{
    test_open_resets();
    test_blocked_mask();
    test_down_skips_disabled();
    test_confirm_rows();
    test_cancel_resumes();
    test_hover_and_click();
    test_idle_pointer_does_not_fight_pad();
    test_disabled_hover_ignored();
    test_opening_tick_ignores_pointer();
    test_right_click_cancels();
    test_exit_confirm();
    test_apply_hover();
    test_reason();
    puts("OK");
    return 0;
}

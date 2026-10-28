/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_pause_menu_core.h"

void PmMenu_Open(PmMenu* m, PmBlock block)
{
    m->sel = PM_ROW_CONTINUE;
    m->block = block;
    m->confirm.open = 0;
    m->confirm.yes = 0;
    m->opening = 1;
}

unsigned PmMenu_EnabledMask(const PmMenu* m)
{
    unsigned mask = (1u << PM_ROW_COUNT) - 1u;
    if (m->block != PM_BLOCK_NONE)
        mask &= ~((1u << PM_ROW_SAVE) | (1u << PM_ROW_LOAD));
    return mask;
}

static PmAction Activate(PmMenu* m, int row)
{
    switch (row) {
    case PM_ROW_CONTINUE: return PM_ACT_RESUME;
    case PM_ROW_SAVE:     return PM_ACT_SAVE;
    case PM_ROW_LOAD:     return PM_ACT_LOAD;
    case PM_ROW_OPTION:   return PM_ACT_OPTION;
    case PM_ROW_EXIT:
        NmConfirm_Open(&m->confirm);
        return PM_ACT_EXIT_ASK;
    }
    return PM_ACT_NONE;
}

static int Step(const PmMenu* m, int dir)
{
    unsigned mask = PmMenu_EnabledMask(m);
    int i, row = m->sel;
    for (i = 0; i < PM_ROW_COUNT; i++) {
        row = (row + dir + PM_ROW_COUNT) % PM_ROW_COUNT;
        if (mask & (1u << row)) return row;
    }
    return m->sel;
}

NmInput PmConfirm_ApplyHover(NmConfirm* c, const NmInput* in, int hoverAnswer)
{
    NmInput out = *in;
    if (hoverAnswer == 0 || hoverAnswer == 1) {
        if (in->mouseMoved || in->mouseLeft) c->yes = (hoverAnswer == 0);
        if (in->mouseLeft) out.confirm = 1;
    }
    if (in->mouseRight) out.cancel = 1;
    return out;
}

PmAction PmMenu_Update(PmMenu* m, const NmInput* rawIn, int hoverRow, int hoverAnswer)
{
    unsigned mask = PmMenu_EnabledMask(m);
    NmInput pin = *rawIn;
    const NmInput* in = &pin;
    int hoverOk;

    if (m->opening) {
        m->opening = 0;
        pin.mouseMoved = 0;
        pin.mouseLeft = 0;
    }

    if (m->confirm.open) {
        NmInput c = PmConfirm_ApplyHover(&m->confirm, in, hoverAnswer);
        switch (NmConfirm_Update(&m->confirm, &c)) {
        case NM_CONFIRM_YES: return PM_ACT_EXIT_YES;
        case NM_CONFIRM_NO:  return PM_ACT_EXIT_NO;
        default:             return PM_ACT_NONE;
        }
    }

    if (in->cancel || in->mouseRight) return PM_ACT_RESUME;

    hoverOk = hoverRow >= 0 && hoverRow < PM_ROW_COUNT && (mask & (1u << hoverRow));
    if (hoverOk && in->mouseLeft) {
        m->sel = hoverRow;
        return Activate(m, hoverRow);
    }
    if (in->confirm) return Activate(m, m->sel);

    if (in->up || in->down) {
        int next = Step(m, in->up ? -1 : +1);
        if (next != m->sel) {
            m->sel = next;
            return PM_ACT_MOVED;
        }
        return PM_ACT_NONE;
    }
    if (hoverOk && in->mouseMoved && hoverRow != m->sel) {
        m->sel = hoverRow;
        return PM_ACT_MOVED;
    }
    return PM_ACT_NONE;
}

const char* PmBlock_Reason(PmBlock b)
{
    switch (b) {
    case PM_BLOCK_BOSS:  return "Cannot_save_during_a_boss_fight";
    case PM_BLOCK_RANDO: return "Cannot_save_in_a_randomizer_run";
    default:             return NULL;
    }
}

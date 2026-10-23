/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_native_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- scroll ------------------------------------------------------------ */

static int scroll_max_top(const NmScroll* s)
{
    int m = s->count - s->visible;
    return m > 0 ? m : 0;
}

static void scroll_clamp(NmScroll* s)
{
    if (s->top > scroll_max_top(s)) s->top = scroll_max_top(s);
    if (s->top < 0) s->top = 0;
    if (s->sel >= s->count) s->sel = s->count - 1;
    if (s->sel < 0) s->sel = 0;
}

static void scroll_follow(NmScroll* s)
{
    if (s->sel < s->top) s->top = s->sel;
    if (s->sel >= s->top + s->visible) s->top = s->sel - s->visible + 1;
    scroll_clamp(s);
}

void NmScroll_Init(NmScroll* s, int count, int visible)
{
    s->count   = count < 0 ? 0 : count;
    s->visible = visible < 1 ? 1 : visible;
    s->top     = 0;
    s->sel     = 0;
}

void NmScroll_SetCount(NmScroll* s, int count)
{
    s->count = count < 0 ? 0 : count;
    scroll_clamp(s);
}

int NmScroll_Move(NmScroll* s, int delta, int wrap)
{
    int old = s->sel, n = s->sel + delta;
    if (s->count <= 0) return 0;
    if (wrap) n = ((n % s->count) + s->count) % s->count;
    else if (n < 0) n = 0;
    else if (n >= s->count) n = s->count - 1;
    s->sel = n;
    scroll_follow(s);
    return s->sel != old;
}

void NmScroll_Select(NmScroll* s, int index)
{
    s->sel = index;
    scroll_clamp(s);
    scroll_follow(s);
}

void NmScroll_Wheel(NmScroll* s, int notches)
{
    s->top -= notches;
    scroll_clamp(s);
    if (s->sel < s->top) s->sel = s->top;
    if (s->sel >= s->top + s->visible) s->sel = s->top + s->visible - 1;
    scroll_clamp(s);
}

int NmScroll_CanUp(const NmScroll* s)   { return s->top > 0; }
int NmScroll_CanDown(const NmScroll* s) { return s->top + s->visible < s->count; }

/* ---- text -------------------------------------------------------------- */

/* Latin-1 U+00C0..U+00FF folded to the base letter; "" = dropped. */
static const char* const k_latin1Fold[64] = {
    "A","A","A","A","A","A","","C","E","E","E","E","I","I","I","I",
    "","N","O","O","O","O","O","","O","U","U","U","U","Y","","ss",
    "a","a","a","a","a","a","","c","e","e","e","e","i","i","i","i",
    "","n","o","o","o","o","o","","o","u","u","u","u","y","","y",
};

static const char* fold_codepoint(unsigned cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201C: case 0x201D: return "'";
    case 0x2013: case 0x2014: return "-";
    case 0x2026: return "...";
    }
    if (cp >= 0xC0 && cp <= 0xFF) return k_latin1Fold[cp - 0xC0];
    return "";
}

size_t Nm_SanitizeGlyphs(const char* utf8, char* out, size_t outSize, int upper)
{
    const unsigned char* p = (const unsigned char*)utf8;
    size_t n = 0;
    if (!out || outSize == 0) return 0;
    while (*p) {
        char tmp[4] = {0};
        const char* emit = tmp;
        unsigned c = *p;
        if (c < 0x80) {
            p++;
            if (c == ' ') tmp[0] = '_';
            else if (c == '"') tmp[0] = '\'';
            else if ((c >= 0x27 && c <= 0x7A) || c == '!' || c == '&') tmp[0] = (char)c;
        } else {
            int len = (c >= 0xF0 && c <= 0xF7) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2 && c < 0xE0) ? 2 : 0;
            unsigned cp = 0;
            int ok = len != 0 && c < 0xF8;
            if (ok) {
                cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
                for (int i = 1; i < len; i++) {
                    if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
                    cp = (cp << 6) | (p[i] & 0x3F);
                }
            }
            if (ok) { p += len; emit = fold_codepoint(cp); }
            else    { p++; }
        }
        size_t el = strlen(emit);
        if (el == 0) continue;
        if (emit[0] == '_' && n > 0 && out[n - 1] == '_') continue;
        if (n + el > outSize - 1) break;
        for (size_t i = 0; i < el; i++)
            out[n++] = upper ? (char)toupper((unsigned char)emit[i]) : emit[i];
    }
    out[n] = '\0';
    return n;
}

static int measure_prefix(const char* in, size_t len, NmMeasureFn m)
{
    char tmp[NM_LINE_MAX];
    if (len > NM_LINE_MAX - 1) len = NM_LINE_MAX - 1;
    memcpy(tmp, in, len);
    tmp[len] = '\0';
    return m(tmp);
}

int Nm_FitEllipsis(const char* in, char* out, size_t outSize, int maxWidth, NmMeasureFn m)
{
    size_t n = strlen(in), keep = 0, len;
    int dots = m("..."), cut;
    char buf[NM_LINE_MAX + 4];
    if (!out || outSize == 0) return 0;
    cut = n > NM_LINE_MAX - 1 || m(in) > maxWidth;
    if (!cut) {
        snprintf(out, outSize, "%s", in);
        return 0;
    }
    for (len = 0; len <= n && len <= NM_LINE_MAX - 1; len++) {
        if (measure_prefix(in, len, m) + dots > maxWidth) break;
        keep = len;
    }
    while (keep > 0 && in[keep - 1] == '_') keep--;
    memcpy(buf, in, keep);
    memcpy(buf + keep, "...", 4);
    snprintf(out, outSize, "%s", buf);
    return 1;
}

int Nm_WrapLines(const char* in, char lines[][NM_LINE_MAX], int maxLines, int maxWidth, NmMeasureFn m)
{
    const char* p = in;
    const char* lastStart = in;
    int count = 0;
    if (maxLines <= 0) return 0;
    while (*p == '_') p++;
    while (*p && count < maxLines) {
        const char* ls = p;
        const char* e = ls;
        const char* w = ls;
        size_t len;
        lastStart = ls;
        for (;;) {
            while (*w && *w != '_') w++;
            if ((size_t)(w - ls) < NM_LINE_MAX && measure_prefix(ls, (size_t)(w - ls), m) <= maxWidth) e = w;
            else break;
            if (!*w) break;
            w++;
        }
        if (e == ls) {
            size_t k = 1;
            while (ls[k] && k < NM_LINE_MAX - 1 && measure_prefix(ls, k + 1, m) <= maxWidth) k++;
            e = ls + k;
        }
        len = (size_t)(e - ls);
        memcpy(lines[count], ls, len);
        lines[count][len] = '\0';
        count++;
        p = e;
        while (*p == '_') p++;
    }
    if (*p && count > 0)
        Nm_FitEllipsis(lastStart, lines[count - 1], NM_LINE_MAX, maxWidth, m);
    return count;
}

void Nm_FormatDate(int day, int month, int year, int hour, int minute, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    snprintf(out, outSize, "%02d._%02d._%04d._%02d:%02d", day, month, year, hour, minute);
}

/* ---- row page ---------------------------------------------------------- */

static int row_selectable(const NmPage* p, int i)
{
    return i >= 0 && i < p->rowCount && p->rows[i].kind != NM_ROW_INFO;
}

static int next_selectable(const NmPage* p, int from, int dir)
{
    for (int k = 1; k <= p->rowCount; k++) {
        int i = ((from + dir * k) % p->rowCount + p->rowCount) % p->rowCount;
        if (row_selectable(p, i)) return i;
    }
    return from;
}

void NmPage_Init(NmPage* p, int visibleRows)
{
    int first = 0;
    NmScroll_Init(&p->scroll, p->rowCount, visibleRows);
    while (first < p->rowCount && !row_selectable(p, first)) first++;
    if (first < p->rowCount) NmScroll_Select(&p->scroll, first);
}

static NmEvent page_press(NmPage* p, int dir, int fromConfirm)
{
    const NmRow* r = &p->rows[p->scroll.sel];
    if (r->kind == NM_ROW_VALUE && r->step) {
        r->step(p->ctx, dir);
        return NM_EV_STEPPED;
    }
    if (fromConfirm && r->kind == NM_ROW_ACTION && r->activate) {
        r->activate(p->ctx);
        return NM_EV_ACTIVATED;
    }
    return NM_EV_NONE;
}

NmEvent NmPage_Update(NmPage* p, const NmInput* in, int hoverRow)
{
    NmEvent ev = NM_EV_NONE;
    int old;
    if (p->rowCount <= 0) return in->cancel || in->mouseRight ? NM_EV_BACK : NM_EV_NONE;
    if (in->cancel || in->mouseRight) return NM_EV_BACK;

    old = p->scroll.sel;
    if (in->wheel) {
        NmScroll_Wheel(&p->scroll, in->wheel);
        if (!row_selectable(p, p->scroll.sel)) {
            int s = p->scroll.sel, i;
            for (i = s; i < p->rowCount && !row_selectable(p, i); i++) {}
            if (i >= p->rowCount) for (i = s; i >= 0 && !row_selectable(p, i); i--) {}
            if (i >= 0 && i < p->rowCount) NmScroll_Select(&p->scroll, i);
        }
        if (p->scroll.sel != old) ev = NM_EV_MOVED;
    }
    if (row_selectable(p, hoverRow)) {
        if (hoverRow != p->scroll.sel) {
            NmScroll_Select(&p->scroll, hoverRow);
            ev = NM_EV_MOVED;
        }
        if (in->mouseLeft) {
            NmEvent a = page_press(p, +1, 1);
            if (a != NM_EV_NONE) return a;
        }
    }
    if (in->up || in->down) {
        int n = next_selectable(p, p->scroll.sel, in->up ? -1 : +1);
        if (n != p->scroll.sel) {
            NmScroll_Select(&p->scroll, n);
            ev = NM_EV_MOVED;
        }
    }
    if (in->left || in->right) {
        NmEvent a = page_press(p, in->left ? -1 : +1, 0);
        if (a != NM_EV_NONE) return a;
    }
    if (in->confirm) {
        NmEvent a = page_press(p, +1, 1);
        if (a != NM_EV_NONE) return a;
    }
    return ev;
}

/* ---- confirm box ------------------------------------------------------- */

void NmConfirm_Open(NmConfirm* c)
{
    c->open = 1;
    c->yes  = 0;
}

NmConfirmResult NmConfirm_Update(NmConfirm* c, const NmInput* in)
{
    if (!c->open) return NM_CONFIRM_PENDING;
    if (in->cancel) {
        c->open = 0;
        return NM_CONFIRM_NO;
    }
    if (in->left || in->right) c->yes = !c->yes;
    if (in->confirm) {
        c->open = 0;
        return c->yes ? NM_CONFIRM_YES : NM_CONFIRM_NO;
    }
    return NM_CONFIRM_PENDING;
}

/* ---- title layout ------------------------------------------------------ */

int Nm_CompactRows(unsigned visibleFlags, int entryCount, int order[])
{
    int i, n = 0;
    for (i = 0; i < entryCount; i++)
        if (visibleFlags & (1u << i)) order[n++] = i;
    return n;
}

int Nm_SlotBase(int n, int windowSlots)
{
    return n <= windowSlots ? windowSlots - n : 0;
}

int Nm_SlotToIndex(int slot, int base, int top, int n)
{
    int idx = slot - base + top;
    return (slot >= base && idx >= 0 && idx < n) ? idx : -1;
}

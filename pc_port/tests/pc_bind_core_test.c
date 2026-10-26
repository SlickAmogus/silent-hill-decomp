/* pc_bind_core_test.c — renderer-free bind table logic against an in-memory
 * config. */
/* NDEBUG is set by the RelWithDebInfo build; this test IS its assertions. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pc_bind_core.h"
#include "pc_config.h"
#include "sh_log.h"

FILE* g_ShDebugLog = NULL;
int   g_PcBindsGen = 0;

#define MAX_FIELDS 128
static struct { char key[48]; char val[24]; } s_fields[MAX_FIELDS];
static int  s_fieldCount;
static char s_lastSaveKey[64], s_lastSaveVal[24];
static int  s_reapplied;

static const char* stripAlt(const char* key, char* buf, size_t n, int* perScheme)
{
    snprintf(buf, n, "%s", key);
    *perScheme = strncmp(key, "key_quick", 9) != 0 && strcmp(key, "key_swap_shoulder") != 0 &&
                 strcmp(key, "pad_quick_options") != 0 && strcmp(key, "pad_swap_shoulder") != 0;
    return buf;
}

/* Per-scheme keys keep one slot per scheme, as the real config does. */
char* PcConfig_BindField(const char* key, int scheme, int* outPerScheme)
{
    char        slot[64];
    char        tmp[64];
    int         perScheme, i;

    stripAlt(key, tmp, sizeof tmp, &perScheme);
    if (outPerScheme)
        *outPerScheme = perScheme;
    snprintf(slot, sizeof slot, "%s#%d", key, perScheme ? scheme : 0);
    for (i = 0; i < s_fieldCount; i++)
        if (strcmp(s_fields[i].key, slot) == 0)
            return s_fields[i].val;
    assert(s_fieldCount < MAX_FIELDS);
    snprintf(s_fields[s_fieldCount].key, sizeof s_fields[0].key, "%s", slot);
    s_fields[s_fieldCount].val[0] = '\0';
    return s_fields[s_fieldCount++].val;
}

const char* PcConfig_BindDefault(const char* key, int scheme)
{
    (void)key; (void)scheme;
    return "";
}

void PcConfig_SaveKeyValue(const char* key, const char* value)
{
    snprintf(s_lastSaveKey, sizeof s_lastSaveKey, "%s", key);
    snprintf(s_lastSaveVal, sizeof s_lastSaveVal, "%s", value);
}

void PcConfig_SaveKeyValues(const char* const* keys, const char* const* values, int count)
{
    (void)keys; (void)values; (void)count;
}

void Pc_ControlStyle_ReapplyBinds(void) { s_reapplied++; }
void Pc_QuickOptions_InvalidateRows(void) {}

static void setField(const char* key, int scheme, const char* v)
{
    snprintf(PcConfig_BindField(key, scheme, NULL), 24, "%s", v);
}

static void test_bind_cell_usable(void)
{
    BindCore_SetScheme(0);
    assert(!strcmp(BindCore_Row(15)->label, "Cycle Weapons"));
    assert(!BindCore_CellUsable(15, BC_KB2));
    assert(BindCore_CellUsable(15, BC_KB1));
    assert(!strcmp(BindCore_Row(0)->label, "Move Up"));
    assert(!BindCore_CellUsable(0, BC_PAD1));
    assert(!strcmp(BindCore_Row(18)->label, "Rear Look"));
    assert(!BindCore_RowEnabled(18) && !BindCore_CellUsable(18, BC_KB1));
    BindCore_SetScheme(1);
    assert(BindCore_RowEnabled(18) && BindCore_CellUsable(18, BC_KB1));
    BindCore_SetScheme(0);
}

static void test_bind_duplicate(void)
{
    BindCore_SetScheme(0);
    setField("key_cross", 0, "Space");
    setField("key_circle", 0, "Space");
    assert(BindCore_IsDuplicate(4, BC_KB1) && BindCore_IsDuplicate(5, BC_KB1));
    setField("pad_cross", 0, "Space");
    assert(!BindCore_IsDuplicate(4, BC_PAD1));
    setField("key_circle", 0, "NONE");
    assert(!BindCore_IsDuplicate(4, BC_KB1));
}

static void test_bind_set_altcam_key(void)
{
    int before = s_reapplied;

    BindCore_SetScheme(1);
    BindCore_Set(7, BC_KB1, "E");
    assert(!strcmp(s_lastSaveKey, "key_square_altcam") && !strcmp(s_lastSaveVal, "E"));
    assert(s_reapplied == before + 1);
    BindCore_Set(22, BC_KB1, "F5");
    assert(!strcmp(s_lastSaveKey, "key_quicksave"));
    BindCore_SetScheme(0);
    BindCore_Set(22, BC_KB1, "F6");
    assert(!strcmp(s_lastSaveKey, "key_quicksave"));
    BindCore_Set(7, BC_KB1, "X");
    assert(!strcmp(s_lastSaveKey, "key_square"));
}

static void test_bind_toast(void)
{
    char buf[64];

    BindCore_FormatRebindToast(7, BC_KB1, buf, sizeof buf);
    assert(!strcmp(buf, "Changing Primary input for Run"));
    BindCore_FormatRebindToast(4, BC_PAD2, buf, sizeof buf);
    assert(!strcmp(buf, "Changing Secondary input for Action / Shoot"));
}

static void test_bind_display_key(void)
{
    char buf[32];

    BindCore_DisplayKey("Left Shift", buf, sizeof buf);
    assert(!strcmp(buf, "L-Shift"));
    BindCore_DisplayKey("Right Ctrl", buf, sizeof buf);
    assert(!strcmp(buf, "R-Ctrl"));
    BindCore_DisplayKey("Keypad Enter", buf, sizeof buf);
    assert(!strcmp(buf, "KP Enter"));
    BindCore_DisplayKey("Left", buf, sizeof buf);
    assert(!strcmp(buf, "Left"));
    BindCore_DisplayKey("Right", buf, sizeof buf);
    assert(!strcmp(buf, "Right"));
    BindCore_DisplayKey("mouse2", buf, sizeof buf);
    assert(!strcmp(buf, "R-Mouse"));
    BindCore_DisplayKey("Mouse4", buf, sizeof buf);
    assert(!strcmp(buf, "Mouse4"));
    BindCore_DisplayKey("MouseWheelDown", buf, sizeof buf);
    assert(!strcmp(buf, "Wheel Down"));
    BindCore_DisplayKey("Space", buf, 3);
    assert(!strcmp(buf, "Sp"));
    BindCore_DisplayKey(NULL, buf, sizeof buf);
    assert(!strcmp(buf, ""));
}

int main(void)
{
    test_bind_cell_usable();
    test_bind_duplicate();
    test_bind_set_altcam_key();
    test_bind_toast();
    test_bind_display_key();
    puts("pc_bind_core_test: ok");
    return 0;
}

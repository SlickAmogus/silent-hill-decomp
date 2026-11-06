/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/events/bgm.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/text/text_draw.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"
#include "lang_text.h"
#include "pc_chara_select.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"
#include "pc_native_menu_draw.h"
#include "pc_playas.h"
#include "pc_skin_chara.h"
#include "sh_log.h"

/* Layout in the menu space (see pc_native_menu_draw.h): 224 rows, and
 * -margin..320+margin columns, the margin being what the window's aspect adds
 * beyond 4:3 (PsyX_WideMenuMargin while g_PsxWideMenu is set). The columns
 * share the field by proportion, so the screen fills 16:9 and 21:9. */
#define CS_HEADING_Y   12
#define CS_RULE_Y      30
#define CS_LIST_Y      40
#define CS_PITCH       16
#define CS_VISIBLE     9
#define CS_ARROW_UP_Y  34
#define CS_ARROW_DN_Y  188
#define CS_PREV_Y0     34
#define CS_PREV_Y1     196
#define CS_FOOTER_Y    204
#define CS_OT_LINE     6
#define CS_OT_SHADE    9

extern int   g_PsxWideMenu;
extern float PsyX_WideMenuMargin(void);

static int s_fieldL, s_fieldR; /* this frame's field, menu columns */
#define CS_FIELD_W    (s_fieldR - s_fieldL)
#define CS_COL1_X0    (s_fieldL + 8)
#define CS_DIV1_X     (s_fieldL + (CS_FIELD_W * 31) / 100)
#define CS_COL1_X1    (CS_DIV1_X - 6)
#define CS_COL2_X0    (CS_DIV1_X + 8)
#define CS_DIV2_X     (s_fieldL + (CS_FIELD_W * 62) / 100)
#define CS_COL2_X1    (CS_DIV2_X - 6)
#define CS_PREV_X0    (CS_DIV2_X + 6)
#define CS_PREV_X1    (s_fieldR - 8)
#define CS_BACK_X     (s_fieldL + 16)

static void cs_field_update(void)
{
    int m = g_PsxWideMenu ? (int)(PsyX_WideMenuMargin() + 0.5f) : 0;

    s_fieldL = -m;
    s_fieldR = 320 + m;
}

#define CS_ROW_DOWNLOAD 0
#define CS_HIT_NONE   (-1)
#define CS_HIT_BACK   (-2)
#define CS_HIT_PREV   (-3)
#define CS_HIT_NEXT   (-4)

#define CS_MAX_MODELS  48
#define CS_TOKEN_MAX   48

typedef struct
{
    const char* ilm;   /* stock model name: the loader's key and the config key suffix */
    const char* label;
    int         charaId;
} CsChara;

/* The humans the skin loader has a rest table for (s_restDefs), so any swap
 * fits them. Monster and ending Cybil share Cybil's row through the loader's
 * alt rule, so the rows stay the characters a player names. */
static const CsChara s_charas[] = {
    { "HERO",  "HARRY",            Chara_Harry },
    { "SIBYL", "CYBIL",            Chara_Cybil },
    { "MSB",   "MONSTER CYBIL",    Chara_MonsterCybil },
    { "LISA",  "LISA",             Chara_Lisa },
    { "BLISA", "BLOODY LISA",      Chara_BloodyLisa },
    { "DARIA", "DAHLIA",           Chara_Dahlia },
    { "KAU",   "KAUFMANN",         Chara_Kaufmann },
    { "SRL",   "CHERYL",           Chara_Cheryl },
    { "AR",    "ALESSA",           Chara_Alessa },
    { "TAR",   "GHOST ALESSA",     Chara_GhostChildAlessa },
    { "MAR",   "INCUBATOR",        Chara_Incubator },
    { "BAR",   "BLOODY INCUBATOR", Chara_BloodyIncubator },
    { "DOC",   "GHOST DOCTOR",     Chara_GhostDoctor },
};
#define CS_CHARA_COUNT ((int)(sizeof(s_charas) / sizeof(s_charas[0])))
#define CS_ROW_COUNT   (1 + CS_CHARA_COUNT)

typedef enum { CS_M_STOCK, CS_M_PLAYAS, CS_M_FILE } CsModelKind;

typedef struct
{
    CsModelKind kind;
    char        token[CS_TOKEN_MAX]; /* config value: "-" stock, play-as name, or file base */
    char        display[64];         /* the file name shown */
    int         playasIdx;
} CsModel;

/* Saved choices, filled from config before the roster exists. */
static char s_choice[CS_CHARA_COUNT][CS_TOKEN_MAX];

static int      s_open;
static int      s_onBack;
static int      s_escPrev;
static NmScroll s_scroll;
static int      s_hoverCol2;
static char     s_status[64];
static unsigned s_statusUntil;

/* The selected row's models, rebuilt when the row changes. */
static CsModel s_models[CS_MAX_MODELS];
static int     s_modelCount;
static int     s_modelCur;
static int     s_modelsRow = -1;

/* The swap files in gamedata/load/CHARA, scanned when the screen opens. */
static char s_files[CS_MAX_MODELS][CS_TOKEN_MAX];
static int  s_fileCount;

/* Music: the results track while the screen is open, as the online lobby
 * plays it. Under the title menu nothing pumps the BGM state machine, so the
 * screen does; in game the gameplay state keeps doing it. */
static int      s_prevBgm = -1;
static int      s_musicStep;

static int cs_chara_index(const char* ilm)
{
    int i;

    for (i = 0; i < CS_CHARA_COUNT; i++)
        if (strcmp(s_charas[i].ilm, ilm) == 0)
            return i;
    return -1;
}

void Pc_CharaSelect_ConfigKey(const char* name, const char* value)
{
    int i = cs_chara_index(name);

    if (i < 0 || value == NULL)
        return;
    snprintf(s_choice[i], sizeof(s_choice[i]), "%s", value);
}

/* Harry's choice lives in player_character through Play As. */
static void cs_apply(int chara)
{
    if (chara == 0)
        return;
    Pc_SkinChara_SetCharaFile(s_charas[chara].ilm, s_choice[chara]);
}

void Pc_CharaSelect_Init(void)
{
    int i;

    for (i = 1; i < CS_CHARA_COUNT; i++)
        cs_apply(i);
}

static int cs_file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");

    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

static void cs_scan_files(void)
{
    DIR*           d;
    struct dirent* e;

    s_fileCount = 0;
    if (!g_PcConfig.allowLooseFiles || (d = opendir("gamedata/load/CHARA")) == NULL)
        return;
    while ((e = readdir(d)) != NULL && s_fileCount < CS_MAX_MODELS)
    {
        const char* dot = strrchr(e->d_name, '.');
        size_t      n;

        if (dot == NULL || (SDL_strcasecmp(dot, ".glb") != 0 && SDL_strcasecmp(dot, ".gltf") != 0))
            continue;
        n = (size_t)(dot - e->d_name);
        if (n == 0 || n >= CS_TOKEN_MAX)
            continue;
        /* HERO.glb replaces Harry outright and is not a choice. */
        if (n == 4 && SDL_strncasecmp(e->d_name, "HERO", 4) == 0)
            continue;
        memcpy(s_files[s_fileCount], e->d_name, n);
        s_files[s_fileCount][n] = '\0';
        s_fileCount++;
    }
    closedir(d);
}

static void cs_ilm_name(int charaId, char* out, size_t outSize)
{
    char file[32];
    char* dot;

    Fs_GetFileName(file, CHARA_FILE_INFOS[charaId].modelFileIdx);
    dot = strrchr(file, '.');
    if (dot != NULL)
        *dot = '\0';
    snprintf(out, outSize, "%s", file);
}

static void cs_file_display(const char* base, char* out, size_t outSize)
{
    char path[160];

    snprintf(path, sizeof(path), "gamedata/load/CHARA/%s.glb", base);
    snprintf(out, outSize, "%s.%s", base, cs_file_exists(path) ? "GLB" : "GLTF");
}

static void cs_build_models(int chara)
{
    int i;

    s_modelCount = 0;
    s_modelCur   = 0;
    s_modelsRow  = chara;
    if (chara == 0)
    {
        /* The Play As roster, Harry's own body first. */
        int n   = Pc_PlayAs_Count();
        int cur = Pc_PlayAs_Current();
        int pass;

        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < n && s_modelCount < CS_MAX_MODELS; i++)
            {
                CsModel*    m    = &s_models[s_modelCount];
                const char* file = Pc_PlayAs_File(i);
                int         self = Pc_PlayAs_CharaId(i) == Chara_Harry && file == NULL;

                if ((pass == 0) != self)
                    continue;
                m->kind      = CS_M_PLAYAS;
                m->playasIdx = i;
                snprintf(m->token, sizeof(m->token), "%s", Pc_PlayAs_Name(i));
                if (file != NULL)
                    cs_file_display(file, m->display, sizeof(m->display));
                else
                {
                    char ilm[32];
                    cs_ilm_name(Pc_PlayAs_CharaId(i), ilm, sizeof(ilm));
                    snprintf(m->display, sizeof(m->display), "%s.ILM", ilm);
                }
                if (i == cur)
                    s_modelCur = s_modelCount;
                s_modelCount++;
            }
        return;
    }

    {
        CsModel*    m      = &s_models[s_modelCount++];
        const char* choice = s_choice[chara];
        char        own[160];
        int         ownGlb;

        m->kind = CS_M_STOCK;
        snprintf(m->token, sizeof(m->token), "-");
        snprintf(m->display, sizeof(m->display), "%s.ILM", s_charas[chara].ilm);

        snprintf(own, sizeof(own), "gamedata/load/CHARA/%s.glb", s_charas[chara].ilm);
        ownGlb = cs_file_exists(own);
        for (i = 0; i < s_fileCount && s_modelCount < CS_MAX_MODELS; i++)
        {
            m       = &s_models[s_modelCount];
            m->kind = CS_M_FILE;
            snprintf(m->token, sizeof(m->token), "%s", s_files[i]);
            cs_file_display(s_files[i], m->display, sizeof(m->display));
            /* No saved choice keeps the loader's old rule: NAME.glb if installed. */
            if ((choice[0] != '\0' && strcmp(choice, m->token) == 0) ||
                (choice[0] == '\0' && ownGlb && SDL_strcasecmp(s_files[i], s_charas[chara].ilm) == 0))
                s_modelCur = s_modelCount;
            s_modelCount++;
        }
    }
}

static int cs_chara_row(void)
{
    return s_scroll.sel - 1;
}

static void cs_models_for_row(void)
{
    int chara = cs_chara_row();

    if (chara < 0)
    {
        s_modelCount = 0;
        s_modelsRow  = -1;
        return;
    }
    if (s_modelsRow != chara)
        cs_build_models(chara);
}

static void cs_set_status(const char* s)
{
    snprintf(s_status, sizeof(s_status), "%s", s);
    s_statusUntil = SDL_GetTicks() + 3000;
}

static void cs_pick(int chara, int idx)
{
    const CsModel* m;
    char           key[32];

    if (idx < 0 || idx >= s_modelCount)
        return;
    m          = &s_models[idx];
    s_modelCur = idx;
    if (m->kind == CS_M_PLAYAS)
    {
        if (!Pc_PlayAs_SetByIndex(m->playasIdx, 1))
            cs_set_status("COULD NOT LOAD THAT MODEL");
        return;
    }
    snprintf(s_choice[chara], sizeof(s_choice[chara]), "%s", m->token);
    cs_apply(chara);
    snprintf(key, sizeof(key), "chara_model_%s", s_charas[chara].ilm);
    PcConfig_SaveKeyValue(key, m->token);
    SH_DBG("[CHARASEL] %s -> %s", s_charas[chara].ilm, m->token);
}

static void cs_cycle(int dir)
{
    int chara = cs_chara_row();

    if (chara < 0 || s_modelCount < 2)
        return;
    SD_Call(Sfx_MenuMove);
    cs_pick(chara, (s_modelCur + (dir < 0 ? -1 : 1) + s_modelCount) % s_modelCount);
}

/* ---- music ---------------------------------------------------------- */

static void cs_music_start(void)
{
    s_prevBgm   = g_GameWork.bgmIdx;
    s_musicStep = 0;
    if (s_prevBgm == BgmTrackIdx_5)
    {
        s_musicStep = 2;
        return;
    }
    SD_Call(18);
    Bgm_AllLayersMute();
    s_musicStep = 1;
}

static void cs_music_tick(void)
{
    if (s_musicStep == 1)
    {
        if (Sd_AudioStreamingCheck() || Fs_QueueGetLength() != 0 || func_80045BC8() != 0)
            return;
        Bgm_TrackSet(BgmTrackIdx_5);
        s_musicStep = 2;
    }
    if (s_musicStep == 2 && g_GameWork.gameState == GameState_MainMenu)
    {
        /* Nothing else runs the BGM state machine under the title menu. The
         * layer ramps read the player's health, which is zero here. */
        if (g_SysWork.playerWork.player.health <= Q12(0.0f))
            g_SysWork.playerWork.player.health = Q12(100.0f);
        Bgm_Update(BgmFlag_Layer1 | BgmFlag_Layer2 | BgmFlag_Layer3 | BgmFlag_Layer4 |
                   BgmFlag_Layer5 | BgmFlag_Layer6 | BgmFlag_Layer7 | BgmFlag_KeepAlive,
                   Q12(240.0f), NULL);
    }
}

static void cs_music_stop(void)
{
    if (s_prevBgm < 0)
        return;
    if (s_prevBgm == BgmTrackIdx_5)
    {
        s_prevBgm = -1;
        return;
    }
    SD_Call(18);
    Bgm_AllLayersMute();
    g_GameWork.bgmIdx = BgmTrackIdx_None;
    if (Bgm_ActiveBgmTrackCheck(s_prevBgm))
        Bgm_PlayNewTrack(s_prevBgm);
    s_prevBgm = -1;
}

/* ---- open / close --------------------------------------------------- */

void Pc_CharaSelect_Open(void)
{
    cs_scan_files();
    NmScroll_Init(&s_scroll, CS_ROW_COUNT, CS_VISIBLE);
    NmScroll_Select(&s_scroll, 1);
    s_modelsRow = -1;
    cs_models_for_row();
    s_onBack    = 0;
    s_hoverCol2 = 0;
    s_escPrev   = 1;
    s_status[0] = '\0';
    s_open      = 1;
    g_PsxWideMenu = !g_PcConfig.nativeResolution;
    cs_field_update();
    cs_music_start();
    SH_DBG("[CHARASEL] opened: %d swap files", s_fileCount);
}

int Pc_CharaSelect_IsOpen(void)
{
    return s_open;
}

void Pc_CharaSelect_Close(void)
{
    if (!s_open)
        return;
    s_open = 0;
    g_PsxWideMenu = 0;
    cs_music_stop();
}

/* ---- input ---------------------------------------------------------- */

static int cs_back_right(void)
{
    return CS_BACK_X + NM_BULLET_TEXT_DX + Nm_TextWidth("Back");
}

static int cs_escape_pressed(void)
{
    const Uint8* ks   = SDL_GetKeyboardState(NULL);
    int          held = ks != NULL && ks[SDL_SCANCODE_ESCAPE];
    int          edge = held && !s_escPrev;

    s_escPrev = held;
    return edge;
}

static int cs_hit(const NmInput* in)
{
    int i;

    s_hoverCol2 = 0;
    if (!in->mouseOk)
        return CS_HIT_NONE;

    for (i = 0; i < CS_VISIBLE && s_scroll.top + i < CS_ROW_COUNT; i++)
    {
        int y   = CS_LIST_Y + (i * CS_PITCH) - 1;
        int row = s_scroll.top + i;

        if (in->mouseY < y || in->mouseY >= y + CS_PITCH)
            continue;
        if (in->mouseX >= s_fieldL && in->mouseX < CS_DIV1_X)
            return row;
        if (row > 0 && in->mouseX >= CS_DIV1_X && in->mouseX < CS_DIV2_X)
        {
            s_hoverCol2 = 1;
            /* The arrow halves cycle; the name itself just selects the row. */
            if (row == s_scroll.sel)
            {
                if (in->mouseX < CS_COL2_X0 + 10)
                    return CS_HIT_PREV;
                if (in->mouseX >= CS_COL2_X1 - 10)
                    return CS_HIT_NEXT;
            }
            return row;
        }
    }

    if (in->mouseX >= CS_BACK_X - 1 && in->mouseX < cs_back_right() + 4 &&
        in->mouseY >= CS_FOOTER_Y && in->mouseY < CS_FOOTER_Y + 16)
        return CS_HIT_BACK;
    return CS_HIT_NONE;
}

static void cs_select_row(int row)
{
    s_onBack = 0;
    NmScroll_Select(&s_scroll, row);
    cs_models_for_row();
}

static void cs_download(void)
{
    SD_Call(Sfx_MenuConfirm);
    cs_set_status("DOWNLOADS COME WITH THE NEXT UPDATE");
}

/* 0 when the activation closes the screen. */
static int cs_activate(void)
{
    if (s_onBack)
    {
        SD_Call(Sfx_MenuCancel);
        return 0;
    }
    if (s_scroll.sel == CS_ROW_DOWNLOAD)
    {
        cs_download();
        return 1;
    }
    cs_cycle(+1);
    return 1;
}

int Pc_CharaSelect_Update(const NmInput* inRaw)
{
    NmInput  loc;
    const NmInput* in = &loc;
    int oldSel;
    int oldBack;
    int back;
    int hit;

    if (!s_open)
        return 0;
    cs_field_update();
    loc = *inRaw;
    if (loc.mouseOk)
        loc.mouseX = s_fieldL + (loc.mouseX * CS_FIELD_W) / 320;
    cs_music_tick();
    back    = cs_escape_pressed();
    oldSel  = s_scroll.sel;
    oldBack = s_onBack;

    if (back || in->cancel || in->mouseRight)
    {
        SD_Call(Sfx_MenuCancel);
        return 0;
    }

    hit = cs_hit(in);
    if (in->wheel != 0)
    {
        if (s_hoverCol2 && hit != CS_HIT_NONE && hit != CS_HIT_BACK)
        {
            /* Scrolling over the model column cycles the row under the mouse. */
            if (hit == CS_HIT_PREV || hit == CS_HIT_NEXT || hit == s_scroll.sel)
                cs_cycle(in->wheel > 0 ? -1 : +1);
            else
            {
                Nm_HighlightSnap();
                cs_select_row(hit);
                cs_cycle(in->wheel > 0 ? -1 : +1);
            }
        }
        else
        {
            int oldTop = s_scroll.top;

            NmScroll_Wheel(&s_scroll, in->wheel);
            if (s_scroll.top != oldTop)
                s_onBack = 0;
            cs_models_for_row();
        }
    }

    if (in->mouseLeft && hit != CS_HIT_NONE)
    {
        Nm_HighlightSnap();
        if (hit == CS_HIT_BACK)
        {
            s_onBack = 1;
            return cs_activate();
        }
        if (hit == CS_HIT_PREV || hit == CS_HIT_NEXT)
        {
            cs_cycle(hit == CS_HIT_PREV ? -1 : +1);
            return 1;
        }
        if (hit != s_scroll.sel || s_onBack)
            cs_select_row(hit);
        else
            return cs_activate();
    }
    else if (in->mouseMoved && hit != CS_HIT_NONE)
    {
        Nm_HighlightSnap();
        if (hit == CS_HIT_BACK)
            s_onBack = 1;
        else if (hit >= 0)
            cs_select_row(hit);
    }

    if (in->up && s_onBack)
        s_onBack = 0;
    else if (in->up)
    {
        NmScroll_Move(&s_scroll, -1, 0);
        cs_models_for_row();
    }
    else if (in->down && !s_onBack)
    {
        if (s_scroll.sel >= CS_ROW_COUNT - 1)
            s_onBack = 1;
        else
        {
            NmScroll_Move(&s_scroll, +1, 0);
            cs_models_for_row();
        }
    }
    else if (in->left && !s_onBack)
        cs_cycle(-1);
    else if (in->right && !s_onBack)
        cs_cycle(+1);

    if (s_scroll.sel != oldSel || s_onBack != oldBack)
        SD_Call(Sfx_MenuMove);

    if (in->confirm)
        return cs_activate();
    return 1;
}

/* ---- drawing -------------------------------------------------------- */

static int cs_interlaced(void)
{
    return g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED;
}

static s16 cs_px(int x) { return (s16)(x - 160); }
static s16 cs_py(int y) { return (s16)(cs_interlaced() ? (y - 112) * 2 : (y - 112)); }

/* The inventory's blue double frame (Gfx_Results_Save): (96,96,255) outside,
 * (32,32,255) two pixels in. */
static void cs_line(int x0, int y0, int x1, int y1, int inner)
{
    LINE_F2* line = (LINE_F2*)GsOUT_PACKET_P;
    u8       rg   = inner ? Q8_COLOR(0.125f) : Q8_COLOR(0.375f);

    setLineF2(line);
    setRGB0(line, rg, rg, 0xFF);
    setXY2(line, cs_px(x0), cs_py(y0), cs_px(x1), cs_py(y1));
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[CS_OT_LINE], line);
    GsOUT_PACKET_P = (PACKET*)(line + 1);
}

static void cs_frame(int x0, int y0, int x1, int y1)
{
    cs_line(x0, y0, x1, y0, 0);
    cs_line(x0, y1, x1, y1, 0);
    cs_line(x0, y0, x0, y1, 0);
    cs_line(x1, y0, x1, y1, 0);
    cs_line(x0 + 2, y0 + 2, x1 - 2, y0 + 2, 1);
    cs_line(x0 + 2, y1 - 2, x1 - 2, y1 - 2, 1);
    cs_line(x0 + 2, y0 + 2, x0 + 2, y1 - 2, 1);
    cs_line(x1 - 2, y0 + 2, x1 - 2, y1 - 2, 1);
}

/* In game the world stays behind the screen: darken it so the text reads,
 * with the subtractive blend the native highlight's shadow uses. */
static void cs_shade(int x0, int y0, int x1, int y1)
{
    POLY_F4* poly = (POLY_F4*)GsOUT_PACKET_P;

    setPolyF4(poly);
    setSemiTrans(poly, true);
    setRGB0(poly, 112, 112, 112);
    setXY4(poly, cs_px(x0), cs_py(y0), cs_px(x1), cs_py(y0), cs_px(x0), cs_py(y1), cs_px(x1), cs_py(y1));
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[CS_OT_SHADE], poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
    Gfx_Primitive2dTextureSet(0, 0, CS_OT_SHADE, 2);
}

static void cs_back_draw(int selected)
{
    int x = CS_BACK_X + NM_BULLET_TEXT_DX;

    Nm_BulletDraw(CS_BACK_X, CS_FOOTER_Y, selected);
    Nm_TextDraw(x, CS_FOOTER_Y, NM_TEXT_TALL, StringColorId_White, "Back");
    if (selected)
        Nm_HighlightDraw(CS_BACK_X - 1, cs_back_right() + 4, CS_FOOTER_Y + 14);
}

static void cs_rows_draw(void)
{
    char clean[128];
    int  i;

    for (i = 0; i < CS_VISIBLE && s_scroll.top + i < CS_ROW_COUNT; i++)
    {
        int row   = s_scroll.top + i;
        int y     = CS_LIST_Y + (i * CS_PITCH);
        int sel   = !s_onBack && row == s_scroll.sel;
        int color = sel ? StringColorId_Gold : StringColorId_White;
        int w;

        if (row == CS_ROW_DOWNLOAD)
        {
            Nm_SanitizeGlyphs("Download more...", clean, sizeof(clean), 1);
            w = Nm_TextDrawMarquee(CS_COL1_X0, y, NM_TEXT_TALL, color, clean, CS_COL1_X1 - CS_COL1_X0, sel);
            if (sel)
                Nm_HighlightDraw(CS_COL1_X0 - 1, CS_COL1_X0 + w + 4, y + 14);
            continue;
        }

        {
            const CsChara* c = &s_charas[row - 1];
            const char*    value = "";

            Nm_SanitizeGlyphs(c->label, clean, sizeof(clean), 1);
            w = Nm_TextDrawLatin(CS_COL1_X0, y, NM_TEXT_TALL, color, clean, CS_COL1_X1 - CS_COL1_X0, sel);
            if (sel)
                Nm_HighlightDraw(CS_COL1_X0 - 1, CS_COL1_X0 + w + 4, y + 14);

            if (sel && s_modelCount > 0)
                value = s_models[s_modelCur].display;
            else if (row - 1 == 0)
            {
                const char* f = Pc_PlayAs_File(Pc_PlayAs_Current());
                char        ilm[32];
                static char cur[64];

                if (f != NULL)
                    cs_file_display(f, cur, sizeof(cur));
                else
                {
                    cs_ilm_name(Pc_PlayAs_SkinCharaId(), ilm, sizeof(ilm));
                    snprintf(cur, sizeof(cur), "%s.ILM", ilm);
                }
                value = cur;
            }
            else
            {
                static char cur[64];
                const char* choice = s_choice[row - 1];

                if (choice[0] == '\0' || strcmp(choice, "-") == 0)
                {
                    char own[160];

                    snprintf(own, sizeof(own), "gamedata/load/CHARA/%s.glb", c->ilm);
                    if (choice[0] == '\0' && cs_file_exists(own))
                        snprintf(cur, sizeof(cur), "%s.GLB", c->ilm);
                    else
                        snprintf(cur, sizeof(cur), "%s.ILM", c->ilm);
                }
                else
                    cs_file_display(choice, cur, sizeof(cur));
                value = cur;
            }

            Nm_SanitizeGlyphs(value, clean, sizeof(clean), 1);
            if (sel)
            {
                int maxW = CS_COL2_X1 - CS_COL2_X0 - 20;
                int tw   = Nm_TextWidthLatin(clean, NM_TEXT_TALL);
                int x    = CS_COL2_X0 + 10 + ((maxW - (tw < maxW ? tw : maxW)) / 2);

                w = Nm_TextDrawLatin(x, y, NM_TEXT_TALL, StringColorId_Gold, clean, maxW, 1);
                if (s_modelCount > 1)
                    Nm_ValueArrowsDraw(x, x + w, y);
            }
            else
                Nm_TextDrawLatin(CS_COL2_X0, y, NM_TEXT_TALL, StringColorId_LightGrey, clean,
                                 CS_COL2_X1 - CS_COL2_X0, 0);
        }
    }

    if (CS_ROW_COUNT > CS_VISIBLE)
    {
        Nm_ScrollArrowDraw((CS_COL1_X0 + CS_DIV2_X) / 2, CS_ARROW_UP_Y, 1, NmScroll_CanUp(&s_scroll));
        Nm_ScrollArrowDraw((CS_COL1_X0 + CS_DIV2_X) / 2, CS_ARROW_DN_Y, 0, NmScroll_CanDown(&s_scroll));
    }
}

void Pc_CharaSelect_Draw(void)
{
    char clean[128];

    if (!s_open)
        return;
    cs_field_update();

    if (g_GameWork.gameState == GameState_InGame)
        cs_shade(s_fieldL, 0, s_fieldR, 224);
    else
        Nm_BlackBackgroundDraw();

    Nm_TextDrawCentered(160, CS_HEADING_Y, NM_TEXT_TALL, StringColorId_White, "CHARACTER_SELECT");
    Nm_RuleDraw(CS_COL1_X0, CS_PREV_X1, CS_RULE_Y, 0);
    cs_line(CS_DIV1_X, CS_RULE_Y, CS_DIV1_X, CS_ARROW_DN_Y + 6, 1);
    cs_frame(CS_PREV_X0, CS_PREV_Y0, CS_PREV_X1, CS_PREV_Y1);

    cs_rows_draw();
    cs_back_draw(s_onBack);

    {
        const char* file;

        if (Pc_CharaSelect_PreviewTarget(&file) != NULL && file == NULL)
            Nm_TextDrawCentered((CS_PREV_X0 + CS_PREV_X1) / 2, (CS_PREV_Y0 + CS_PREV_Y1) / 2 - 8, NM_TEXT_TALL,
                                StringColorId_DarkGrey, "NO_PREVIEW");
    }

    if (s_status[0] != '\0' && SDL_GetTicks() < s_statusUntil)
    {
        Nm_SanitizeGlyphs(s_status, clean, sizeof(clean), 1);
        Nm_TextDrawLatin(cs_back_right() + 12, CS_FOOTER_Y, NM_TEXT_TALL, StringColorId_LightGrey, clean,
                         CS_PREV_X1 - cs_back_right() - 12, 1);
    }
    Pc_MouseCursor_Show();
}

void Pc_CharaSelect_PreviewRect(int* x0, int* y0, int* x1, int* y1)
{
    *x0 = CS_PREV_X0 + 3;
    *y0 = CS_PREV_Y0 + 3;
    *x1 = CS_PREV_X1 - 3;
    *y1 = CS_PREV_Y1 - 3;
}

const char* Pc_CharaSelect_PreviewTarget(const char** file)
{
    int chara = cs_chara_row();

    *file = NULL;
    if (!s_open || chara < 0 || s_modelCount == 0)
        return NULL;
    {
        const CsModel* m = &s_models[s_modelCur];

        if (m->kind == CS_M_FILE)
            *file = m->token;
        else if (m->kind == CS_M_PLAYAS)
            *file = Pc_PlayAs_File(m->playasIdx);
    }
    return s_charas[chara].ilm;
}

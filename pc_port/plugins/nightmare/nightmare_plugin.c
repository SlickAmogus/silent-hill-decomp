#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "pc_config.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "map_registry.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/text/text_debug_draw.h"
#include "bodyprog/sound/sound_system.h"
#include "screens/options.h"

#define SH_LOG(fmt, ...) printf("[NIGHTMARE] " fmt "\n", ##__VA_ARGS__)

#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#include "main/fileinfo.h"

static int s_running = 0;

static void ApplyShadowStalkerModelOverrides(void)
{
    /* In Nightmare Mode, redirect Grey Children & Mumblers to the Shadow Stalker assets (CLD2) */
    CHARA_FILE_INFOS[Chara_GreyChild].modelFileIdx       = FILE_CHARA_CLD2_ILM;
    CHARA_FILE_INFOS[Chara_GreyChild].textureFileIdx     = FILE_CHARA_CLD2_TIM;
    CHARA_FILE_INFOS[Chara_GreyChild].materialBlendMode = BlendMode_Subtractive;

    CHARA_FILE_INFOS[Chara_Mumbler].modelFileIdx         = FILE_CHARA_CLD2_ILM;
    CHARA_FILE_INFOS[Chara_Mumbler].textureFileIdx       = FILE_CHARA_CLD2_TIM;
    CHARA_FILE_INFOS[Chara_Mumbler].materialBlendMode   = BlendMode_Subtractive;
}

static void Patch_HideHealthStatus(void)
{
#ifdef _WIN32
    void* pFunc = (void*)GetProcAddress(GetModuleHandleA(NULL), "Gfx_Inventory_HealthStatusDraw");
    if (pFunc)
    {
        DWORD oldProtect;
        if (VirtualProtect(pFunc, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *(unsigned char*)pFunc = 0xC3; /* x86/x64 RET instruction to suppress health face/bar */
            VirtualProtect(pFunc, 1, oldProtect, &oldProtect);
            SH_LOG("[NIGHTMARE_PLUGIN] Successfully patched Gfx_Inventory_HealthStatusDraw to RET (health indicator hidden).");
        }
    }
#endif
}

static void Plugin_LoadNightmareConfig(void)
{
    g_PcConfig.nightmare = 1;
    g_PcConfig.revampedController = 1;
    g_PcConfig.liveInventory = 1;
    g_PcConfig.nightmareVignette = 1;

    FILE* f = fopen("config.cfg", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64], val[64];
        if (sscanf(line, " %63[^= ] = %63s", key, val) == 2 ||
            sscanf(line, " %63[^= ] = \"%63[^\"]\"", key, val) == 2)
        {
            if (strcmp(key, "live_game") == 0 || strcmp(key, "live_inventory") == 0)
            {
                g_PcConfig.liveInventory = atoi(val);
            }
            else if (strcmp(key, "low_health_fx") == 0 || strcmp(key, "nightmare_vignette") == 0)
            {
                g_PcConfig.nightmareVignette = atoi(val);
            }
        }
    }
    fclose(f);
}

#ifdef _WIN32
static void InstallHook64(void* target, void* replacement)
{
    if (!target || !replacement) return;
    DWORD oldProtect;
    if (VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        unsigned char jmpCode[14] = {
            0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, /* jmp qword ptr [rip + 0] */
            0, 0, 0, 0, 0, 0, 0, 0              /* 64-bit target address */
        };
        *(uintptr_t*)(&jmpCode[6]) = (uintptr_t)replacement;
        memcpy(target, jmpCode, 14);
        VirtualProtect(target, 14, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), target, 14);
    }
}
#endif

typedef struct {
    const char* label;
    const char* keyCfg;
    const char* padCfg;
    size_t      keyOff;
    size_t      padOff;
} Plugin_PcControlAction;

#define CTRL_OFF_KEY(f) offsetof(ControlScheme, f)
#define CTRL_OFF_PAD(f) offsetof(ControlScheme, f)

static const Plugin_PcControlAction PLUGIN_CONTROL_ACTIONS[] = {
    { "FORWARD",     "key_up",         "pad_up",         CTRL_OFF_KEY(keyUp),        0 },
    { "BACKWARD",    "key_down",       "pad_down",       CTRL_OFF_KEY(keyDown),      0 },
    { "TURN LEFT",   "key_left",       "pad_left",       CTRL_OFF_KEY(keyLeft),      0 },
    { "TURN RIGHT",  "key_right",      "pad_right",      CTRL_OFF_KEY(keyRight),     0 },
    { "STRAFE L",    "key_l1",         "pad_l1",         CTRL_OFF_KEY(keyL1),        CTRL_OFF_PAD(padL1) },
    { "STRAFE R",    "key_r1",         "pad_r1",         CTRL_OFF_KEY(keyR1),        CTRL_OFF_PAD(padR1) },
    { "ACTION",      "key_cross",      "pad_cross",      CTRL_OFF_KEY(keyCross),     CTRL_OFF_PAD(padCross) },
    { "AIM",         "key_r2",         "pad_r2",         CTRL_OFF_KEY(keyR2),        CTRL_OFF_PAD(padR2) },
    { "LIGHT",       "key_circle",     "pad_circle",     CTRL_OFF_KEY(keyCircle),    CTRL_OFF_PAD(padCircle) },
    { "RUN",         "key_square",     "pad_square",     CTRL_OFF_KEY(keySquare),    CTRL_OFF_PAD(padSquare) },
    { "VIEW",        "key_l2",         "pad_l2",         CTRL_OFF_KEY(keyL2),        CTRL_OFF_PAD(padL2) },
    { "ITEM",        "key_select",     "pad_select",     CTRL_OFF_KEY(keySelect),    CTRL_OFF_PAD(padSelect) },
    { "MAP",         "key_triangle",   "pad_triangle",   CTRL_OFF_KEY(keyTriangle),  CTRL_OFF_PAD(padTriangle) },
    { "PAUSE",       "key_start",      "pad_start",      CTRL_OFF_KEY(keyStart),     CTRL_OFF_PAD(padStart) },
    { "RELOAD",      "key_reload",     "pad_reload",     CTRL_OFF_KEY(keyReload),    CTRL_OFF_PAD(padReload) },
    { "QUICK TURN",  "key_quick_turn", "pad_quick_turn", CTRL_OFF_KEY(keyQuickTurn), CTRL_OFF_PAD(padQuickTurn) },
    { "QUICK HEAL",  "key_quick_heal", "pad_quick_heal", CTRL_OFF_KEY(keyQuickHeal), CTRL_OFF_PAD(padQuickHeal) }
};

#define PLUGIN_CONTROL_ACTION_COUNT ((s32)(sizeof(PLUGIN_CONTROL_ACTIONS) / sizeof(PLUGIN_CONTROL_ACTIONS[0])))

static s32 s_pcCtrlTargetMode     = 0; /* 0 = Keyboard, 1 = Gamepad */
static s32 s_pcCtrlSelectedAction = 0;
static s32 s_pcCtrlLeftRow        = 0; /* 0 = Exit, 1 = Mode, 2 = Reset Defaults */
static s32 s_pcCtrlIsOnRightPane  = 0;
static s32 s_pcCtrlIsWaitingInput = 0;
static s32 s_pcCtrlWaitTimer      = 0;

static const char* Plugin_FormatBindName(const char* raw, int mode)
{
    if (!raw || raw[0] == '\0' || strcmp(raw, "NONE") == 0) return "---";
    if (mode == 0)
    {
        if (strcmp(raw, "Return") == 0) return "ENTER";
        if (strcmp(raw, "Left Shift") == 0) return "LSHIFT";
        if (strcmp(raw, "Right Shift") == 0) return "RSHIFT";
        if (strcmp(raw, "Left Ctrl") == 0) return "LCTRL";
        if (strcmp(raw, "Right Ctrl") == 0) return "RCTRL";
        if (strcmp(raw, "Left Alt") == 0) return "LALT";
        if (strcmp(raw, "Right Alt") == 0) return "RALT";
        if (strcmp(raw, "Space") == 0) return "SPACE";
        if (strcmp(raw, "Up") == 0) return "UP ARROW";
        if (strcmp(raw, "Down") == 0) return "DOWN ARROW";
        if (strcmp(raw, "Left") == 0) return "LEFT ARROW";
        if (strcmp(raw, "Right") == 0) return "RIGHT ARROW";
    }
    else
    {
        if (strcmp(raw, "a") == 0) return "A";
        if (strcmp(raw, "b") == 0) return "B";
        if (strcmp(raw, "x") == 0) return "X";
        if (strcmp(raw, "y") == 0) return "Y";
        if (strcmp(raw, "leftshoulder") == 0) return "LB";
        if (strcmp(raw, "rightshoulder") == 0) return "RB";
        if (strcmp(raw, "lefttrigger") == 0) return "LT";
        if (strcmp(raw, "righttrigger") == 0) return "RT";
        if (strcmp(raw, "back") == 0) return "BACK";
        if (strcmp(raw, "start") == 0) return "START";
        if (strcmp(raw, "leftstick") == 0) return "LS CLICK";
        if (strcmp(raw, "rightstick") == 0) return "RS CLICK";
        if (strcmp(raw, "dpup") == 0) return "D-PAD UP";
        if (strcmp(raw, "dpdown") == 0) return "D-PAD DOWN";
        if (strcmp(raw, "dpleft") == 0) return "D-PAD LEFT";
        if (strcmp(raw, "dpright") == 0) return "D-PAD RIGHT";
    }
    return raw;
}

static const char* Plugin_CaptureKey(void)
{
#ifdef _WIN32
    for (int vk = 1; vk < 256; vk++)
    {
        if (GetAsyncKeyState(vk) & 0x8000)
        {
            if (vk == VK_ESCAPE) return NULL;
            if (vk == VK_RETURN) return "Return";
            if (vk == VK_SPACE) return "Space";
            if (vk == VK_LSHIFT) return "Left Shift";
            if (vk == VK_RSHIFT) return "Right Shift";
            if (vk == VK_LCONTROL) return "Left Ctrl";
            if (vk == VK_RCONTROL) return "Right Ctrl";
            if (vk == VK_LMENU) return "Left Alt";
            if (vk == VK_RMENU) return "Right Alt";
            if (vk == VK_UP) return "Up";
            if (vk == VK_DOWN) return "Down";
            if (vk == VK_LEFT) return "Left";
            if (vk == VK_RIGHT) return "Right";
            if (vk >= 'A' && vk <= 'Z')
            {
                static char s_keyChar[2] = {0};
                s_keyChar[0] = (char)vk;
                return s_keyChar;
            }
            if (vk >= '0' && vk <= '9')
            {
                static char s_numChar[2] = {0};
                s_numChar[0] = (char)vk;
                return s_numChar;
            }
        }
    }
#endif
    return NULL;
}

static const char* Plugin_CapturePad(void)
{
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_Cross) return "a";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_Circle) return "b";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_Square) return "x";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_Triangle) return "y";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_L1) return "leftshoulder";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_R1) return "rightshoulder";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_L2) return "lefttrigger";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_R2) return "righttrigger";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_Select) return "back";
    if (g_Controller0->pulsedBtnFlags & ControllerFlag_Start) return "start";
    return NULL;
}

static void Plugin_ControllerMenu_EntriesDraw(bool isOnRightPane, s32 presetsEntryIdx, s32 actionsEntryIdx, s32 boundActionIdx)
{
    const ControlScheme* scheme = &g_PcConfig.classic;
    s32 i;

    /* Left Pane */
    const char* LEFT_LABELS[4] = { "EXIT", "KEYBOARD", "GAMEPAD", "RESET" };

    for (i = 0; i < 4; i++)
    {
        if (!s_pcCtrlIsOnRightPane && s_pcCtrlLeftRow == i)
            Gfx_StringSetColor(StringColorId_Gold);
        else if ((i == 1 && s_pcCtrlTargetMode == 0) || (i == 2 && s_pcCtrlTargetMode == 1))
            Gfx_StringSetColor(StringColorId_Gold);
        else
            Gfx_StringSetColor(StringColorId_White);

        Gfx_StringSetPosition(8, 22 + (i * 22));
        Gfx_StringDraw(LEFT_LABELS[i], 16);
    }

    /* Right Pane Actions List */
    for (i = 0; i < PLUGIN_CONTROL_ACTION_COUNT; i++)
    {
        s32 y = 22 + (i * 11);
        bool isRowSelected = (s_pcCtrlIsOnRightPane && s_pcCtrlSelectedAction == i);

        Text_Debug_PositionSet(135, y);
        Text_Debug_Draw(PLUGIN_CONTROL_ACTIONS[i].label);

        Text_Debug_PositionSet(226, y);
        if (isRowSelected && s_pcCtrlIsWaitingInput)
        {
            if ((g_SysWork.counters_1C[0] & 0x10) != 0)
                Text_Debug_Draw(s_pcCtrlTargetMode == 0 ? "[PRESS KEY]" : "[PRESS BTN]");
            else
                Text_Debug_Draw("           ");
        }
        else
        {
            const char* rawVal = NULL;
            if (s_pcCtrlTargetMode == 0)
                rawVal = (const char*)scheme + PLUGIN_CONTROL_ACTIONS[i].keyOff;
            else
            {
                if (PLUGIN_CONTROL_ACTIONS[i].padOff != 0)
                    rawVal = (const char*)scheme + PLUGIN_CONTROL_ACTIONS[i].padOff;
                else
                    rawVal = "STICK/DPAD";
            }

            const char* val = Plugin_FormatBindName(rawVal, s_pcCtrlTargetMode);
            char buf[16];
            s32 n;
            for (n = 0; n < (s32)sizeof(buf) - 1 && val[n] != '\0'; n++)
            {
                unsigned char c = (unsigned char)toupper((unsigned char)val[n]);
                buf[n] = (c >= '*' && c <= 'i') ? (char)c : ' ';
            }
            buf[n] = '\0';
            Text_Debug_Draw(buf);
        }

        if (isRowSelected && !s_pcCtrlIsWaitingInput)
        {
            Text_Debug_PositionSet(126, y);
            Text_Debug_Draw(">");
        }
    }
}

static void Plugin_ControllerMenu_Control(void)
{
    ControlScheme* activeScheme = &g_PcConfig.classic;

    if (g_GameWork.gameStateSteps[1] == ControllerMenuState_Leave)
    {
        g_GameWork.gameStateSteps[0] = OptionsMenuState_LeaveController;
        g_SysWork.counters_1C[1]     = 0;
        g_GameWork.gameStateSteps[1] = 0;
        g_GameWork.gameStateSteps[2] = 0;
        return;
    }

    if (s_pcCtrlIsWaitingInput)
    {
        if (s_pcCtrlWaitTimer > 0)
        {
            s_pcCtrlWaitTimer--;
        }
        else
        {
#ifdef _WIN32
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                SD_Call(Sfx_MenuCancel);
                s_pcCtrlIsWaitingInput = 0;
                return;
            }
#endif

            if (s_pcCtrlTargetMode == 0)
            {
                const char* newKey = Plugin_CaptureKey();
                if (newKey && newKey[0] != '\0')
                {
                    char* dstField = (char*)activeScheme + PLUGIN_CONTROL_ACTIONS[s_pcCtrlSelectedAction].keyOff;
                    strncpy(dstField, newKey, 23);
                    dstField[23] = '\0';

                    PcConfig_SaveKeyValue(PLUGIN_CONTROL_ACTIONS[s_pcCtrlSelectedAction].keyCfg, newKey);
                    SD_Call(Sfx_MenuConfirm);
                    s_pcCtrlIsWaitingInput = 0;
                }
            }
            else
            {
                if (PLUGIN_CONTROL_ACTIONS[s_pcCtrlSelectedAction].padOff != 0)
                {
                    const char* newPad = Plugin_CapturePad();
                    if (newPad && newPad[0] != '\0')
                    {
                        char* dstField = (char*)activeScheme + PLUGIN_CONTROL_ACTIONS[s_pcCtrlSelectedAction].padOff;
                        strncpy(dstField, newPad, 23);
                        dstField[23] = '\0';

                        PcConfig_SaveKeyValue(PLUGIN_CONTROL_ACTIONS[s_pcCtrlSelectedAction].padCfg, newPad);
                        SD_Call(Sfx_MenuConfirm);
                        s_pcCtrlIsWaitingInput = 0;
                    }
                }
                else
                {
                    SD_Call(Sfx_MenuMove);
                    s_pcCtrlIsWaitingInput = 0;
                }
            }
        }
        Plugin_ControllerMenu_EntriesDraw(s_pcCtrlIsOnRightPane, s_pcCtrlLeftRow, s_pcCtrlSelectedAction, NO_VALUE);
        return;
    }

    /* Navigation */
    if (!s_pcCtrlIsOnRightPane)
    {
        if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickUp)
        {
            s_pcCtrlLeftRow = (s_pcCtrlLeftRow + 3) % 4;
            if (s_pcCtrlLeftRow == 1) s_pcCtrlTargetMode = 0;
            if (s_pcCtrlLeftRow == 2) s_pcCtrlTargetMode = 1;
            SD_Call(Sfx_MenuMove);
        }
        else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickDown)
        {
            s_pcCtrlLeftRow = (s_pcCtrlLeftRow + 1) % 4;
            if (s_pcCtrlLeftRow == 1) s_pcCtrlTargetMode = 0;
            if (s_pcCtrlLeftRow == 2) s_pcCtrlTargetMode = 1;
            SD_Call(Sfx_MenuMove);
        }
        else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickRight)
        {
            s_pcCtrlIsOnRightPane = 1;
            SD_Call(Sfx_MenuMove);
        }
        else if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
        {
            if (s_pcCtrlLeftRow == 0)
            {
                SD_Call(Sfx_MenuCancel);
                g_GameWork.gameStateSteps[1] = ControllerMenuState_Leave;
                g_GameWork.gameStateSteps[2] = 0;
                return;
            }
            else if (s_pcCtrlLeftRow == 1)
            {
                s_pcCtrlTargetMode    = 0;
                s_pcCtrlIsOnRightPane = 1;
                SD_Call(Sfx_MenuConfirm);
            }
            else if (s_pcCtrlLeftRow == 2)
            {
                s_pcCtrlTargetMode    = 1;
                s_pcCtrlIsOnRightPane = 1;
                SD_Call(Sfx_MenuConfirm);
            }
        }
        else if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
        {
            SD_Call(Sfx_MenuCancel);
            g_GameWork.gameStateSteps[1] = ControllerMenuState_Leave;
            g_GameWork.gameStateSteps[2] = 0;
            return;
        }
    }
    else
    {
        if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickUp)
        {
            s_pcCtrlSelectedAction = (s_pcCtrlSelectedAction + PLUGIN_CONTROL_ACTION_COUNT - 1) % PLUGIN_CONTROL_ACTION_COUNT;
            SD_Call(Sfx_MenuMove);
        }
        else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickDown)
        {
            s_pcCtrlSelectedAction = (s_pcCtrlSelectedAction + 1) % PLUGIN_CONTROL_ACTION_COUNT;
            SD_Call(Sfx_MenuMove);
        }
        else if (g_Controller0->pulsedGuiBtnFlags & ControllerFlag_LStickLeft)
        {
            s_pcCtrlIsOnRightPane = 0;
            s_pcCtrlLeftRow       = (s_pcCtrlTargetMode == 0) ? 1 : 2;
            SD_Call(Sfx_MenuMove);
        }
        else if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.enter)
        {
            s_pcCtrlIsWaitingInput = 1;
            s_pcCtrlWaitTimer      = 12;
            SD_Call(Sfx_MenuConfirm);
        }
        else if (g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.cancel)
        {
            s_pcCtrlIsOnRightPane = 0;
            s_pcCtrlLeftRow       = (s_pcCtrlTargetMode == 0) ? 1 : 2;
            SD_Call(Sfx_MenuMove);
        }
    }

    Plugin_ControllerMenu_EntriesDraw(s_pcCtrlIsOnRightPane, s_pcCtrlLeftRow, s_pcCtrlSelectedAction, NO_VALUE);
}

PLUGIN_EXPORT const char* SH_Plugin_GetName(void)
{
    return "Nightmare Mode Overhaul";
}

PLUGIN_EXPORT s32 SH_Plugin_GetApiVersion(void)
{
    return 1;
}

PLUGIN_EXPORT void SH_Plugin_Init(void)
{
    SH_LOG("[NIGHTMARE_PLUGIN] Initialized Nightmare Overhaul Plugin.");
    Plugin_LoadNightmareConfig();
    ApplyShadowStalkerModelOverrides();
    Patch_HideHealthStatus();

#ifdef _WIN32
    void* pCtrl = (void*)GetProcAddress(GetModuleHandleA(NULL), "Options_ControllerMenu_Control");
    if (pCtrl)
    {
        InstallHook64(pCtrl, (void*)Plugin_ControllerMenu_Control);
        SH_LOG("[NIGHTMARE_PLUGIN] Hooked Options_ControllerMenu_Control -> Revamped UI");
    }

    void* pDraw = (void*)GetProcAddress(GetModuleHandleA(NULL), "Options_ControllerMenu_EntriesDraw");
    if (pDraw)
    {
        InstallHook64(pDraw, (void*)Plugin_ControllerMenu_EntriesDraw);
        SH_LOG("[NIGHTMARE_PLUGIN] Hooked Options_ControllerMenu_EntriesDraw -> Revamped UI");
    }
#endif
}

PLUGIN_EXPORT void SH_Plugin_OnNewGame(void)
{
    Plugin_LoadNightmareConfig();
    s_running = 1;
    SH_LOG("[NIGHTMARE_PLUGIN] New Game started in Nightmare Mode.");
    ApplyShadowStalkerModelOverrides();
    Patch_HideHealthStatus();
}

PLUGIN_EXPORT void SH_Plugin_OnMapLoad(s32 mapIdx)
{
    Plugin_LoadNightmareConfig();
    ApplyShadowStalkerModelOverrides();
    Patch_HideHealthStatus();

    const char* mapName = MapRegistry_GetName((e_MapIdx)mapIdx);
    SH_LOG("[NIGHTMARE_PLUGIN] Map loaded: %d (%s)", mapIdx, mapName ? mapName : "map");

    if (g_pMapOverlayHeader != NULL)
    {
        g_pMapOverlayHeader->field_16 = 2;
        g_pMapOverlayHeader->field_17 = 6;
        if (g_pMapOverlayHeader->enviromentSet != NULL)
        {
            g_pMapOverlayHeader->enviromentSet(6, 2);
        }
        SH_LOG("[NIGHTMARE_PLUGIN] Applied dark ambient (field_16 = 2) & rain (field_17 = 6)");
    }
}

static s_CharaModel* s_lastChildModel = NULL;
static q19_12        s_prevHp         = 0;

PLUGIN_EXPORT void SH_Plugin_OnUpdate(void)
{
    /* Always ensure nightmare mode is active when plugin is present */
    g_PcConfig.nightmare = 1;

    /* Patch health indicator in inventory */
    static int s_patchedHealth = 0;
    if (!s_patchedHealth)
    {
        s_patchedHealth = 1;
        Patch_HideHealthStatus();
    }

    /* Enforce darkness & rain while in active gameplay across all maps */
    if (g_SysWork.sysState == SysState_Gameplay &&
        (g_GameWork.gameState == GameState_InGame || g_GameWork.gameState == GameState_MapEvent) &&
        g_pMapOverlayHeader != NULL)
    {
        if (g_pMapOverlayHeader->field_16 != 2 || g_pMapOverlayHeader->field_17 != 6)
        {
            g_pMapOverlayHeader->field_16 = 2;
            g_pMapOverlayHeader->field_17 = 6;
            if (g_pMapOverlayHeader->enviromentSet != NULL)
            {
                g_pMapOverlayHeader->enviromentSet(6, 2);
            }
        }

        /* Nightmare Mode: Grey Children / Mumblers / Stalkers are rendered as translucent shadow stalkers */
        if (WorldGfx_IsCharaModelPresent(Chara_GreyChild))
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[Chara_GreyChild];
            if (m != NULL && m != s_lastChildModel)
            {
                WorldGfx_CharaModelTransparentSet(Chara_GreyChild, true);
                WorldGfx_CharaModelMaterialSet(Chara_GreyChild, BlendMode_Subtractive);
                s_lastChildModel = m;
            }
        }
        else if (WorldGfx_IsCharaModelPresent(Chara_Mumbler))
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[Chara_Mumbler];
            if (m != NULL && m != s_lastChildModel)
            {
                WorldGfx_CharaModelTransparentSet(Chara_Mumbler, true);
                WorldGfx_CharaModelMaterialSet(Chara_Mumbler, BlendMode_Subtractive);
                s_lastChildModel = m;
            }
        }
        else if (WorldGfx_IsCharaModelPresent(Chara_Stalker))
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[Chara_Stalker];
            if (m != NULL && m != s_lastChildModel)
            {
                WorldGfx_CharaModelTransparentSet(Chara_Stalker, true);
                WorldGfx_CharaModelMaterialSet(Chara_Stalker, BlendMode_Subtractive);
                s_lastChildModel = m;
            }
        }
        else
        {
            s_lastChildModel = NULL;
        }

        /* 2x Lethal Combat Damage in any binary build:
         * Double incoming damage buffer and monitor direct health loss */
        if (g_SysWork.playerWork.player.damage.amount > 0)
        {
            g_SysWork.playerWork.player.damage.amount *= 2;
        }

        q19_12 curHp = g_SysWork.playerWork.player.health;
        if (s_prevHp > curHp && s_prevHp > 0 && curHp > 0)
        {
            q19_12 damageDelta = s_prevHp - curHp;
            curHp -= damageDelta; /* Extra 1x delta -> total 2x damage */
            if (curHp < 0) curHp = 0;
            g_SysWork.playerWork.player.health = curHp;
        }
        s_prevHp = g_SysWork.playerWork.player.health;
    }
    else
    {
        s_prevHp = g_SysWork.playerWork.player.health;
    }

    /* Live Real-Time World Simulation:
     * While in Inventory or Map screens, continue running
     * NPC AI updates so enemies advance and stalk Harry in real time. */
    if (g_PcConfig.liveInventory)
    {
        if (g_GameWork.gameState == GameState_InventoryScreen ||
            g_GameWork.gameState == GameState_PaperMapScreen)
        {
            extern void Game_NpcUpdate(void);
            Game_NpcUpdate();
        }
    }
}

PLUGIN_EXPORT void SH_Plugin_OnPlayerDamage(s32* damage)
{
    if (!g_PcConfig.nightmare)
        return;

    if (damage && *damage > 0)
    {
        /* 2x damage multiplier in Nightmare Mode (heavy punishing hit, fair challenge) */
        *damage *= 2;
        if (*damage < Q12(10.0f))
        {
            *damage = Q12(10.0f);
        }
    }
}

PLUGIN_EXPORT int SH_Plugin_ShouldHideHealth(void)
{
    if (!g_PcConfig.nightmare)
        return 0;

    /* Hide health indicator in pause/inventory screens */
    return 1;
}

PLUGIN_EXPORT int SH_Plugin_OverrideWeather(s32* ambient, s32* rain)
{
    if (!g_PcConfig.nightmare)
        return 0;

    if (ambient) *ambient = 2; // Dark Otherworld ambient
    if (rain)    *rain    = 6; // Heavy rain
    return 1;
}

PLUGIN_EXPORT int SH_Plugin_IsNightmarePlugin(void)
{
    return 1;
}

PLUGIN_EXPORT int SH_Plugin_IsLiveInventory(void)
{
    if (!g_PcConfig.nightmare)
        return 0;

    return (g_PcConfig.liveInventory != 0);
}

/* Standalone PSX Vignette Renderer for standalone beta & dev builds */
static POLY_G4 s_vignetteQuads[2][4];
static DR_MODE s_vignetteDrMode[2];
static int     s_vignetteStaticsInited = 0;

static void Plugin_DrawDirectVignette(u8 r, u8 g, u8 b, u8 alpha)
{
    if (alpha == 0) return;
    int buf = g_ActiveBufferIdx;
    if (!s_vignetteStaticsInited)
    {
        s_vignetteStaticsInited = 1;
        for (int b_idx = 0; b_idx < 2; b_idx++)
        {
            SetDrawMode(&s_vignetteDrMode[b_idx], 0, 0, GetTPage(0, 1, 0, 0), 0);
            for (int q = 0; q < 4; q++)
            {
                SetPolyG4(&s_vignetteQuads[b_idx][q]);
                SetSemiTrans(&s_vignetteQuads[b_idx][q], 1);
            }
        }
    }

    short halfH = 120;
    float aspect = 1.333333f;
    if (g_PcConfig.windowWidth > 0 && g_PcConfig.windowHeight > 0)
    {
        aspect = (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight;
    }
    if (aspect < 1.333333f) aspect = 1.333333f;
    short halfW = (short)(120.0f * aspect + 16.0f);

    short borderX = (short)(28.0f * (aspect / 1.333333f));
    short borderY = 30;

    short xL = -halfW;
    short xR =  halfW;
    short yT = -halfH;
    short yB =  halfH;

    short inXL = xL + borderX;
    short inXR = xR - borderX;
    short inYT = yT + borderY;
    short inYB = yB - borderY;

    u8 oR = (u8)((r * alpha) / 255);
    u8 oG = (u8)((g * alpha) / 255);
    u8 oB = (u8)((b * alpha) / 255);

    /* Top Quad */
    POLY_G4* q0 = &s_vignetteQuads[buf][0];
    q0->x0 = xL;   q0->y0 = yT;   q0->r0 = oR; q0->g0 = oG; q0->b0 = oB;
    q0->x1 = xR;   q0->y1 = yT;   q0->r1 = oR; q0->g1 = oG; q0->b1 = oB;
    q0->x2 = inXL; q0->y2 = inYT; q0->r2 = 0;  q0->g2 = 0;  q0->b2 = 0;
    q0->x3 = inXR; q0->y3 = inYT; q0->r3 = 0;  q0->g3 = 0;  q0->b3 = 0;

    /* Bottom Quad */
    POLY_G4* q1 = &s_vignetteQuads[buf][1];
    q1->x0 = inXL; q1->y0 = inYB; q1->r0 = 0;  q1->g0 = 0;  q1->b0 = 0;
    q1->x1 = inXR; q1->y1 = inYB; q1->r1 = 0;  q1->g1 = 0;  q1->b1 = 0;
    q1->x2 = xL;   q1->y2 = yB;   q1->r2 = oR; q1->g2 = oG; q1->b2 = oB;
    q1->x3 = xR;   q1->y3 = yB;   q1->r3 = oR; q1->g3 = oG; q1->b3 = oB;

    /* Left Quad */
    POLY_G4* q2 = &s_vignetteQuads[buf][2];
    q2->x0 = xL;   q2->y0 = yT;   q2->r0 = oR; q2->g0 = oG; q2->b0 = oB;
    q2->x1 = inXL; q2->y1 = inYT; q2->r1 = 0;  q2->g1 = 0;  q2->b1 = 0;
    q2->x2 = xL;   q2->y2 = yB;   q2->r2 = oR; q2->g2 = oG; q2->b2 = oB;
    q2->x3 = inXL; q2->y3 = inYB; q2->r3 = 0;  q2->g3 = 0;  q2->b3 = 0;

    /* Right Quad */
    POLY_G4* q3 = &s_vignetteQuads[buf][3];
    q3->x0 = inXR; q3->y0 = inYT; q3->r0 = 0;  q3->g0 = 0;  q3->b0 = 0;
    q3->x1 = xR;   q3->y1 = yT;   q3->r1 = oR; q3->g1 = oG; q3->b1 = oB;
    q3->x2 = inXR; q3->y2 = inYB; q3->r2 = 0;  q3->g2 = 0;  q3->b2 = 0;
    q3->x3 = xR;   q3->y3 = yB;   q3->r3 = oR; q3->g3 = oG; q3->b3 = oB;

    void* ot = (void*)&g_OtTags0[buf][5];
    AddPrim(ot, &s_vignetteDrMode[buf]);
    AddPrim(ot, q0);
    AddPrim(ot, q1);
    AddPrim(ot, q2);
    AddPrim(ot, q3);
}

static void Plugin_RenderLowHealthEffects(void)
{
    if (!g_PcConfig.nightmare || !g_PcConfig.nightmareVignette)
        return;

    /* Only render the 3D world low-health vignette during active gameplay (never over menus) */
    if (g_GameWork.gameState != GameState_InGame && g_GameWork.gameState != GameState_MapEvent)
        return;

    /* Silent Hill 2 Remake Real-Time Delta Heartbeat Low-Health Edge Vignette */
    if (g_SysWork.playerWork.player.health > Q12(0.0f) && g_SysWork.playerWork.player.health <= Q12(35.0f))
    {
        static q19_12 s_cardiacTimeQ12 = 0;

        q19_12 hp = g_SysWork.playerWork.player.health;
        /* Cardiac Cycle Period in real time (Q12 fixed-point seconds):
         * 35 HP -> 1.15 seconds (~52 BPM, resting dread)
         * 10 HP -> 0.75 seconds (~80 BPM, panic rush) */
        q19_12 periodQ12 = (hp <= Q12(15.0f)) ? Q12(0.75f) : Q12(1.15f);

        /* Advance by real frame delta time (framerate-independent on 60/120/144/240Hz monitors) */
        q19_12 dt = (g_DeltaTime > 0) ? g_DeltaTime : Q12(1.0f / 60.0f);
        s_cardiacTimeQ12 += dt;
        if (s_cardiacTimeQ12 >= periodQ12)
        {
            s_cardiacTimeQ12 %= periodQ12;
        }

        /* Phase in cycle mapped to 0..1000 */
        u32 phase = (u32)((s_cardiacTimeQ12 * 1000) / periodQ12);

        s32 pulse = 0; // 0..4096
        if (phase < 200) // 0% - 20%: Primary systolic beat ("LUB")
        {
            u16 a = (u16)((phase * 2048) / 200); // 0..180 deg
            pulse = Math_Sin(a);
        }
        else if (phase >= 260 && phase < 440) // 26% - 44%: Secondary diastolic beat ("DUB")
        {
            u16 a = (u16)(((phase - 260) * 2048) / 180);
            pulse = (Math_Sin(a) * 6) / 10; // 60% amplitude of primary beat
        }
        else // 44% - 100%: Diastolic Rest Interval (calm pause)
        {
            pulse = 0;
        }

        /* Vivid, atmospheric blood perimeter intensity */
        int baseIntensity  = (hp <= Q12(15.0f)) ? 45 : 28;
        int peakAdd        = (hp <= Q12(15.0f)) ? 115 : 75;
        int finalIntensity = baseIntensity + ((peakAdd * pulse) >> 12);
        finalIntensity     = CLAMP(finalIntensity, 20, 175);

        /* Blood red: Red = finalIntensity, Green = 0, Blue = 0 */
        Plugin_DrawDirectVignette(220, 10, 10, (u8)finalIntensity);
    }
}

/* Primary render callback called every frame by both Beta and dev builds */
PLUGIN_EXPORT void SH_Plugin_OnRender(void)
{
    Plugin_RenderLowHealthEffects();
}

PLUGIN_EXPORT void SH_Plugin_OnScreenFadeDraw(void)
{
    Plugin_RenderLowHealthEffects();
}

PLUGIN_EXPORT int SH_Plugin_OverrideNpcSpawn(e_CharaId* charaId)
{
    if (!charaId) return 0;
    /* Swap street enemies to their Otherworld counterparts */
    if (*charaId == Chara_AirScreamer)
    {
        *charaId = Chara_NightFlutter;
        return 1;
    }
    if (*charaId == Chara_Groaner)
    {
        *charaId = Chara_Wormhead;
        return 1;
    }
    return 0;
}

PLUGIN_EXPORT void SH_Plugin_ModifyRadioVolume(s32* volume)
{
    if (!volume || !g_PcConfig.nightmare) return;
    if (g_SysWork.playerWork.player.health <= Q12(35.0f))
    {
        /* PSX Audio Driver uses Attenuation (0 = Max Loudness, 255 = Silent).
         * Calculate true perceived loudness (255 - volume), attenuate by 60%,
         * and convert back to attenuation byte. */
        s32 loudness = 255 - *volume;
        if (loudness > 0)
        {
            loudness = (loudness * 4) / 10;
            *volume  = 255 - loudness;
        }
    }
}

PLUGIN_EXPORT void SH_Plugin_ModifyRadioAttributes(s32* volume, s32* pitch)
{
    if (!g_PcConfig.nightmare) return;

    q19_12 hp = g_SysWork.playerWork.player.health;
    if (hp <= Q12(35.0f))
    {
        /* 1. Attenuate perceived loudness */
        if (volume)
        {
            s32 loudness = 255 - *volume;
            if (loudness > 0)
            {
                loudness = (loudness * 4) / 10;
                *volume  = 255 - loudness;
            }
        }

        /* 2. Unstable distorted radio static pitch wobble */
        if (pitch)
        {
            static q19_12 s_radioPhase = 0;
            s_radioPhase += Q12(0.08f);
            s32 wobble = (Math_Sin(s_radioPhase) >> 9); /* -8..+8 wobble */
            *pitch = (hp <= Q12(15.0f)) ? (wobble * 3) : (wobble * 2);
        }
    }
}

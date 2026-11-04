/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port: phone motion controls.
 *
 *  - gyro_aim:      the gyroscope steers the alternate cameras (Pc_Gyro_TakeLook).
 *  - gyro_movement: tilting the phone is a left stick (Pc_Gyro_MoveStick), in
 *                   every camera. It goes in where the touch stick does, so each
 *                   control mode reads it the way it reads any stick: tank turns
 *                   in Classic, camera-relative walking and strafing in the
 *                   alternate cameras, screen-relative under 2D controls.
 *                   Classic also runs on a far forward tilt, and a sharp flick
 *                   back is the quick back-jump (Run + Back) outside 2D control.
 *  - gyro_sensitivity scales both.
 * One at a time: the quick menu turns the other off (options.c).
 *  - with either on, a fast wag breaks free of a grab (Pc_Gyro_GrabShakes).
 *
 * The device's own sensors through SDL's sensor API, not a controller's:
 * SDL_HINT_ACCELEROMETER_AS_JOYSTICK is off (main_pc.c), so the phone is not a
 * joystick here. Opened only while one of the options is on, so a phone with
 * both off never powers the sensors.
 *
 * SDL hands both sensors over in the device's NATURAL (portrait) frame and does
 * not rotate them with the display: x to the right edge, y to the top edge, z
 * out of the screen, angular velocity counter-clockwise positive, and the
 * accelerometer reading the reaction to gravity (it points UP at rest). The
 * game is locked to landscape, either way up, so which edge is up is read from
 * gravity rather than from SDL_GetDisplayOrientation, whose landscape naming
 * has not been consistent across SDL's backends.
 */
#include "game.h"
#include "pc_config.h"
#include "sh_log.h"

#include <SDL.h>
#include <math.h>

static SDL_Sensor* s_Gyro;
static SDL_Sensor* s_Accel;
static int         s_Tried;   /* opening failed once this session: do not retry every tick */
static Uint64      s_LastTick;
/* +1: the device's right edge (portrait +x) is up; -1: its left edge is. */
static int         s_UpSign = 1;

static int         s_ShakeSign;
static Uint32      s_GrabbedMs;

/* Movement: forward/back tilt is measured from where the phone was held when
 * movement (re)started, so any comfortable holding angle is "standing still".
 * Side tilt is measured from level, the way a steering wheel is. */
static int         s_MoveCal;
static float       s_NeutralPitch;
static float       s_GravUp, s_GravRight, s_GravOut;
static Uint32      s_MoveLastMs;
static float       s_PrevRawPitch;
static int         s_MoveRun;    /* Classic's auto-run, latched with hysteresis */
static int         s_MoveBack;   /* last read was tilted back past the dead zone */
static Uint32      s_SharpMs;    /* last time the phone was pitching fast */
static Uint32      s_HopUntilMs; /* Run held until then, for the quick back-jump */

/* Below this the reading is sensor noise and resting-hand tremor, not an aim,
 * and integrating it walks the camera off on its own. */
#define GYRO_DEADZONE_RAD_S 0.015f
/* Gravity has to lean this far onto one edge before the up side flips, so a
 * phone held near flat does not swap the axes back and forth. */
#define GYRO_UP_FLIP_MS2    3.0f
/* A deliberate wag, well past any aiming turn (~170 degrees a second). */
#define GYRO_SHAKE_RAD_S    3.0f
/* Tilt: nothing within ~6 degrees of neutral, a full push at ~24 (run, where
 * the control mode runs on a full stick). */
#define GYRO_TILT_DEAD_RAD  0.105f
#define GYRO_TILT_FULL_RAD  0.42f
/* Away from gameplay longer than this (a menu, a cutscene) and the phone has
 * likely been put down or re-gripped, so the next tilt re-centres first. */
#define GYRO_RECAL_GAP_MS   1000
/* Classic's auto-run, on the touch stick's own thresholds (pc_touch.c). */
#define GYRO_RUN_ENGAGE     0.68f
#define GYRO_RUN_RELEASE    0.50f
/* A flick back, not a lean back: pitching at least this fast (~140 degrees a
 * second) within GYRO_HOP_WINDOW_MS of leaving the dead zone backwards. */
#define GYRO_HOP_RAD_S      2.5f
#define GYRO_HOP_WINDOW_MS  200
/* Run is held this long from that moment: past the stick crossing into "back",
 * which is the edge the back-jump fires on, and short enough that Harry walks
 * back after the hop instead of hopping again. */
#define GYRO_HOP_HOLD_MS    300

static void Gyro_Close(void)
{
    if (s_Gyro != NULL)
        SDL_SensorClose(s_Gyro);
    if (s_Accel != NULL)
        SDL_SensorClose(s_Accel);
    s_Gyro      = NULL;
    s_Accel     = NULL;
    s_LastTick  = 0;
    s_ShakeSign = 0;
    s_MoveCal   = 0;
}

/* Sensors open while either option is on, closed otherwise. */
static int Gyro_Ensure(void)
{
    int i, n;

    /* Only a hand-edited config.cfg gets here with both; aiming is the older
     * option, so it is the one kept. */
    if (g_PcConfig.gyroAim != 0 && g_PcConfig.gyroMove)
        g_PcConfig.gyroMove = 0;

    if (g_PcConfig.gyroAim == 0 && !g_PcConfig.gyroMove)
    {
        Gyro_Close();
        s_Tried = 0;
        return 0;
    }
    if (s_Gyro != NULL || s_Accel != NULL)
        return 1;
    if (s_Tried)
        return 0;

    s_Tried = 1;
    if (SDL_InitSubSystem(SDL_INIT_SENSOR) != 0)
    {
        SH_DBG("[GYRO] sensor subsystem unavailable: %s", SDL_GetError());
        return 0;
    }

    n = SDL_NumSensors();
    for (i = 0; i < n; i++)
    {
        SDL_SensorType type = SDL_SensorGetDeviceType(i);

        if (type == SDL_SENSOR_GYRO && s_Gyro == NULL)
            s_Gyro = SDL_SensorOpen(i);
        else if (type == SDL_SENSOR_ACCEL && s_Accel == NULL)
            s_Accel = SDL_SensorOpen(i);
    }
    SH_DBG("[GYRO] %d sensors, gyro %s, accelerometer %s", n, s_Gyro ? "open" : "none",
           s_Accel ? "open" : "none");
    return s_Gyro != NULL || s_Accel != NULL;
}

/* Gravity in the screen's frame (up, right, out), and which edge is up. */
static int Gyro_Gravity(float* up, float* right, float* out)
{
    float data[3];

    if (s_Accel == NULL || SDL_SensorGetData(s_Accel, data, 3) != 0)
        return 0;

    if (data[0] > GYRO_UP_FLIP_MS2)
        s_UpSign = 1;
    else if (data[0] < -GYRO_UP_FLIP_MS2)
        s_UpSign = -1;

    /* Landscape: the screen's up axis is portrait +x or -x, its right axis
     * portrait -y or +y. */
    *up    = (float)s_UpSign * data[0];
    *right = (float)(-s_UpSign) * data[1];
    *out   = data[2];
    return 1;
}

/* Angular velocity about the screen's up and right axes. Counter-clockwise
 * about up turns the view LEFT, and about right tilts it UP. */
static int Gyro_Rates(float* wUp, float* wRight)
{
    float data[3];
    float up, right, out;

    (void)Gyro_Gravity(&up, &right, &out);
    if (s_Gyro == NULL || SDL_SensorGetData(s_Gyro, data, 3) != 0)
        return 0;

    *wUp    = (float)s_UpSign * data[0];
    *wRight = (float)(-s_UpSign) * data[1];
    return 1;
}

/* Rotation since the last call, in radians, in the PLAYER's frame: yaw > 0 turns
 * the view right, pitch > 0 tilts it up. Call every tick so time spent with the
 * camera frozen or the option gated off is drained rather than banked. */
int Pc_Gyro_TakeLook(float* yaw, float* pitch)
{
    Uint64 now;
    float  dt;
    float  wUp, wRight;

    *yaw   = 0.0f;
    *pitch = 0.0f;

    if (!Gyro_Ensure() || g_PcConfig.gyroAim == 0 || s_Gyro == NULL)
    {
        s_LastTick = 0;
        return 0;
    }

    now = SDL_GetPerformanceCounter();
    if (s_LastTick == 0)
    {
        s_LastTick = now;
        return 0;
    }
    dt         = (float)((double)(now - s_LastTick) / (double)SDL_GetPerformanceFrequency());
    s_LastTick = now;
    /* A hitch (loading, the app backgrounded) must not turn one stale reading
     * into a spin. */
    if (dt > 0.1f)
        return 0;

    if (!Gyro_Rates(&wUp, &wRight))
        return 0;

    /* While held, the wag is a struggle, not an aim: the view stays put. */
    if ((SDL_GetTicks() - s_GrabbedMs) < 100)
        return 0;

    if (fabsf(wUp) < GYRO_DEADZONE_RAD_S)
        wUp = 0.0f;
    if (fabsf(wRight) < GYRO_DEADZONE_RAD_S)
        wRight = 0.0f;

    *yaw   = -wUp * dt;
    *pitch = wRight * dt;
    return (*yaw != 0.0f || *pitch != 0.0f);
}

/* Wag reversals since the last call, for the grab escape (player_control.c),
 * which calls this every tick only while Harry is held. That call is also what
 * keeps Pc_Gyro_TakeLook from turning the view with the wag. A gap between
 * calls means a new grab, so a wag made before it is never counted into it. */
int Pc_Gyro_GrabShakes(void)
{
    const Uint32 now = SDL_GetTicks();
    float        wUp, wRight;
    int          shakes = 0;

    if ((now - s_GrabbedMs) > 200)
        s_ShakeSign = 0;
    s_GrabbedMs = now;

    if (!Gyro_Ensure() || !Gyro_Rates(&wUp, &wRight))
        return 0;

    if (fabsf(wUp) >= GYRO_SHAKE_RAD_S)
    {
        const int sign = (wUp > 0.0f) ? 1 : -1;

        if (s_ShakeSign != 0 && sign != s_ShakeSign)
            shakes = 1;
        s_ShakeSign = sign;
    }
    return shakes;
}

/* The quick menu's close: whatever the phone was doing before, movement takes
 * its neutral from how it is held now and aim starts from a clean tick, so a
 * player whose tilt has wandered can fix it by opening and closing the menu. */
void Pc_Gyro_Recentre(void)
{
    s_MoveCal    = 0;
    s_MoveRun    = 0;
    s_MoveBack   = 0;
    s_HopUntilMs = 0;
    s_LastTick   = 0;
}

static float Gyro_TiltAxis(float a)
{
    const float m = fabsf(a);
    float       v;

    if (m < GYRO_TILT_DEAD_RAD)
        return 0.0f;
    v = (m - GYRO_TILT_DEAD_RAD) / (GYRO_TILT_FULL_RAD - GYRO_TILT_DEAD_RAD);
    if (v > 1.0f)
        v = 1.0f;
    return (a < 0.0f) ? -v : v;
}

/* Tilt as a left stick (0..255, 128 centre): tip the top edge away to walk
 * forward, toward you to back up, and lower an edge to go that way. Asked
 * once per pad read, only in gameplay; returns 0 for "not driving". *run is
 * set when Run should be held with it:
 *  - Classic tank controls never run off the stick alone, so a far enough
 *    forward tilt runs there (the other modes already run on a full stick).
 *  - a sharp flick back is Run + Back, the quick back-jump, in every mode but
 *    2D control, where back is just the bottom of the screen. */
int Pc_Gyro_MoveStick(unsigned char* x, unsigned char* y, int* run)
{
    extern int   g_DebugThirdPersonCam;
    const Uint32 now  = SDL_GetTicks();
    const float  sens = g_PcConfig.gyroSensitivity;
    float        up, right, out, pitch, roll, fwd, side, rawPitch, wUp, wRight;
    Uint32       dtMs;

    if (!g_PcConfig.gyroMove || !Gyro_Ensure() || !Gyro_Gravity(&up, &right, &out))
        return 0;

    *run = 0;
    dtMs = now - s_MoveLastMs;
    if (dtMs > GYRO_RECAL_GAP_MS)
    {
        s_MoveCal   = 0;
        s_MoveRun   = 0;
        s_MoveBack  = 0;
        s_HopUntilMs = 0;
    }
    s_MoveLastMs = now;

    /* How fast the phone is pitching: the gyro when there is one, otherwise
     * the raw (unsmoothed) gravity angle's change. */
    rawPitch = atan2f(out, up);
    if (s_Gyro != NULL && Gyro_Rates(&wUp, &wRight))
    {
        if (fabsf(wRight) >= GYRO_HOP_RAD_S)
            s_SharpMs = now;
    }
    else if (s_MoveCal && dtMs > 0 &&
             fabsf(rawPitch - s_PrevRawPitch) * 1000.0f / (float)dtMs >= GYRO_HOP_RAD_S)
    {
        s_SharpMs = now;
    }
    s_PrevRawPitch = rawPitch;

    /* Hand tremor and the accelerometer's own noise, smoothed over ~4 reads. */
    if (!s_MoveCal)
    {
        s_GravUp = up; s_GravRight = right; s_GravOut = out;
    }
    else
    {
        s_GravUp    += (up    - s_GravUp)    * 0.25f;
        s_GravRight += (right - s_GravRight) * 0.25f;
        s_GravOut   += (out   - s_GravOut)   * 0.25f;
    }

    /* Tipping the top edge away lays the phone back, moving gravity from the
     * up axis onto the out axis; lowering the right edge moves it onto -right. */
    pitch = atan2f(s_GravOut, s_GravUp);
    roll  = atan2f(-s_GravRight, sqrtf(s_GravUp * s_GravUp + s_GravOut * s_GravOut));

    if (!s_MoveCal)
    {
        s_NeutralPitch = pitch;
        s_MoveCal      = 1;
        SH_DBG("[GYRO] movement centred at %.0f degrees", (double)(pitch * 57.2958f));
        return 0;
    }

    /* Sensitivity scales the tilt: higher reaches a full push, and Classic's
     * run, with less of it. */
    fwd  = Gyro_TiltAxis((pitch - s_NeutralPitch) * sens);
    side = Gyro_TiltAxis(roll * sens);

    if (fwd < 0.0f && !s_MoveBack && !g_PcConfig.control2d &&
        (now - s_SharpMs) < GYRO_HOP_WINDOW_MS)
        s_HopUntilMs = now + GYRO_HOP_HOLD_MS;
    s_MoveBack = (fwd < 0.0f);

    if (g_DebugThirdPersonCam == 0 && !g_PcConfig.control2d)
        s_MoveRun = fwd >= (s_MoveRun ? GYRO_RUN_RELEASE : GYRO_RUN_ENGAGE);
    else
        s_MoveRun = 0;

    *run = s_MoveRun || (s_HopUntilMs != 0 && (Sint32)(s_HopUntilMs - now) > 0);

    if (fwd == 0.0f && side == 0.0f)
        return 0;

    *x = (unsigned char)(128.0f + side * 127.0f);
    *y = (unsigned char)(128.0f - fwd * 127.0f);
    return 1;
}

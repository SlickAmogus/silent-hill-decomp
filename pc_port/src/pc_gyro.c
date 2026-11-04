/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port: phone gyroscope aiming for the alternate cameras (gyro_aim).
 *
 * The device's own gyroscope and accelerometer through SDL's sensor API, not a
 * controller's: SDL_HINT_ACCELEROMETER_AS_JOYSTICK is off (main_pc.c), so the
 * phone is not a joystick here. Opened only while gyro_aim is on, so a phone
 * with the option off never powers the sensors.
 *
 * SDL hands both sensors over in the device's NATURAL (portrait) frame and does
 * not rotate them with the display: x to the right edge, y to the top edge, z
 * out of the screen, angular velocity counter-clockwise positive. The game is
 * locked to landscape, either way up, so which edge is up is read from gravity
 * rather than from SDL_GetDisplayOrientation, whose landscape naming has not
 * been consistent across SDL's backends.
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
/* Shake-to-escape: left/right reversals of a fast wag, from the latest
 * reading only, so a wag made before a grab is never banked into it. */
static int         s_ShakeSign;
static int         s_Shakes;
static Uint32      s_GrabbedMs;

/* Below this the reading is sensor noise and resting-hand tremor, not an aim,
 * and integrating it walks the camera off on its own. */
#define GYRO_DEADZONE_RAD_S 0.015f
/* Gravity has to lean this far onto one edge before the up side flips, so a
 * phone held near flat does not swap the axes back and forth. */
#define GYRO_UP_FLIP_MS2    3.0f
/* A deliberate wag, well past any aiming turn (~170 degrees a second). */
#define GYRO_SHAKE_RAD_S    3.0f

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
}

static void Gyro_Open(void)
{
    int i, n;

    s_Tried = 1;
    if (SDL_InitSubSystem(SDL_INIT_SENSOR) != 0)
    {
        SH_DBG("[GYRO] sensor subsystem unavailable: %s", SDL_GetError());
        return;
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
}

/* Rotation since the last call, in radians, in the PLAYER's frame: yaw > 0 turns
 * the view right, pitch > 0 tilts it up. Call every tick so time spent with the
 * camera frozen or the option gated off is drained rather than banked. */
int Pc_Gyro_TakeLook(float* yaw, float* pitch)
{
    float  data[3];
    Uint64 now;
    float  dt;
    float  wUp, wRight;

    *yaw     = 0.0f;
    *pitch   = 0.0f;
    s_Shakes = 0;

    if (g_PcConfig.gyroAim == 0)
    {
        Gyro_Close();
        s_Tried = 0;
        return 0;
    }

    if (s_Gyro == NULL)
    {
        if (!s_Tried)
            Gyro_Open();
        if (s_Gyro == NULL)
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

    if (s_Accel != NULL && SDL_SensorGetData(s_Accel, data, 3) == 0)
    {
        if (data[0] > GYRO_UP_FLIP_MS2)
            s_UpSign = 1;
        else if (data[0] < -GYRO_UP_FLIP_MS2)
            s_UpSign = -1;
    }

    if (SDL_SensorGetData(s_Gyro, data, 3) != 0)
        return 0;

    /* Landscape: the screen's up axis is portrait +x or -x, its right axis
     * portrait -y or +y. Counter-clockwise about up turns the view LEFT, and
     * about right tilts it UP. */
    wUp    = (float)s_UpSign * data[0];
    wRight = (float)(-s_UpSign) * data[1];

    if (fabsf(wUp) >= GYRO_SHAKE_RAD_S)
    {
        const int sign = (wUp > 0.0f) ? 1 : -1;

        if (s_ShakeSign != 0 && sign != s_ShakeSign)
            s_Shakes++;
        s_ShakeSign = sign;
    }

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
 * which calls this only while Harry is held; that call is also what keeps
 * Pc_Gyro_TakeLook from turning the view with the wag. */
int Pc_Gyro_GrabShakes(void)
{
    const int n = s_Shakes;

    s_Shakes    = 0;
    s_GrabbedMs = SDL_GetTicks();
    return n;
}

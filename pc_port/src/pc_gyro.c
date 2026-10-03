/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_gyro.c - gyro aim (see pc_gyro.h).
 *
 * Two sources, the same maths: the controller driving the game if it has a
 * gyroscope, else the device's own. Readings go to screen space (x right,
 * y up, z out of the screen toward the player), where:
 *   - pitch is the turn about screen x, local to the device;
 *   - yaw is "player space": the turn about the real vertical, from the
 *     accelerometer's gravity, so turning the body works however the phone is
 *     tilted, while a turn about screen y or a roll still counts in full.
 *     Without an accelerometer it falls back to the turn about screen y.
 */

#include <math.h>
#include <string.h>

#include <SDL.h>

#include "pc_config.h"
#include "pc_gyro.h"
#include "sh_log.h"

extern const char* PsyX_Pad_ConnectedControllerName(void);

/* Slower turns than this are scaled down towards zero, so the hand's tremor
 * and the sensor's noise leave the view still while a deliberate turn passes
 * at full rate. */
#define GYRO_TIGHTEN_RAD_S 0.035f
/* Per-reading weight of a new accelerometer sample in the gravity estimate. */
#define GYRO_GRAVITY_LP    0.1f
/* GyroWiki's "yaw relax": how much of a tilted turn about the vertical counts
 * before it is capped by the turn's actual size. */
#define GYRO_YAW_RELAX     1.41f
#define GYRO_RESCAN_MS     1000u

static int                 s_subsystem;
static SDL_Sensor*         s_devGyro;
static SDL_Sensor*         s_devAccel;
static SDL_GameController* s_pad;
static Uint32              s_rescanMs;
static float               s_up[3];
static int                 s_upValid;

/* A controller reports its sensors in screen space already; a phone reports
 * them in its natural (portrait) frame, so they turn with the display. */
static void Gyro_DeviceToScreen(const float* d, float* s)
{
    switch (SDL_GetDisplayOrientation(0))
    {
        case SDL_ORIENTATION_LANDSCAPE:         s[0] = -d[1]; s[1] =  d[0]; break; /* right side up */
        case SDL_ORIENTATION_LANDSCAPE_FLIPPED: s[0] =  d[1]; s[1] = -d[0]; break;
        case SDL_ORIENTATION_PORTRAIT_FLIPPED:  s[0] = -d[0]; s[1] = -d[1]; break;
        default:                                s[0] =  d[0]; s[1] =  d[1]; break;
    }
    s[2] = d[2];
}

static void Gyro_PadSensors(SDL_GameController* gc, SDL_bool on)
{
    SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, on);
    if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL))
        SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, on);
}

static void Gyro_DropPad(void)
{
    if (s_pad)
    {
        if (SDL_GameControllerGetAttached(s_pad))
            Gyro_PadSensors(s_pad, SDL_FALSE);
        s_pad = NULL;
    }
}

static void Gyro_CloseAll(void)
{
    Gyro_DropPad();
    if (s_devGyro)
    {
        SDL_SensorClose(s_devGyro);
        s_devGyro = NULL;
    }
    if (s_devAccel)
    {
        SDL_SensorClose(s_devAccel);
        s_devAccel = NULL;
    }
    s_upValid  = 0;
    s_rescanMs = 0;
}

/* The pad that drives the game, if it has a gyroscope. PsyCross opened it, so
 * this only borrows the handle by instance id. */
static SDL_GameController* Gyro_FindPad(void)
{
    const char* active = PsyX_Pad_ConnectedControllerName();
    int         i;

    if (active == NULL)
        return NULL;
    for (i = 0; i < SDL_NumJoysticks(); i++)
    {
        SDL_GameController* gc;
        const char*         name;

        if (!SDL_IsGameController(i))
            continue;
        gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (gc == NULL || !SDL_GameControllerGetAttached(gc))
            continue;
        name = SDL_GameControllerName(gc);
        if (name && strcmp(name, active) == 0 && SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO))
            return gc;
    }
    return NULL;
}

static void Gyro_OpenDevice(void)
{
    int i;

    if (!s_subsystem)
    {
        s_subsystem = 1;
        if (!SDL_WasInit(SDL_INIT_SENSOR) && SDL_InitSubSystem(SDL_INIT_SENSOR) != 0)
            SH_DBG("[GYRO] sensor subsystem: %s", SDL_GetError());
    }
    if (!SDL_WasInit(SDL_INIT_SENSOR))
        return;
    for (i = 0; i < SDL_NumSensors(); i++)
    {
        const SDL_SensorType t = SDL_SensorGetDeviceType(i);
        if (t == SDL_SENSOR_GYRO && s_devGyro == NULL)
        {
            s_devGyro = SDL_SensorOpen(i);
            SH_DBG("[GYRO] device gyroscope: %s", s_devGyro ? SDL_SensorGetDeviceName(i) : SDL_GetError());
        }
        else if (t == SDL_SENSOR_ACCEL && s_devAccel == NULL)
        {
            s_devAccel = SDL_SensorOpen(i);
        }
    }
}

static void Gyro_Rescan(void)
{
    const Uint32        now = SDL_GetTicks();
    SDL_GameController* pad;

    if (s_rescanMs != 0 && now - s_rescanMs < GYRO_RESCAN_MS &&
        (s_pad == NULL || SDL_GameControllerGetAttached(s_pad)))
        return;
    s_rescanMs = now;

    pad = Gyro_FindPad();
    if (pad != s_pad)
    {
        Gyro_DropPad();
        s_pad     = pad;
        s_upValid = 0;
        if (s_pad)
        {
            Gyro_PadSensors(s_pad, SDL_TRUE);
            SH_DBG("[GYRO] controller gyroscope: %s", SDL_GameControllerName(s_pad));
        }
    }
    if (s_pad == NULL && s_devGyro == NULL)
        Gyro_OpenDevice();
}

static void Gyro_FeedGravity(const float* a)
{
    const float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    int         k;

    if (len < 1.0f)
        return;
    if (!s_upValid)
    {
        for (k = 0; k < 3; k++)
            s_up[k] = a[k] / len;
        s_upValid = 1;
        return;
    }
    for (k = 0; k < 3; k++)
        s_up[k] += (a[k] / len - s_up[k]) * GYRO_GRAVITY_LP;
}

int Pc_Gyro_TakeLook(float dt, int aiming, float* yaw, float* pitch)
{
    const int mode = g_PcConfig.gyroAim;
    float     w[3], a[3], raw[3], left, up, mag;

    *yaw   = 0.0f;
    *pitch = 0.0f;

    if (mode == GyroAim_Off)
    {
        Gyro_CloseAll();
        return 0;
    }
    Gyro_Rescan();

    if (s_pad)
    {
        if (SDL_GameControllerGetSensorData(s_pad, SDL_SENSOR_GYRO, w, 3) != 0)
            return 0;
        if (SDL_GameControllerGetSensorData(s_pad, SDL_SENSOR_ACCEL, a, 3) == 0)
            Gyro_FeedGravity(a);
    }
    else if (s_devGyro)
    {
        if (SDL_SensorGetData(s_devGyro, raw, 3) != 0)
            return 0;
        Gyro_DeviceToScreen(raw, w);
        if (s_devAccel && SDL_SensorGetData(s_devAccel, raw, 3) == 0)
        {
            Gyro_DeviceToScreen(raw, a);
            Gyro_FeedGravity(a);
        }
    }
    else
    {
        return 0;
    }

    /* Gravity keeps tracking while the gyro is idle, so the first aimed frame
     * already knows which way is up. */
    if (mode == GyroAim_Aiming && !aiming)
        return 0;
    if (dt <= 0.0f || dt > 0.1f)
        return 0;

    /* Right-hand rule: a positive turn about up is to the left, a positive
     * turn about screen x tips the top edge toward the player, which looks up. */
    if (s_upValid)
    {
        const float world = w[0] * s_up[0] + w[1] * s_up[1] + w[2] * s_up[2];
        const float cap   = sqrtf(w[1] * w[1] + w[2] * w[2]);
        float       v     = fabsf(world) * GYRO_YAW_RELAX;
        if (v > cap)
            v = cap;
        left = (world < 0.0f) ? -v : v;
    }
    else
    {
        left = w[1];
    }
    up = w[0];

    mag = sqrtf(left * left + up * up);
    if (mag < GYRO_TIGHTEN_RAD_S)
    {
        left *= mag / GYRO_TIGHTEN_RAD_S;
        up   *= mag / GYRO_TIGHTEN_RAD_S;
    }

    *yaw   = -left * dt * g_PcConfig.gyroSensitivity;
    *pitch = (g_PcConfig.gyroInvertY ? -up : up) * dt * g_PcConfig.gyroSensitivity;
    return 1;
}

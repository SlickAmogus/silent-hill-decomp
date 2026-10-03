/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_GYRO_H
#define PC_GYRO_H

/* Gyro aim: the phone's own gyroscope, or a controller's (DualShock 4,
 * DualSense, Switch Pro...), turned into camera yaw and pitch.
 *
 * gyro_aim: 0 off, 1 while aiming, 2 always. Only the cameras that can be
 * turned use it (Thirdperson, Over the Shoulder, First Person). */

enum
{
    GyroAim_Off    = 0,
    GyroAim_Aiming = 1,
    GyroAim_Always = 2
};

#ifdef __cplusplus
extern "C" {
#endif

/* The turn the device made over the last dt seconds, in radians, already
 * scaled by gyro_sensitivity: +yaw turns the view right, +pitch looks up.
 * Returns 0 (and zeros) when gyro aim is off, inactive this frame, or there
 * is no gyroscope. Call once per game frame. */
int Pc_Gyro_TakeLook(float dt, int aiming, float* yaw, float* pitch);

#ifdef __cplusplus
}
#endif

#endif /* PC_GYRO_H */

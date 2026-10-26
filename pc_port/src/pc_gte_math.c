/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <libgte.h>

/* PSY-Q Q12 cross product. The flashlight's relative-angle calculation uses
 * both results immediately; leaving this as a no-op makes its normal and
 * side-step vectors uninitialized, so turning Harry randomly changes the
 * flare's angle and intensity. Compute all components before writing so the
 * output may alias either input. */
void OuterProduct12(VECTOR* v0, VECTOR* v1, VECTOR* v2)
{
    const int x = (int)(((int64_t)v0->vy * v1->vz - (int64_t)v0->vz * v1->vy) >> 12);
    const int y = (int)(((int64_t)v0->vz * v1->vx - (int64_t)v0->vx * v1->vz) >> 12);
    const int z = (int)(((int64_t)v0->vx * v1->vy - (int64_t)v0->vy * v1->vx) >> 12);

    v2->vx = x;
    v2->vy = y;
    v2->vz = z;
}

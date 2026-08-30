/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Generated textures for the online client - see sh_net_art.h. No game
 * headers, so online_server/ghost_art_dump.c can link the same code and turn
 * these into PNGs for inspection. */

#include <math.h>
#include <stdlib.h>

#include "sh_net_art.h"

static float ShNetArt_SegDist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t    = (len2 > 0.0001f) ? ((wx * vx + wy * vy) / len2) : 0.0f;
    float dx, dy;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    dx = px - (ax + vx * t);
    dy = py - (ay + vy * t);
    return sqrtf(dx * dx + dy * dy);
}

/* Negative inside the body, positive outside - a union of capsules that reads
 * as a person from the front. Coordinates are in the 64x128 texture's own
 * pixel space. */
static float ShNetArt_BodySdf(float x, float y)
{
    float d = sqrtf((x - 32.0f) * (x - 32.0f) + (y - 20.0f) * (y - 20.0f)) - 10.0f;

    { float t = ShNetArt_SegDist(x, y, 32.0f, 34.0f, 32.0f, 68.0f) - 9.5f;  if (t < d) d = t; }
    { float t = ShNetArt_SegDist(x, y, 23.0f, 38.0f, 17.0f, 74.0f) - 4.0f;  if (t < d) d = t; }
    { float t = ShNetArt_SegDist(x, y, 41.0f, 38.0f, 47.0f, 74.0f) - 4.0f;  if (t < d) d = t; }
    { float t = ShNetArt_SegDist(x, y, 27.0f, 68.0f, 25.0f, 122.0f) - 5.5f; if (t < d) d = t; }
    { float t = ShNetArt_SegDist(x, y, 37.0f, 68.0f, 39.0f, 122.0f) - 5.5f; if (t < d) d = t; }
    return d;
}

unsigned char* ShNetArt_MakeGhost(void)
{
    unsigned char* px = (unsigned char*)malloc(SHNET_ART_GHOST_W * SHNET_ART_GHOST_H * 4);
    int            x, y;

    if (!px)
    {
        return NULL;
    }

    for (y = 0; y < SHNET_ART_GHOST_H; y++)
    {
        for (x = 0; x < SHNET_ART_GHOST_W; x++)
        {
            float d = ShNetArt_BodySdf((float)x + 0.5f, (float)y + 0.5f);
            float band;
            float a;
            int   o = (y * SHNET_ART_GHOST_W + x) * 4;

            /* The contour: brightest on the zero crossing, falling off over
             * about three pixels each way. This is the hollow outline. */
            band = 1.0f - (fabsf(d) / 3.0f);
            if (band < 0.0f) band = 0.0f;
            a = band * band * 240.0f;

            /* A faint interior, brighter at the head, so the figure still
             * reads as a body once the contour is only a pixel or two wide.
             * Low enough that it is never a solid silhouette. */
            if (d < 0.0f)
            {
                float fill = 30.0f + 22.0f * (1.0f - (float)y / (float)SHNET_ART_GHOST_H);
                if (fill > a) a = fill;
            }
            if (a > 255.0f) a = 255.0f;

            px[o + 0] = 255;
            px[o + 1] = 255;
            px[o + 2] = 255;
            px[o + 3] = (unsigned char)a;
        }
    }
    return px;
}

unsigned char* ShNetArt_MakeMarker(void)
{
    unsigned char* px = (unsigned char*)malloc(SHNET_ART_MEMO_W * SHNET_ART_MEMO_H * 4);
    int            x, y;

    if (!px)
    {
        return NULL;
    }

    for (y = 0; y < SHNET_ART_MEMO_H; y++)
    {
        for (x = 0; x < SHNET_ART_MEMO_W; x++)
        {
            float fx = (float)x + 0.5f - 32.0f;
            float fy = (float)y + 0.5f - 32.0f;
            float r  = sqrtf(fx * fx + fy * fy);
            float a  = 0.0f;
            int   o  = (y * SHNET_ART_MEMO_W + x) * 4;

            {
                float ring = 1.0f - (fabsf(r - 24.0f) / 2.5f);
                if (ring > 0.0f) a = ring * ring * 255.0f;
            }
            {
                float inner = 1.0f - (fabsf(r - 10.0f) / 2.0f);
                float v     = inner > 0.0f ? inner * inner * 200.0f : 0.0f;
                if (v > a) a = v;
            }
            /* Ticks at the cardinals, spanning the gap between the rings. */
            if (r > 10.0f && r < 24.0f)
            {
                float tick = 0.0f;
                if (fabsf(fx) < 1.8f) tick = 1.0f - fabsf(fx) / 1.8f;
                if (fabsf(fy) < 1.8f)
                {
                    float t2 = 1.0f - fabsf(fy) / 1.8f;
                    if (t2 > tick) tick = t2;
                }
                if (tick * 185.0f > a) a = tick * 185.0f;
            }
            if (a > 255.0f) a = 255.0f;

            px[o + 0] = 255;
            px[o + 1] = 255;
            px[o + 2] = 255;
            px[o + 3] = (unsigned char)a;
        }
    }
    return px;
}

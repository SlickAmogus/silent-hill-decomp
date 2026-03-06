/*
 * version.h - PC port override
 *
 * Same as the original but ensures VER_USA is the default for PC builds.
 */
#ifndef _VERSION_H_
#define _VERSION_H_

#define VERSION_IS(release) defined(VER_##release)
#define VERSION_REGION_IS(region) defined(VERSION_##region)
#define VERSION_EQUAL_OR_NEWER(release) (VERSION_DATE >= VERSION_DATE_##release)
#define VERSION_EQUAL_OR_OLDER(release) (VERSION_DATE <= VERSION_DATE_##release)

#define VERSION_DATE_PROTO_981216  981216
#define VERSION_DATE_JAP0          990126
#define VERSION_DATE_USA           990210
#define VERSION_DATE_JAP1          990602
#define VERSION_DATE_EUR           990607
#define VERSION_DATE_JAP2          990616

#if defined(VER_JAP0)
    #define VERSION_NTSCJ 1
    #define VERSION_DATE VERSION_DATE_JAP0
    #define VERSION_SERIAL "SLPM-86192"
#elif defined(VER_USA)
    #define VERSION_NTSC 1
    #define VERSION_DATE VERSION_DATE_USA
    #define VERSION_SERIAL "SLUS-00707"
#elif defined(VER_JAP1)
    #define VERSION_NTSCJ 1
    #define VERSION_DATE VERSION_DATE_JAP1
    #define VERSION_SERIAL "SLPM-86498"
#elif defined(VER_EUR)
    #define VERSION_PAL 1
    #define VERSION_DATE VERSION_DATE_EUR
    #define VERSION_SERIAL "SLES-01514"
#elif defined(VER_JAP2)
    #define VERSION_NTSCJ 1
    #define VERSION_DATE VERSION_DATE_JAP2
    #define VERSION_SERIAL "SLPM-87029"
#else
    /* Default to USA for PC builds */
    #ifndef VER_USA
    #define VER_USA 1
    #endif
    #define VERSION_NTSC 1
    #define VERSION_DATE VERSION_DATE_USA
    #define VERSION_SERIAL "SLUS-00707"
#endif

#endif

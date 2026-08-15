/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ios_paths.m — where the game data lives on iOS.
 *
 * An iOS app starts with its working directory at the bundle root, which is
 * read-only and code-signed. main_pc.c chdir()s to Documents at startup so the
 * port's existing relative paths (config.cfg, gamedata/, saves, SilentHill.log)
 * keep working unchanged.
 *
 * Documents specifically, not Library or the SDL pref path: with
 * UIFileSharingEnabled and LSSupportsOpeningDocumentsInPlace set in
 * Info.plist, Documents is the directory Files.app exposes, which is how the
 * user's own disc image gets onto the device at all.
 */
#import <Foundation/Foundation.h>

const char* Ios_DocumentsPath(void)
{
    /* Returned to C and used for the process lifetime, so it must outlive the
     * autorelease pool this runs inside. Resolved once and kept. */
    static char s_path[1024];
    static int  s_resolved = 0;

    if (s_resolved)
    {
        return s_path[0] ? s_path : NULL;
    }
    s_resolved = 1;

    @autoreleasepool
    {
        NSArray<NSString*>* dirs =
            NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        if (dirs.count == 0)
        {
            return NULL;
        }

        const char* utf8 = [dirs.firstObject fileSystemRepresentation];
        if (utf8 == NULL || strlen(utf8) >= sizeof(s_path))
        {
            return NULL;
        }
        strcpy(s_path, utf8);
    }

    return s_path;
}

/* Read-only files shipped inside the signed bundle (the port's own assets).
 * Separate from Documents: the bundle cannot be written to. */
const char* Ios_BundleResourcePath(void)
{
    static char s_path[1024];
    static int  s_resolved = 0;

    if (s_resolved)
    {
        return s_path[0] ? s_path : NULL;
    }
    s_resolved = 1;

    @autoreleasepool
    {
        NSString* res = [[NSBundle mainBundle] resourcePath];
        if (res == nil)
        {
            return NULL;
        }

        const char* utf8 = [res fileSystemRepresentation];
        if (utf8 == NULL || strlen(utf8) >= sizeof(s_path))
        {
            return NULL;
        }
        strcpy(s_path, utf8);
    }

    return s_path;
}

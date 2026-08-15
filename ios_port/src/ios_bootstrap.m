/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ios_bootstrap.m — stage the port's own shipped assets into Documents.
 *
 * pc_port/assets/ (decal.png, the fonts, the language pack, UI sounds) is
 * copied next to the executable by CMake on desktop. Inside a signed .app that
 * tree is read-only, and the game resolves everything relative to the working
 * directory, which main_pc.c points at Documents. So the bundled copies are
 * staged across on first run.
 *
 * Existing files are never overwritten: the user may have edited config.cfg or
 * dropped in a replacement language pack, and this runs on every launch.
 *
 * This is the counterpart of SilentHillActivity.copyAssetTree() on Android,
 * which does the same job out of the APK's AssetManager.
 *
 * The disc image is deliberately NOT part of this. Nothing copyrighted ships in
 * the bundle; the user supplies their own BIN/CUE through Files.app.
 */
#import <Foundation/Foundation.h>

const char* Ios_DocumentsPath(void);
const char* Ios_BundleResourcePath(void);

static void StageTree(NSFileManager* fm, NSString* srcRoot, NSString* dstRoot, NSString* rel)
{
    NSString* src = [srcRoot stringByAppendingPathComponent:rel];
    NSString* dst = [dstRoot stringByAppendingPathComponent:rel];

    BOOL srcIsDir = NO;
    if (![fm fileExistsAtPath:src isDirectory:&srcIsDir])
    {
        return;
    }

    if (!srcIsDir)
    {
        if ([fm fileExistsAtPath:dst])
        {
            return;
        }
        NSError* err = nil;
        [fm createDirectoryAtPath:[dst stringByDeletingLastPathComponent]
      withIntermediateDirectories:YES
                       attributes:nil
                            error:&err];
        if (![fm copyItemAtPath:src toPath:dst error:&err])
        {
            NSLog(@"[SH] could not stage %@: %@", rel, err.localizedDescription);
        }
        return;
    }

    NSError* err = nil;
    NSArray<NSString*>* children = [fm contentsOfDirectoryAtPath:src error:&err];
    for (NSString* child in children)
    {
        StageTree(fm, srcRoot, dstRoot, [rel stringByAppendingPathComponent:child]);
    }
}

void Ios_StageBundledAssets(void)
{
    @autoreleasepool
    {
        const char* docs   = Ios_DocumentsPath();
        const char* bundle = Ios_BundleResourcePath();
        if (docs == NULL || bundle == NULL)
        {
            return;
        }

        NSFileManager* fm     = [NSFileManager defaultManager];
        NSString*      srcRoot = [fm stringWithFileSystemRepresentation:bundle length:strlen(bundle)];
        NSString*      dstRoot = [fm stringWithFileSystemRepresentation:docs length:strlen(docs)];

        /* Mirrors what the Android activity stages. config.cfg is listed
         * separately because it sits at the root rather than inside a tree. */
        StageTree(fm, srcRoot, dstRoot, @"gamedata");
        StageTree(fm, srcRoot, dstRoot, @"licenses");
        StageTree(fm, srcRoot, dstRoot, @"config.cfg");
    }
}

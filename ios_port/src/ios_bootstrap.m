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

/* Factory reset for the settings. There is no full config writer in the port
 * (only per-key saves), but iOS ships a pristine config.cfg inside the signed,
 * read-only bundle, so restoring it is just a copy over the staged one --
 * genuinely the file the app was built with, not a guess at what the defaults
 * were. Returns 1 on success.
 *
 * The caller reloads afterwards; rows the menu marks as needing a restart still
 * need one, exactly as they would after editing the file by hand. */
int Ios_RestoreDefaultConfig(void)
{
    int ok = 0;

    @autoreleasepool
    {
        const char* docs   = Ios_DocumentsPath();
        const char* bundle = Ios_BundleResourcePath();
        if (docs == NULL || bundle == NULL)
        {
            return 0;
        }

        NSFileManager* fm  = [NSFileManager defaultManager];
        NSString*      src = [[fm stringWithFileSystemRepresentation:bundle length:strlen(bundle)]
                                stringByAppendingPathComponent:@"config.cfg"];
        NSString*      dst = [[fm stringWithFileSystemRepresentation:docs length:strlen(docs)]
                                stringByAppendingPathComponent:@"config.cfg"];

        if (![fm fileExistsAtPath:src])
        {
            NSLog(@"[SH] no bundled config.cfg to restore from");
            return 0;
        }

        NSError* err = nil;
        if ([fm fileExistsAtPath:dst])
        {
            [fm removeItemAtPath:dst error:&err];
        }
        ok = [fm copyItemAtPath:src toPath:dst error:&err] ? 1 : 0;
        if (!ok)
        {
            NSLog(@"[SH] config restore failed: %@", err.localizedDescription);
        }
    }

    return ok;
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

/* A PSX memory card is a file, and nothing in the port ever creates one:
 * MemCardFormat is an unimplemented stub, and MemCardExist just fopen()s
 * "<chan>.MCD" relative to the working directory. With no card on disk the
 * save screen sits on "checking the memory card" forever, because the check it
 * is waiting on can never succeed.
 *
 * So lay down a blank formatted card on first run, the way a console ships with
 * one in the slot. 128 KB = 1024 frames x 128 bytes; frame 0 carries the "MC"
 * magic MemCardAccept tests for, and the rest stays zeroed, which is what an
 * empty card looks like to the directory walk in MemCardOpen.
 *
 * Never overwrites an existing card -- that would erase the player's saves. */
void Ios_EnsureMemoryCard(void)
{
    @autoreleasepool
    {
        const char* docs = Ios_DocumentsPath();
        if (docs == NULL)
        {
            return;
        }

        NSFileManager* fm = [NSFileManager defaultManager];
        NSString*      dir = [fm stringWithFileSystemRepresentation:docs length:strlen(docs)];

        for (int chan = 0; chan < 2; chan++)
        {
            NSString* path = [dir stringByAppendingPathComponent:
                                 [NSString stringWithFormat:@"%d.MCD", chan]];

            if ([fm fileExistsAtPath:path])
            {
                continue;
            }

            NSMutableData* card = [NSMutableData dataWithLength:128 * 1024];
            unsigned char* p    = (unsigned char*)card.mutableBytes;
            p[0] = 'M';
            p[1] = 'C';

            if (![card writeToFile:path atomically:YES])
            {
                NSLog(@"[SH] could not create %@", path.lastPathComponent);
            }
        }
    }
}

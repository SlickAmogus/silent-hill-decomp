/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ios_ra_http.m - iOS implementation of the RetroAchievements HTTP transport.
 *
 * Replaces pc_ra_http.c wholesale on iOS (the CMake list filters that file out
 * there): its two backends are WinHTTP and libcurl, and an iPhone has neither.
 * NSURLSession is the only HTTP stack an App Store-shaped process is expected
 * to use, and it is the one App Transport Security is configured against, so
 * going through it means retroachievements.org's TLS just works with no
 * bundled CA store and no exception in Info.plist.
 *
 * Same contract as pc_ra_http.c: blocking, called only from the SH_RA_HTTP
 * worker thread. NSURLSession is asynchronous, so the completion handler
 * signals a semaphore this function waits on -- which is safe precisely
 * because it is never the main thread. Blocking the main thread here would
 * deadlock, since the delegate queue is a background queue but UIKit is not.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "pc_ra_http.h"
#include "pc_qol_mods.h"

void* Pc_RaSymbolLookup(const char* name)
{
    /* Mach-O, unlike the PE build's -Wl,--export-all-symbols, only resolves
     * what the link kept in the export table -- see -Wl,-export_dynamic in
     * pc_port/CMakeLists.txt, without which this returns NULL for everything
     * and the address map comes up empty. */
    return dlsym(RTLD_DEFAULT, name);
}

int Pc_RaHttpRequest(const char* url, const char* post, char** out_body, size_t* out_len)
{
    __block int    status  = 0;
    __block char*  body    = NULL;
    __block size_t bodyLen = 0;

    *out_body = NULL;
    if (out_len)
        *out_len = 0;

    if (!url || !url[0])
        return 0;

    @autoreleasepool
    {
        NSString* urlStr = [NSString stringWithUTF8String:url];
        NSURL*    nsUrl  = urlStr ? [NSURL URLWithString:urlStr] : nil;

        if (!nsUrl)
            return 0;

        NSMutableURLRequest* req =
            [NSMutableURLRequest requestWithURL:nsUrl
                                    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                timeoutInterval:30.0];

        [req setValue:@"SilentHilliOS/1.0" forHTTPHeaderField:@"User-Agent"];

        if (post && post[0])
        {
            NSData* bodyData = [[NSString stringWithUTF8String:post]
                                   dataUsingEncoding:NSUTF8StringEncoding];
            req.HTTPMethod = @"POST";
            req.HTTPBody   = bodyData;
            [req setValue:@"application/x-www-form-urlencoded"
                forHTTPHeaderField:@"Content-Type"];
        }

        dispatch_semaphore_t done = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task =
            [[NSURLSession sharedSession] dataTaskWithRequest:req
                completionHandler:^(NSData* data, NSURLResponse* response, NSError* error)
        {
            if (!error && [response isKindOfClass:[NSHTTPURLResponse class]])
                status = (int)((NSHTTPURLResponse*)response).statusCode;

            /* rc_client reads the body as a C string, so it is NUL-terminated
             * here rather than trusting the response to be text. A body is
             * handed back even on an error status: the RA API returns its
             * failure reasons as JSON with a 4xx. */
            if (data)
            {
                bodyLen = (size_t)data.length;
                body    = (char*)malloc(bodyLen + 1);
                if (body)
                {
                    memcpy(body, data.bytes, bodyLen);
                    body[bodyLen] = '\0';
                }
                else
                {
                    bodyLen = 0;
                }
            }

            dispatch_semaphore_signal(done);
        }];

        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

        /* This target is built without ARC, so the create above has to be
         * balanced by hand or every RA request leaks a semaphore. Guarded so
         * the file still compiles if ARC is ever turned on, where the call is
         * forbidden rather than merely unnecessary. */
#if !__has_feature(objc_arc)
        dispatch_release(done);
#endif
    }

    if (body)
    {
        *out_body = body;
        if (out_len)
            *out_len = bodyLen;
    }

    return status;
}

/* The Essential QoL Mods installer (pc_qol_mods.c), on its worker thread.
 * A download task rather than the data task above: it writes to disk as it
 * goes, so a texture pack never has to fit in memory. The wait is sliced so
 * the byte count can be published for the progress bar. */
int Pc_QolHttpDownload(const char* url, const char* path, volatile long long* got)
{
    __block int status = 0;

    if (!url || !url[0] || !path || !path[0])
        return 0;

    @autoreleasepool
    {
        NSURL*    nsUrl  = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        NSString* dest   = [NSString stringWithUTF8String:path];
        NSString* destAbs;

        if (!nsUrl || !dest)
            return 0;
        /* The game's paths are relative to the working directory (Documents);
         * NSFileManager wants it spelled out. */
        destAbs = [dest isAbsolutePath] ? dest
                : [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:dest];

        NSMutableURLRequest* req =
            [NSMutableURLRequest requestWithURL:nsUrl
                                    cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                timeoutInterval:60.0];
        [req setValue:@"SilentHilliOS/1.0" forHTTPHeaderField:@"User-Agent"];

        dispatch_semaphore_t done = dispatch_semaphore_create(0);

        NSURLSessionDownloadTask* task =
            [[NSURLSession sharedSession] downloadTaskWithRequest:req
                completionHandler:^(NSURL* location, NSURLResponse* response, NSError* error)
        {
            if (!error && [response isKindOfClass:[NSHTTPURLResponse class]])
                status = (int)((NSHTTPURLResponse*)response).statusCode;

            /* The temporary file is deleted when this handler returns, so it
             * has to be moved now, not after the wait below. */
            if (status == 200 && location)
            {
                NSFileManager* fm = [NSFileManager defaultManager];
                [fm removeItemAtPath:destAbs error:nil];
                if (![fm moveItemAtURL:location toURL:[NSURL fileURLWithPath:destAbs] error:nil])
                    status = 0;
            }
            dispatch_semaphore_signal(done);
        }];

        [task resume];
        while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0)
        {
            if (got)
                *got = (long long)task.countOfBytesReceived;
        }
#if !__has_feature(objc_arc)
        dispatch_release(done);
#endif
    }
    return status;
}

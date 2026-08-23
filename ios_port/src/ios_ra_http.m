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
    }

    if (body)
    {
        *out_body = body;
        if (out_len)
            *out_len = bodyLen;
    }

    return status;
}

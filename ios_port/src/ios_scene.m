/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ios_scene.m — the UIScene lifecycle, for iOS 27.
 *
 * An app linked against the iOS 27 SDK is refused at launch unless it adopts
 * the scene lifecycle: UIKit traps in
 * _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption as the first
 * scene is created, before SDL_main ever runs. SDL 2 predates scenes: its
 * UIKit backend builds the launch window and the game window with
 * -initWithFrame:, which puts them on no scene, so even with a manifest they
 * would never be shown.
 *
 * Two pieces fix that without touching SDL:
 *   - SHSceneDelegate, named by UIApplicationSceneManifest in Info.plist.in,
 *     which satisfies UIKit's check and remembers the one application scene.
 *   - A UIWindow initialiser hook: every window made with -initWithFrame: is
 *     placed on that scene, immediately when the scene already exists, or as
 *     soon as it connects for a window SDL made first (its launch window).
 * SDL keeps hearing foreground/background through the UIApplication
 * notifications, which iOS still posts under scenes.
 */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

static UIWindowScene* s_scene;
static NSPointerArray* s_waiting;   /* windows made before the scene connected */

static void Ios_PlaceWindowOnScene(UIWindow* window)
{
    if (window.windowScene != nil)
    {
        return;
    }
    if (s_scene != nil)
    {
        window.windowScene = s_scene;
        return;
    }
    if (s_waiting == nil)
    {
        s_waiting = [NSPointerArray weakObjectsPointerArray];
    }
    [s_waiting addPointer:(__bridge void*)window];
}

@interface SHSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@implementation SHSceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)options
{
    if (![scene isKindOfClass:[UIWindowScene class]])
    {
        return;
    }
    s_scene = (UIWindowScene*)scene;

    UIWindow* visible = nil;
    for (UIWindow* window in s_waiting.allObjects)
    {
        window.windowScene = s_scene;
        if (!window.hidden)
        {
            visible = window;
        }
    }
    s_waiting = nil;
    [visible makeKeyAndVisible];
}

- (void)sceneDidDisconnect:(UIScene*)scene
{
    if (scene == (UIScene*)s_scene)
    {
        s_scene = nil;
    }
}

@end

@interface UIWindow (SHScene)
@end

@implementation UIWindow (SHScene)

+ (void)load
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        SEL    original = @selector(initWithFrame:);
        SEL    hooked   = @selector(sh_initPlacedWithFrame:);
        Method hookedM  = class_getInstanceMethod(self, hooked);
        /* UIWindow may inherit -initWithFrame: from UIView. Give UIWindow its
         * own copy first so the exchange never reaches every UIView. */
        class_addMethod(self, original,
                        method_getImplementation(class_getInstanceMethod(self, original)),
                        method_getTypeEncoding(hookedM));
        method_exchangeImplementations(class_getInstanceMethod(self, original), hookedM);
    });
}

/* An init-family selector, so ARC treats it as an initialiser. After the
 * exchange above this call reaches the original -initWithFrame:. */
- (instancetype)sh_initPlacedWithFrame:(CGRect)frame
{
    UIWindow* window = [self sh_initPlacedWithFrame:frame];
    if (window != nil)
    {
        Ios_PlaceWindowOnScene(window);
    }
    return window;
}

@end

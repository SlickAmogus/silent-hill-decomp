/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ios_ra_login.m - native RetroAchievements sign-in sheet.
 *
 * On PC the launcher authenticates before the game starts and the game only
 * ever sees a token. iOS has no launcher, so the sign-in has to happen inside
 * the app, and a password field is not something the game's own 2D screens can
 * offer: they have no text input, no keyboard, and no way to mask what is
 * typed. A UIAlertController with two text fields is the platform's answer to
 * exactly this, so that is what the options menu opens.
 *
 * Fire-and-forget by design. SDL runs the game loop ON the main thread and
 * UIKit presents on the main thread, so blocking here to wait for a button
 * would stop the run loop that has to service the alert -- a deadlock with the
 * sheet frozen on screen. Instead the sheet is presented and control returns
 * immediately; the button handler starts the login, and the options screen
 * polls Pc_Ra_LoginPending()/Pc_Ra_LoginResult() to show the outcome.
 */
#import <UIKit/UIKit.h>

#include "pc_retroachievements.h"

/* The window to present from. SDL creates its own UIWindow, and on iOS 13+
 * -[UIApplication keyWindow] is deprecated and returns nil under a scene-based
 * lifecycle, so the connected-scenes list is checked first and the deprecated
 * path is only the fallback for the pre-scene case. */
static UIViewController* Ios_RaTopViewController(void)
{
    UIWindow* keyWindow = nil;

    if (@available(iOS 13.0, *))
    {
        for (UIScene* scene in [UIApplication sharedApplication].connectedScenes)
        {
            if (![scene isKindOfClass:[UIWindowScene class]])
                continue;

            for (UIWindow* w in ((UIWindowScene*)scene).windows)
            {
                if (w.isKeyWindow)
                {
                    keyWindow = w;
                    break;
                }
            }

            if (keyWindow)
                break;
        }
    }

    if (!keyWindow)
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        keyWindow = [UIApplication sharedApplication].keyWindow;
#pragma clang diagnostic pop
    }

    if (!keyWindow)
        return nil;

    /* Presenting on a controller that is itself already presenting throws, so
     * walk to the top of the chain. */
    UIViewController* vc = keyWindow.rootViewController;
    while (vc.presentedViewController)
        vc = vc.presentedViewController;

    return vc;
}

/* 1 if the sheet was put on screen. 0 means there was nowhere to present it,
 * which the caller reports rather than silently doing nothing. */
int Ios_ShowRetroAchievementsLogin(void)
{
    UIViewController* vc = Ios_RaTopViewController();

    if (!vc)
        return 0;

    UIAlertController* alert = [UIAlertController
        alertControllerWithTitle:@"RetroAchievements"
                         message:@"Sign in with your retroachievements.org account. "
                                  "Your password is exchanged for a token and is "
                                  "never stored."
                  preferredStyle:UIAlertControllerStyleAlert];

    [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
        field.placeholder             = @"Username";
        field.autocapitalizationType  = UITextAutocapitalizationTypeNone;
        field.autocorrectionType      = UITextAutocorrectionTypeNo;
        field.textContentType         = UITextContentTypeUsername;
        field.returnKeyType           = UIReturnKeyNext;
    }];

    [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
        field.placeholder            = @"Password";
        field.secureTextEntry        = YES;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.autocorrectionType     = UITextAutocorrectionTypeNo;
        field.textContentType        = UITextContentTypePassword;
        field.returnKeyType          = UIReturnKeyDone;
    }];

    /* __unsafe_unretained, not __weak: this target is built WITHOUT ARC, and a
     * zeroing weak reference needs the ARC runtime to register it.
     *
     * It is not a plain capture either. Under MRC a block retains the object
     * pointers it captures when it is copied, and the alert retains its actions
     * which retain their handlers — capturing `alert` directly would be a
     * retain cycle that leaks the sheet. This qualifier captures without
     * retaining, and the reference cannot dangle: the only thing that runs this
     * block is the alert's own action, so the alert is alive by construction. */
    __unsafe_unretained UIAlertController* weakAlert = alert;

    [alert addAction:[UIAlertAction actionWithTitle:@"Sign In"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction* action)
    {
        UIAlertController* a = weakAlert;
        if (!a)
            return;

        NSString* user = a.textFields.count > 0 ? a.textFields[0].text : @"";
        NSString* pass = a.textFields.count > 1 ? a.textFields[1].text : @"";

        Pc_Ra_BeginPasswordLogin(user.UTF8String ? user.UTF8String : "",
                                 pass.UTF8String ? pass.UTF8String : "");
    }]];

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    [vc presentViewController:alert animated:YES completion:nil];
    return 1;
}

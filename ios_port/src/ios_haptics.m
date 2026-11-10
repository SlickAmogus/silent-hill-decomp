// SPDX-License-Identifier: GPL-3.0-or-later
// The phone's Taptic Engine as the rumble motor (pc_phone_haptics.c).
//
// An impact is a single tick, so a rumble is a run of them: the caller renews
// while the game holds its motors on. iPads have no Taptic Engine and stay
// still; the generator is simply inert there.
#import <UIKit/UIKit.h>

void Ios_HapticPulse(float strength)
{
    static UIImpactFeedbackGenerator *gen;

    if (gen == nil) {
        gen = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy];
    }
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;

    if (@available(iOS 13.0, *)) {
        [gen impactOccurredWithIntensity:(CGFloat)strength];
    } else {
        [gen impactOccurred];
    }
    [gen prepare];
}

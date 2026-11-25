#import <Cocoa/Cocoa.h>

#include "macos_activate.h"

namespace launcher
{
void activate_macos_app()
{
    if (!NSApp)
        return;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}
} // namespace launcher

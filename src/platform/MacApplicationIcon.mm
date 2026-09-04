#include "ApplicationIcon.h"

#include <StudioDuoBrandData.h>

#import <AppKit/AppKit.h>

namespace studio
{
void applyPlatformApplicationIcon()
{
    @autoreleasepool
    {
        auto* data = [NSData dataWithBytes:studio_brand::studioduoicon512_png
                                   length:static_cast<NSUInteger>(
                                              studio_brand::studioduoicon512_pngSize)];
        auto* image = [[NSImage alloc] initWithData:data];
        if (image != nil)
            [NSApp setApplicationIconImage:image];
        [image release];
    }
}

void setPlatformAccessoryApplication()
{
    @autoreleasepool
    {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}
}

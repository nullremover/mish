#ifdef __APPLE__

#include "platform.h"

#import <Cocoa/Cocoa.h>


std::string openFolderDialog()
{
    NSOpenPanel* panel =
        [NSOpenPanel openPanel];


    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];


    if ([panel runModal] == NSModalResponseOK)
    {
        NSURL* url =
            [[panel URLs] firstObject];

        return std::string(
            [[url path] UTF8String]
        );
    }


    return "";
}


bool loadAppIcon(sf::Image&)
{
    return false;
}


#endif
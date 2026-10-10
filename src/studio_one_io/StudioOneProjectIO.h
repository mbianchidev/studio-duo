#pragma once

#include "project_io/ProjectImportResult.h"

#if STUDIO_DUO_TESTING
#include <functional>
#endif

namespace studio
{
class StudioOneProjectIO
{
public:
#if STUDIO_DUO_TESTING
    using MediaReadTestHook = std::function<void(const juce::File&)>;
    static void setMediaReadTestHookForTesting(MediaReadTestHook hook);
#endif

    static ProjectImportResult importSong(
        const juce::File& sourceSong,
        const juce::File& destinationPackage,
        bool allowPartialImport = false);
};
}

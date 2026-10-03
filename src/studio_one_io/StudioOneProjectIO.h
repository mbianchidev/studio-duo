#pragma once

#include "project_io/ProjectImportResult.h"

namespace studio
{
class StudioOneProjectIO
{
public:
    static ProjectImportResult importSong(
        const juce::File& sourceSong,
        const juce::File& destinationPackage,
        bool allowPartialImport = false);
};
}

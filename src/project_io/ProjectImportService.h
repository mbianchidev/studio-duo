#pragma once

#include "ProjectImportResult.h"

namespace studio
{
enum class ProjectSourceFormat
{
    studioDuo,
    studioOneSong,
    dawProject,
    unsupported
};

class ProjectImportService
{
public:
    static constexpr auto openFilePatterns = "*.studioduo;*.song;*.dawproject";
    static constexpr auto importFilePatterns = "*.song;*.dawproject";

    [[nodiscard]] static ProjectSourceFormat sourceFormat(const juce::File& source);
    [[nodiscard]] static bool supportsProjectSource(const juce::File& source);
    static ProjectImportResult importProject(
        const juce::File& source,
        const juce::File& destinationPackage,
        bool allowPartialImport = false);
};
}

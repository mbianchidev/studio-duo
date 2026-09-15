#pragma once

#include "model/ProjectModel.h"

#include <optional>

namespace studio
{
struct DawProjectExportResult
{
    juce::Result result = juce::Result::fail(
        "DAWproject export did not run.");
    CompatibilityReport report;
    juce::File archive;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return result.wasOk() && archive.existsAsFile();
    }
};

struct DawProjectImportResult
{
    juce::Result result = juce::Result::fail(
        "DAWproject import did not run.");
    CompatibilityReport report;
    std::optional<Project> project;
    juce::File package;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return result.wasOk()
            && project.has_value()
            && package.exists();
    }
};

class DawProjectIO
{
public:
    static juce::File normaliseArchivePath(
        const juce::File& requestedPath);
    static DawProjectExportResult exportProject(
        const Project& project,
        const juce::File& sourcePackage,
        const juce::File& destinationArchive);
    static DawProjectImportResult importProject(
        const juce::File& sourceArchive,
        const juce::File& destinationPackage);
    static juce::Result saveCompatibilityReport(
        const CompatibilityReport& report,
        const juce::File& destination);
};
}

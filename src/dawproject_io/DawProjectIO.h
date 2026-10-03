#pragma once

#include "model/ProjectModel.h"
#include "project_io/ProjectImportResult.h"

#if STUDIO_DUO_TESTING
#include <functional>
#endif

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

using DawProjectImportResult = ProjectImportResult;

class DawProjectIO
{
public:
#if STUDIO_DUO_TESTING
    enum class ExportTestPhase
    {
        beforePayloadSnapshot,
        afterPayloadSnapshot,
        beforeArchiveVerification
    };

    using ExportTestHook =
        std::function<void(ExportTestPhase, const juce::File&)>;

    static void setExportTestHookForTesting(ExportTestHook hook);
#endif

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

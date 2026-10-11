#pragma once

#include "model/ProjectModel.h"

#if STUDIO_DUO_TESTING
#include <functional>
#endif

namespace studio
{
struct ProjectResourceIssue
{
    juce::String objectPath;
    juce::String sourcePath;
    juce::String expectedHash;
    juce::String message;

    [[nodiscard]] juce::var toVar() const;
};

struct ProjectCollectionReport
{
    int copiedResources = 0;
    int missingResources = 0;
    int invalidResources = 0;
    int repairedResources = 0;
    std::vector<ProjectResourceIssue> issues;

    [[nodiscard]] juce::var toVar() const;
};

class ProjectCollectionService
{
public:
#if STUDIO_DUO_TESTING
    enum class ResourceTestPhase
    {
        sourceHash,
        manifestHash,
        validationHash
    };
    using ResourceTestHook =
        std::function<void(ResourceTestPhase, const juce::File&)>;
    static void setResourceTestHookForTesting(ResourceTestHook hook);
#endif

    static std::optional<ProjectCollectionReport> savePortableCopy(
        const Project& project,
        const juce::File& sourcePackage,
        const juce::File& destinationPackage,
        juce::String& error);
    static std::optional<ProjectCollectionReport> validatePortableCopy(
        const juce::File& package,
        juce::String& error);
    static ProjectCollectionReport repairMissingResources(
        Project& project,
        const std::vector<juce::File>& searchRoots,
        juce::String& error,
        const juce::File& projectPackage = {});
};
}

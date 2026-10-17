#pragma once

#include "model/ProjectModel.h"

#include <optional>
#if defined(STUDIO_DUO_TESTING)
#include <functional>
#endif

namespace studio
{
struct ProjectOpenResult
{
    Project project;
    bool recovered = false;
    juce::String warning;
};

class ProjectFile
{
public:
    static juce::File normalisePackagePath(const juce::File& requestedPath);
    static juce::Result save(const Project& project, const juce::File& packageDirectory);
    static std::optional<Project> load(const juce::File& packageDirectory, juce::String& error);
    static std::optional<ProjectOpenResult> loadForOpen(
        const juce::File& packageDirectory,
        juce::String& error);
    static juce::Result writeRecoveryPoint(const Project& project, const juce::File& packageDirectory);
    static juce::Result writeReducedIsolationMarker(
        const juce::File& packageDirectory,
        const std::vector<juce::String>& insertIds);
    static std::vector<juce::String> reducedIsolationMarker(
        const juce::File& packageDirectory);
    static juce::Result clearReducedIsolationMarker(
        const juce::File& packageDirectory);

#if defined(STUDIO_DUO_TESTING)
    enum class WriteCheckpoint { temporaryFlushed, published };
    static void setWriteCheckpointForTesting(
        std::function<void(const juce::File&, WriteCheckpoint)> callback);
#endif

private:
    static juce::Result writeJsonAtomically(const juce::File& destination, const juce::var& value);
    static juce::Result writeRecoveryPointInternal(
        const Project& project, const juce::File& packageDirectory);
};
}

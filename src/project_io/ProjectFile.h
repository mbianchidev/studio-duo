#pragma once

#include "model/ProjectModel.h"

#include <optional>

namespace studio
{
class ProjectFile
{
public:
    static juce::File normalisePackagePath(const juce::File& requestedPath);
    static juce::Result save(const Project& project, const juce::File& packageDirectory);
    static std::optional<Project> load(const juce::File& packageDirectory, juce::String& error);
    static juce::Result writeRecoveryPoint(const Project& project, const juce::File& packageDirectory);

private:
    static juce::Result writeJsonAtomically(const juce::File& destination, const juce::var& value);
};
}

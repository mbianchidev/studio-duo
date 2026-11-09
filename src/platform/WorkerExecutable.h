#pragma once

#include <juce_core/juce_core.h>

namespace studio
{
[[nodiscard]] inline bool isBackgroundWorkerExecutable(
    [[maybe_unused]] const juce::File& executable)
{
#if JUCE_MAC
    const auto macOS = executable.getParentDirectory();
    const auto contents = macOS.getParentDirectory();
    const auto bundle = contents.getParentDirectory();
    return executable.getFileName() == "Studio Duo Worker"
        && macOS.getFileName() == "MacOS"
        && contents.getFileName() == "Contents"
        && bundle.getFileName() == "Studio Duo Worker.app";
#else
    return false;
#endif
}

[[nodiscard]] inline juce::File backgroundWorkerExecutable(
    const juce::File& applicationExecutable)
{
#if JUCE_MAC
    const auto macOS = applicationExecutable.getParentDirectory();
    const auto contents = macOS.getParentDirectory();
    const auto bundle = contents.getParentDirectory();
    if (applicationExecutable.getFileName() == "Studio Duo"
        && macOS.getFileName() == "MacOS"
        && contents.getFileName() == "Contents"
        && bundle.hasFileExtension("app"))
    {
        return contents.getChildFile(
            "Helpers/Studio Duo Worker.app/Contents/MacOS/Studio Duo Worker");
    }
#endif
    return applicationExecutable;
}
}

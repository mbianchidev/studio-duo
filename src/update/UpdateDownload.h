#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace studio
{
struct UpdateDownloadResult
{
    enum class Status { complete, cancelled, failed };
    Status status = Status::failed;
    juce::String message;
};

[[nodiscard]] UpdateDownloadResult downloadUpdatePackage(
    juce::InputStream& input,
    const juce::File& destination,
    juce::int64 expectedBytes,
    const juce::String& expectedSha256,
    const std::function<bool()>& cancelled,
    const std::function<void(double)>& progress);
}

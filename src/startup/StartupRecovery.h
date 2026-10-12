#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace studio
{
struct StartupRecoveryCheck
{
    juce::Result result = juce::Result::ok();
    bool unclean = false;
    int previousSessions = 0;
};

class StartupRecovery
{
public:
    using ProcessCheck = std::function<bool(juce::int64)>;
    explicit StartupRecovery(juce::File directory, ProcessCheck processCheck = {});

    StartupRecoveryCheck inspectAndMarkActive();
    juce::Result acknowledgePreviousSessions();
    juce::Result finishCleanShutdown();
    [[nodiscard]] juce::File markerFile() const;

private:
    juce::File directory;
    juce::File activeMarker;
    ProcessCheck processIsAlive;
    std::vector<juce::File> previousMarkers;
    bool active = false;
};
}

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <mutex>
#include <queue>

namespace studio
{
inline constexpr auto pluginScanProcessId = "studioduopluginscan";

class PluginScanWorker final : private juce::ChildProcessWorker,
                               private juce::AsyncUpdater
{
public:
    PluginScanWorker();
    ~PluginScanWorker() override;

    bool initialise(const juce::String& commandLine);

private:
    void handleMessageFromCoordinator(const juce::MemoryBlock& message) override;
    void handleConnectionLost() override;
    void handleAsyncUpdate() override;
    juce::MemoryBlock scan(const juce::MemoryBlock& request);

    std::mutex queueMutex;
    std::queue<juce::MemoryBlock> requests;
    juce::AudioPluginFormatManager formatManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginScanWorker)
};
}

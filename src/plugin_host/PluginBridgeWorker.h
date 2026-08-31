#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_events/juce_events.h>

namespace studio
{
class PluginBridgeWorker final : private juce::ChildProcessWorker,
                                 private juce::Thread
{
public:
    PluginBridgeWorker();
    ~PluginBridgeWorker() override;

    bool initialise(const juce::String& commandLine);

private:
    void handleMessageFromCoordinator(const juce::MemoryBlock& message) override;
    void handleConnectionLost() override;
    void run() override;

    std::unique_ptr<juce::MemoryMappedFile> mapping;
    PluginBridgeSharedState* sharedState = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBridgeWorker)
};
}

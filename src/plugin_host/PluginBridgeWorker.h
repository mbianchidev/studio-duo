#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
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
    void startProcessing(std::unique_ptr<juce::AudioPluginInstance> newPlugin,
                         const juce::MemoryBlock& state,
                         double sampleRate,
                         int blockSize);
    void sendStatus(const juce::String& status);

    std::unique_ptr<juce::MemoryMappedFile> mapping;
    PluginBridgeSharedState* sharedState = nullptr;
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> processBuffer;
    juce::MidiBuffer midiBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBridgeWorker)
};
}

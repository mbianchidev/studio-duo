#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <mutex>
#include <vector>

namespace studio
{
class PluginEditorWindow;

class PluginBridgeWorker final : private juce::ChildProcessWorker,
                                 private juce::Thread,
                                 private juce::AudioProcessorListener,
                                 private juce::Timer
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
                         int blockSize,
                         int sidechainChannels);
    void sendStatus(const juce::String& status);
    void showEditor();
    void hideEditor();
    void focusEditor();
    void resizeEditor(int width, int height);
    void captureStateOnMessageThread();
    juce::String parameterMetadata() const;
    void audioProcessorParameterChanged(
        juce::AudioProcessor*,
        int,
        float) override;
    void audioProcessorChanged(
        juce::AudioProcessor*,
        const ChangeDetails&) override;
    void timerCallback() override;

    std::unique_ptr<juce::MemoryMappedFile> mapping;
    PluginBridgeSharedState* sharedState = nullptr;
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<PluginEditorWindow> editorWindow;
    juce::AudioBuffer<float> processBuffer;
    juce::MidiBuffer midiBuffer;
    std::vector<int> automationBoundaries;
    std::mutex pluginMutex;
    static constexpr std::uint32_t stateCaptureBit = 1u << 31;
    static constexpr std::uint32_t processingCountMask =
        ~stateCaptureBit;
    std::atomic<std::uint32_t> pluginAccessState { 0 };
    int mainInputChannels = 2;
    int sidechainInputChannels = 0;
    int mainOutputChannels = 2;
    int processingChannels = 2;
    std::atomic<bool> latencyReportPending { false };
    std::atomic<bool> metadataReportPending { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBridgeWorker)
};
}

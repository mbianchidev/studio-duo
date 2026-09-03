#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <mutex>
#include <span>

namespace studio
{
class PluginBridgeClient final : private juce::ChildProcessCoordinator
{
public:
    explicit PluginBridgeClient(
        juce::File workerExecutable = {});
    ~PluginBridgeClient() override;

    juce::Result start();
    juce::Result startPlugin(const juce::PluginDescription& description,
                             double sampleRate,
                             int blockSize,
                             const juce::MemoryBlock& state = {},
                             int sidechainChannels = 0);
    void stop();
    juce::Result requestState(juce::MemoryBlock& state,
                              std::chrono::milliseconds timeout);
    juce::Result showEditor();
    juce::Result hideEditor();
    juce::Result focusEditor();
    juce::Result resizeEditor(int width, int height);
    bool setParameter(int parameterIndex, float normalizedValue);
    void processBlock(
        juce::AudioBuffer<float>& audio,
        const juce::AudioBuffer<float>* sidechain = nullptr,
        std::span<const PluginBridgeParameterEvent> parameterEvents = {}) noexcept;

    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] std::uint64_t lateBlockCount() const noexcept;
    [[nodiscard]] int reportedLatencySamples() const noexcept;
    [[nodiscard]] double reportedTailSeconds() const noexcept;
    [[nodiscard]] std::vector<PluginParameterDescriptor>
        parameterDescriptors() const;
    [[nodiscard]] juce::String diagnosticState() const;
#if STUDIO_DUO_TESTING
    static bool recoversLateFirstOutputForTesting();
#endif

private:
    void handleMessageFromWorker(const juce::MemoryBlock& message) override;
    void handleConnectionLost() override;
    juce::Result createSharedFile();
    juce::Result startInternal(const juce::PluginDescription* description,
                               double sampleRate,
                               int blockSize,
                               const juce::MemoryBlock& state,
                               int sidechainChannels);
    void fetchWorkerOutput() noexcept;
    void publishInputBlock(int samples) noexcept;
    void prepareOutputTimeline(int blockSize);
    void queueDryInput(int samples) noexcept;
    void queueCompletedOutput() noexcept;
    void readTimelineOutput(juce::AudioBuffer<float>& audio) noexcept;
    juce::Result sendEditorCommand(const juce::String& command,
                                  int width = 0,
                                  int height = 0);
    void updateParameterMetadata(const juce::String& encoded);

    juce::File sharedFile;
    juce::File workerExecutable;
    std::unique_ptr<juce::MemoryMappedFile> mapping;
    PluginBridgeSharedState* sharedState = nullptr;
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> inputAccumulator {};
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> sidechainAccumulator {};
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> completedOutput {};
    juce::AudioBuffer<float> outputTimeline;
    int processingBlockSize = 512;
    int inputChannels = 0;
    int sidechainChannels = 0;
    std::array<PluginBridgeParameterEvent,
               PluginBridgeSharedState::maxParameterEvents> pendingParameterEvents {};
    int pendingParameterEventCount = 0;
    int completedOutputSamples = 0;
    std::int64_t nextInputSequence = 0;
    std::int64_t inFlightSequence = -1;
    std::int64_t inFlightStartSample = 0;
    std::int64_t completedSequence = -1;
    std::int64_t completedStartSample = 0;
    std::uint32_t completedOutputFlags = 0;
    std::int64_t streamSamplePosition = 0;
    std::int64_t wetReplacementBlockedUntilSample = 0;
    bool inFlightMissedDeadline = false;
    bool timelineResetPending = false;
    int bridgeQuantumSamples = 512;
    int outputTimelineCapacity = 1;
    std::uint32_t lastOutputSequence = 0;
    std::atomic<bool> ready { false };
    std::atomic<bool> connectionLost { false };
    std::atomic<std::uint64_t> lateBlocks { 0 };
    std::atomic<int> pluginLatencySamples { 0 };
    std::atomic<double> pluginTailSeconds { 0.0 };
    std::vector<PluginParameterDescriptor> parameters;
    mutable std::mutex parameterMutex;
    std::mutex commandMutex;
    std::mutex responseMutex;
    std::condition_variable responseCondition;
    juce::String responseMessage;
#if STUDIO_DUO_TESTING
    std::function<void()> beforeSecondFetchForTesting;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBridgeClient)
};
}

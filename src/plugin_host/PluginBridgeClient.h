#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <span>

namespace studio
{
class PluginBridgeClient final : private juce::ChildProcessCoordinator
{
public:
    PluginBridgeClient();
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
    bool setParameter(int parameterIndex, float normalizedValue);
    void processBlock(
        juce::AudioBuffer<float>& audio,
        const juce::AudioBuffer<float>* sidechain = nullptr,
        std::span<const PluginBridgeParameterEvent> parameterEvents = {}) noexcept;

    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] std::uint64_t lateBlockCount() const noexcept;
    [[nodiscard]] int reportedLatencySamples() const noexcept;
    [[nodiscard]] double reportedTailSeconds() const noexcept;
    [[nodiscard]] const std::vector<PluginParameterDescriptor>&
        parameterDescriptors() const noexcept;
    [[nodiscard]] juce::String diagnosticState() const;

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
    void writeOutputBlock(juce::AudioBuffer<float>& audio,
                          std::int64_t expectedSequence) noexcept;

    juce::File sharedFile;
    std::unique_ptr<juce::MemoryMappedFile> mapping;
    PluginBridgeSharedState* sharedState = nullptr;
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> lastValidOutput {};
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> inputAccumulator {};
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> sidechainAccumulator {};
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> completedOutput {};
    int processingBlockSize = 512;
    int inputChannels = 0;
    int sidechainChannels = 0;
    std::array<PluginBridgeParameterEvent,
               PluginBridgeSharedState::maxParameterEvents> pendingParameterEvents {};
    int pendingParameterEventCount = 0;
    int completedOutputSamples = 0;
    std::int64_t nextInputSequence = 0;
    std::int64_t inFlightSequence = -1;
    std::int64_t completedSequence = -1;
    bool inFlightMissedDeadline = false;
    int lastValidChannels = 0;
    int lastValidSamples = 0;
    std::uint32_t lastOutputSequence = 0;
    std::atomic<bool> ready { false };
    std::atomic<bool> connectionLost { false };
    std::atomic<std::uint64_t> lateBlocks { 0 };
    std::atomic<int> pluginLatencySamples { 0 };
    std::atomic<double> pluginTailSeconds { 0.0 };
    std::vector<PluginParameterDescriptor> parameters;
    std::mutex responseMutex;
    std::condition_variable responseCondition;
    juce::String responseMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBridgeClient)
};
}

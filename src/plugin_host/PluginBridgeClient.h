#pragma once

#include "PluginBridgeProtocol.h"

#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace studio
{
class PluginBridgeClient final : private juce::ChildProcessCoordinator
{
public:
    PluginBridgeClient();
    ~PluginBridgeClient() override;

    juce::Result start();
    void stop();
    void processBlock(juce::AudioBuffer<float>& audio) noexcept;

    [[nodiscard]] bool isReady() const noexcept;
    [[nodiscard]] std::uint64_t lateBlockCount() const noexcept;

private:
    void handleMessageFromWorker(const juce::MemoryBlock& message) override;
    void handleConnectionLost() override;
    juce::Result createSharedFile();

    juce::File sharedFile;
    std::unique_ptr<juce::MemoryMappedFile> mapping;
    PluginBridgeSharedState* sharedState = nullptr;
    std::array<std::array<float, PluginBridgeSharedState::maxBlockSize>,
               PluginBridgeSharedState::maxChannels> lastValidOutput {};
    int lastValidChannels = 0;
    int lastValidSamples = 0;
    std::uint32_t lastOutputSequence = 0;
    std::atomic<bool> ready { false };
    std::atomic<bool> connectionLost { false };
    std::atomic<std::uint64_t> lateBlocks { 0 };
    std::mutex responseMutex;
    std::condition_variable responseCondition;
    juce::String responseMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginBridgeClient)
};
}

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace studio
{
inline constexpr auto pluginBridgeProcessId = "studioduopluginbridge";

struct PluginParameterDescriptor
{
    int index = -1;
    juce::String id;
    juce::String name;
    float value = 0.0f;
    bool automatable = false;
};

struct PluginBridgeParameterEvent
{
    std::uint32_t parameterIndex = 0;
    std::uint32_t sampleOffset = 0;
    float value = 0.0f;
    std::uint32_t flags = 0;
};

struct alignas(64) PluginBridgeSharedState
{
    static constexpr std::uint32_t magicValue = 0x53444252;
    static constexpr std::uint32_t protocolVersion = 2;
    static constexpr int maxChannels = 8;
    static constexpr int maxBlockSize = 4096;
    static constexpr int maxParameterEvents = 16384;

    std::uint32_t magic = magicValue;
    std::uint32_t version = protocolVersion;
    std::atomic<std::uint32_t> hostSequence { 0 };
    std::atomic<std::uint32_t> workerSequence { 0 };
    std::atomic<std::uint32_t> shutdownRequested { 0 };
    std::atomic<std::uint32_t> numChannels { 0 };
    std::atomic<std::uint32_t> sidechainChannels { 0 };
    std::atomic<std::uint32_t> numSamples { 0 };
    std::atomic<std::uint32_t> parameterEventCount { 0 };
    std::atomic<std::uint32_t> parameterEventOverflowCount { 0 };
    std::atomic<std::uint32_t> heartbeat { 0 };
    alignas(64) std::array<std::array<float, maxBlockSize>, maxChannels> input {};
    alignas(64) std::array<std::array<float, maxBlockSize>, maxChannels> sidechain {};
    alignas(64) std::array<std::array<float, maxBlockSize>, maxChannels> output {};
    alignas(64) std::array<PluginBridgeParameterEvent, maxParameterEvents>
        parameterEvents {};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "The plugin bridge requires lock-free 32-bit shared atomics.");

class PluginBridgeProtocol
{
public:
    static bool isValid(const PluginBridgeSharedState& state) noexcept
    {
        return state.magic == PluginBridgeSharedState::magicValue
            && state.version == PluginBridgeSharedState::protocolVersion;
    }

    static bool processAvailableBlock(PluginBridgeSharedState& state) noexcept
    {
        const auto hostSequence = state.hostSequence.load(std::memory_order_acquire);
        if (hostSequence == state.workerSequence.load(std::memory_order_relaxed))
            return false;

        const auto channels = std::min(static_cast<int>(state.numChannels.load(std::memory_order_relaxed)),
                                       PluginBridgeSharedState::maxChannels);
        const auto samples = std::min(static_cast<int>(state.numSamples.load(std::memory_order_relaxed)),
                                      PluginBridgeSharedState::maxBlockSize);

        for (int channel = 0; channel < PluginBridgeSharedState::maxChannels; ++channel)
        {
            auto& destination = state.output[static_cast<std::size_t>(channel)];
            if (channel < channels)
            {
                const auto& source = state.input[static_cast<std::size_t>(channel)];
                std::copy_n(source.begin(), samples, destination.begin());
            }
            else
            {
                std::fill_n(destination.begin(), samples, 0.0f);
            }
        }

        state.workerSequence.store(hostSequence, std::memory_order_release);
        state.heartbeat.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static int parameterEventCount(
        const PluginBridgeSharedState& state) noexcept
    {
        return std::min(
            static_cast<int>(
                state.parameterEventCount.load(std::memory_order_relaxed)),
            PluginBridgeSharedState::maxParameterEvents);
    }
};
}

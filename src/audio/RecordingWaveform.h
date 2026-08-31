#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace studio
{
class RecordingWaveform
{
public:
    static constexpr int bucketSamples = 2048;
    static constexpr std::size_t capacity = 65536;

    void reset() noexcept
    {
        sequence.store(0, std::memory_order_relaxed);
        partialPeak.store(0.0f, std::memory_order_relaxed);
        partialSamples.store(0, std::memory_order_relaxed);
        peakAccumulator = 0.0f;
        samplesInBucket = 0;
        for (auto& peak : peaks)
            peak.store(0.0f, std::memory_order_relaxed);
    }

    void push(const float* const* inputs,
              int inputChannels,
              int firstInputChannel,
              int captureChannels,
              int samples) noexcept
    {
        for (int sample = 0; sample < samples; ++sample)
        {
            auto peak = 0.0f;
            for (int channel = 0; channel < captureChannels; ++channel)
            {
                const auto sourceChannel = firstInputChannel + channel;
                if (sourceChannel < inputChannels && inputs[sourceChannel] != nullptr)
                    peak = std::max(peak, std::abs(inputs[sourceChannel][sample]));
            }

            peakAccumulator = std::max(peakAccumulator, peak);
            if (++samplesInBucket == bucketSamples)
            {
                const auto current = sequence.load(std::memory_order_relaxed);
                peaks[current % capacity].store(peakAccumulator, std::memory_order_relaxed);
                sequence.store(current + 1, std::memory_order_release);
                peakAccumulator = 0.0f;
                samplesInBucket = 0;
            }
        }

        partialPeak.store(peakAccumulator, std::memory_order_relaxed);
        partialSamples.store(samplesInBucket, std::memory_order_release);
    }

    [[nodiscard]] std::vector<float> snapshot() const
    {
        const auto current = sequence.load(std::memory_order_acquire);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(current, capacity));
        const auto first = current - count;
        std::vector<float> result;
        result.reserve(count + 1);
        for (std::size_t index = 0; index < count; ++index)
            result.push_back(peaks[(first + index) % capacity].load(std::memory_order_relaxed));

        if (partialSamples.load(std::memory_order_acquire) > 0)
            result.push_back(partialPeak.load(std::memory_order_relaxed));
        return result;
    }

private:
    std::array<std::atomic<float>, capacity> peaks {};
    std::atomic<std::uint64_t> sequence { 0 };
    std::atomic<float> partialPeak { 0.0f };
    std::atomic<int> partialSamples { 0 };
    float peakAccumulator = 0.0f;
    int samplesInBucket = 0;
};
}

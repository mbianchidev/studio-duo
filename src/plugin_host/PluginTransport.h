#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace studio
{
struct PluginTransport
{
    static constexpr std::uint64_t valid = 1u;
    static constexpr std::uint64_t playing = 2u;
    static constexpr std::uint64_t recording = 4u;
    static constexpr std::uint64_t looping = 8u;

    std::int64_t samples = 0;
    double sampleRate = 48000.0;
    double seconds = 0.0;
    double bpm = 120.0;
    double ppq = 0.0;
    double barStartPpq = 0.0;
    std::int64_t barCount = 0;
    double loopStartPpq = 0.0;
    double loopEndPpq = 0.0;
    double loopStartSeconds = 0.0;
    double loopEndSeconds = 0.0;
    std::uint64_t numerator = 4;
    std::uint64_t denominator = 4;
    std::uint64_t flags = 0;
    double bpmIncrementPerSample = 0.0;

    [[nodiscard]] PluginTransport advanced(int offset) const noexcept
    {
        auto result = *this;
        if ((flags & playing) == 0)
            return result;
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !std::isfinite(bpm)
            || !std::isfinite(ppq) || !std::isfinite(bpmIncrementPerSample)
            || numerator == 0 || denominator == 0
            || (offset > 0 && samples > std::numeric_limits<std::int64_t>::max() - offset)
            || (offset < 0 && samples < std::numeric_limits<std::int64_t>::min() - offset))
        {
            result.flags = 0;
            return result;
        }
        const auto count = static_cast<double>(offset);
        const auto elapsed = count / sampleRate;
        result.samples += offset;
        result.seconds += elapsed;
        result.ppq += (bpm * elapsed + 0.5 * bpmIncrementPerSample * count * elapsed) / 60.0;
        result.bpm += bpmIncrementPerSample * count;
        const auto barLength = static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
        if (barLength > 0.0 && result.ppq >= barStartPpq + barLength)
        {
            const auto bars = static_cast<std::int64_t>(std::floor((result.ppq - barStartPpq) / barLength));
            result.barCount += bars;
            result.barStartPpq += static_cast<double>(bars) * barLength;
        }
        return result;
    }
};

static_assert(std::is_trivially_copyable_v<PluginTransport>);
static_assert(sizeof(PluginTransport) == 15 * sizeof(std::uint64_t));
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

class PluginHostPlayHead final : public juce::AudioPlayHead
{
public:
    // One processing writer; concurrent readers never receive a torn transport.
    void setTransport(const PluginTransport& value) noexcept
    {
        const auto packed = std::bit_cast<Words>(value);
        sequence.fetch_add(1, std::memory_order_seq_cst);
        for (std::size_t index = 0; index < packed.size(); ++index)
            words[index].store(packed[index], std::memory_order_seq_cst);
        sequence.fetch_add(1, std::memory_order_seq_cst);
    }

    [[nodiscard]] std::optional<PluginTransport> transport() const noexcept
    {
        const auto before = sequence.load(std::memory_order_seq_cst);
        if ((before & 1u) != 0)
            return std::nullopt;
        Words packed {};
        for (std::size_t index = 0; index < packed.size(); ++index)
            packed[index] = words[index].load(std::memory_order_seq_cst);
        if (sequence.load(std::memory_order_seq_cst) != before)
            return std::nullopt;
        return std::bit_cast<PluginTransport>(packed);
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto value = transport();
        if (!value || (value->flags & PluginTransport::valid) == 0
            || !std::isfinite(value->bpm) || value->bpm <= 0.0
            || !std::isfinite(value->seconds) || !std::isfinite(value->ppq)
            || !std::isfinite(value->sampleRate) || value->sampleRate <= 0.0
            || !std::isfinite(value->bpmIncrementPerSample)
            || !std::isfinite(value->loopStartPpq) || !std::isfinite(value->loopEndPpq)
            || !std::isfinite(value->barStartPpq) || value->numerator == 0 || value->numerator > 64
            || value->denominator == 0 || value->denominator > 64)
            return juce::nullopt;
        PositionInfo result;
        result.setTimeInSamples(value->samples);
        result.setTimeInSeconds(value->seconds);
        result.setBpm(value->bpm);
        result.setPpqPosition(value->ppq);
        result.setPpqPositionOfLastBarStart(value->barStartPpq);
        result.setBarCount(value->barCount);
        result.setTimeSignature(TimeSignature { static_cast<int>(value->numerator), static_cast<int>(value->denominator) });
        result.setLoopPoints(LoopPoints { value->loopStartPpq, value->loopEndPpq });
        result.setIsPlaying((value->flags & PluginTransport::playing) != 0);
        result.setIsRecording((value->flags & PluginTransport::recording) != 0);
        result.setIsLooping((value->flags & PluginTransport::looping) != 0);
        return result;
    }

private:
    using Words = std::array<std::uint64_t, sizeof(PluginTransport) / sizeof(std::uint64_t)>;
    std::atomic<std::uint64_t> sequence { 0 };
    std::array<std::atomic<std::uint64_t>, std::tuple_size_v<Words>> words {};
};
}

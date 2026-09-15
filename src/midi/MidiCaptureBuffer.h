#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace studio
{
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "MIDI capture requires lock-free 64-bit atomics.");
static_assert(std::atomic<std::int64_t>::is_always_lock_free,
              "MIDI capture requires lock-free signed 64-bit atomics.");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "MIDI capture requires lock-free 32-bit atomics.");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "MIDI capture requires lock-free byte atomics.");

struct CapturedMidiEvent
{
    std::uint64_t ordinal = 0;
    std::int64_t streamSample = 0;
    std::int64_t timelineSample = 0;
    std::array<std::uint8_t, 3> data {};
    std::uint8_t size = 0;
};

struct CapturedMidiWindow
{
    std::vector<CapturedMidiEvent> events;
    std::uint64_t firstOrdinal = 0;
    std::uint64_t lastOrdinal = 0;
    std::uint64_t overwrittenEvents = 0;
    std::uint64_t unsupportedEvents = 0;
};

class MidiCaptureBuffer
{
public:
    static constexpr std::size_t capacity = 65536;

    MidiCaptureBuffer();

    void push(const juce::MidiBuffer& messages,
              std::int64_t streamBlockStart,
              std::int64_t timelineBlockStart) noexcept;

    [[nodiscard]] std::uint64_t writeOrdinal() const noexcept;
    [[nodiscard]] std::uint64_t unsupportedEventCount() const noexcept;
    [[nodiscard]] CapturedMidiWindow read(
        std::uint64_t firstOrdinal,
        std::uint64_t lastOrdinal) const;
    [[nodiscard]] CapturedMidiWindow readRecent(
        std::int64_t earliestStreamSample,
        std::uint64_t lastOrdinal) const;

private:
    struct Slot
    {
        std::atomic<std::uint64_t> sequence { 0 };
        std::atomic<std::int64_t> streamSample { 0 };
        std::atomic<std::int64_t> timelineSample { 0 };
        std::atomic<std::uint32_t> packedData { 0 };
        std::atomic<std::uint8_t> size { 0 };
    };

    [[nodiscard]] bool readOne(std::uint64_t ordinal,
                               CapturedMidiEvent& event) const noexcept;

    std::unique_ptr<Slot[]> slots;
    std::atomic<std::uint64_t> nextOrdinal { 0 };
    std::atomic<std::uint64_t> unsupportedEvents { 0 };
};
}

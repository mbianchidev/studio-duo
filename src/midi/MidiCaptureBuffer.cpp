#include "MidiCaptureBuffer.h"

#include <algorithm>

namespace studio
{
MidiCaptureBuffer::MidiCaptureBuffer()
    : slots(std::make_unique<Slot[]>(capacity))
{
}

void MidiCaptureBuffer::push(const juce::MidiBuffer& messages,
                             std::int64_t streamBlockStart,
                             std::int64_t timelineBlockStart) noexcept
{
    for (const auto metadata : messages)
        pushEvent(
            metadata.getMessage(),
            streamBlockStart + metadata.samplePosition,
            timelineBlockStart + metadata.samplePosition,
            0);
}

void MidiCaptureBuffer::pushEvent(
    const juce::MidiMessage& message,
    std::int64_t streamSample,
    std::int64_t timelineSample,
    std::uint64_t targetTrackKey) noexcept
{
    const auto size = message.getRawDataSize();
    if (size < 1 || size > 3)
    {
        unsupportedEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto ordinal = nextOrdinal.fetch_add(1, std::memory_order_relaxed);
    auto& slot = slots[ordinal % capacity];
    const auto* bytes = message.getRawData();
    auto packed = std::uint32_t { 0 };
    for (int index = 0; index < size; ++index)
        packed |= static_cast<std::uint32_t>(bytes[index])
            << static_cast<unsigned int>(index * 8);

    // Mark the slot as being rewritten before any payload becomes visible.
    const auto previousSequence = slot.sequence.fetch_add(
        1, std::memory_order_acq_rel);
    slot.ordinal.store(ordinal, std::memory_order_relaxed);
    slot.streamSample.store(streamSample, std::memory_order_relaxed);
    slot.timelineSample.store(timelineSample, std::memory_order_relaxed);
    slot.targetTrackKey.store(targetTrackKey, std::memory_order_relaxed);
    slot.packedData.store(packed, std::memory_order_relaxed);
    slot.size.store(static_cast<std::uint8_t>(size), std::memory_order_relaxed);
#if defined(STUDIO_DUO_TESTING)
    if (publicationEnteredForTesting != nullptr
        && publicationReleaseForTesting != nullptr)
    {
        publicationEnteredForTesting->store(true, std::memory_order_release);
        while (!publicationReleaseForTesting->load(std::memory_order_acquire))
        {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
#endif
    slot.sequence.store(previousSequence + 2, std::memory_order_release);
    publishedOrdinal.store(ordinal + 1, std::memory_order_release);
}

std::uint64_t MidiCaptureBuffer::writeOrdinal() const noexcept
{
    return publishedOrdinal.load(std::memory_order_acquire);
}

std::uint64_t MidiCaptureBuffer::unsupportedEventCount() const noexcept
{
    return unsupportedEvents.load(std::memory_order_acquire);
}

CapturedMidiWindow MidiCaptureBuffer::read(
    std::uint64_t firstOrdinal,
    std::uint64_t lastOrdinal) const
{
    CapturedMidiWindow result;
    result.lastOrdinal = std::min(lastOrdinal, writeOrdinal());
    result.firstOrdinal = firstOrdinal;
    result.unsupportedEvents = unsupportedEventCount();
    if (result.lastOrdinal <= firstOrdinal)
        return result;

    const auto earliestAvailable =
        result.lastOrdinal > capacity
        ? result.lastOrdinal - capacity
        : std::uint64_t { 0 };
    if (result.firstOrdinal < earliestAvailable)
    {
        result.overwrittenEvents = earliestAvailable - result.firstOrdinal;
        result.firstOrdinal = earliestAvailable;
    }
    result.events.reserve(
        static_cast<std::size_t>(result.lastOrdinal - result.firstOrdinal));
    for (auto ordinal = result.firstOrdinal;
         ordinal < result.lastOrdinal;
         ++ordinal)
    {
        CapturedMidiEvent event;
        if (readOne(ordinal, event))
            result.events.push_back(event);
        else
            ++result.overwrittenEvents;
    }
    return result;
}

CapturedMidiWindow MidiCaptureBuffer::readRecent(
    std::int64_t earliestStreamSample,
    std::uint64_t lastOrdinal) const
{
    const auto end = std::min(lastOrdinal, writeOrdinal());
    const auto begin = end > capacity
        ? end - capacity
        : std::uint64_t { 0 };
    auto result = read(begin, end);
    result.events.erase(
        std::remove_if(
            result.events.begin(),
            result.events.end(),
            [earliestStreamSample](const auto& event)
            {
                return event.streamSample < earliestStreamSample;
            }),
        result.events.end());
    return result;
}

bool MidiCaptureBuffer::readOne(std::uint64_t ordinal,
                                CapturedMidiEvent& event) const noexcept
{
    const auto& slot = slots[ordinal % capacity];
    const auto sequenceBefore = slot.sequence.load(
        std::memory_order_acquire);
    if ((sequenceBefore & 1u) != 0u)
        return false;

    const auto storedOrdinal = slot.ordinal.load(
        std::memory_order_relaxed);
    event.streamSample = slot.streamSample.load(std::memory_order_relaxed);
    event.timelineSample = slot.timelineSample.load(std::memory_order_relaxed);
    event.targetTrackKey = slot.targetTrackKey.load(std::memory_order_relaxed);
    const auto packed = slot.packedData.load(std::memory_order_relaxed);
    event.size = slot.size.load(std::memory_order_relaxed);
    const auto sequenceAfter = slot.sequence.load(
        std::memory_order_acquire);
    if (sequenceBefore != sequenceAfter
        || (sequenceAfter & 1u) != 0u
        || storedOrdinal != ordinal)
        return false;

    event.ordinal = ordinal;
    for (std::size_t index = 0; index < event.data.size(); ++index)
        event.data[index] = static_cast<std::uint8_t>(
            (packed >> static_cast<unsigned int>(index * 8)) & 0xffu);
    return event.size >= 1 && event.size <= event.data.size();
}

#if defined(STUDIO_DUO_TESTING)
void MidiCaptureBuffer::setPublicationPauseForTesting(
    std::atomic<bool>* entered,
    const std::atomic<bool>* release) noexcept
{
    publicationEnteredForTesting = entered;
    publicationReleaseForTesting = release;
}
#endif
}

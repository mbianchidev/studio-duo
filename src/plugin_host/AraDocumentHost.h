#pragma once

#include "model/ProjectModel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace studio
{
struct AraDocumentDescriptor
{
    struct AudioRegion
    {
        juce::String sourceId;
        juce::String modificationId;
        juce::String regionId;
        juce::String trackId;
        juce::String trackName;
        juce::String clipName;
        juce::File sourceFile;
        double startInPlaybackSeconds = 0.0;
        double startInModificationSeconds = 0.0;
        double durationInPlaybackSeconds = 0.0;
        double durationInModificationSeconds = 0.0;
    };

    struct TempoEntry
    {
        double timeSeconds = 0.0;
        double quarterPosition = 0.0;
    };

    struct MeterEntry
    {
        double quarterPosition = 0.0;
        int numerator = 4;
        int denominator = 4;
    };

    juce::String name;
    juce::String revision;
    std::vector<AudioRegion> audioRegions;
    std::vector<TempoEntry> tempoEntries;
    std::vector<MeterEntry> meterEntries;
};

class AraDocumentHost
{
public:
    AraDocumentHost();
    ~AraDocumentHost();

    juce::Result bind(juce::AudioPluginInstance& instance);
    juce::Result bind(
        juce::AudioPluginInstance& instance,
        std::shared_ptr<const AraDocumentDescriptor> descriptor,
        const juce::MemoryBlock& archivedState = {});
    juce::Result archive(juce::MemoryBlock& state) const;
    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] std::size_t audioSourceCount() const noexcept;
    [[nodiscard]] std::size_t playbackRegionCount() const noexcept;
    static std::shared_ptr<const AraDocumentDescriptor> describeProject(
        const Project& project,
        const juce::String& trackId);
    static juce::MemoryBlock packState(
        const juce::MemoryBlock& processorState,
        const juce::MemoryBlock& araState);
    static bool unpackState(const juce::MemoryBlock& storedState,
                            juce::MemoryBlock& processorState,
                            juce::MemoryBlock& araState);
    static juce::String reducedIsolationWarning();

private:
#if JUCE_PLUGINHOST_ARA
    struct Impl;
    std::unique_ptr<Impl> impl;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AraDocumentHost)
};
}

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <vector>

namespace studio
{
struct PluginCatalogEntry
{
    juce::String name;
    juce::String manufacturer;
    juce::String category;
    juce::String format;
    juce::String version;
    juce::String fileOrIdentifier;
    juce::String identifier;
    int inputChannels = 0;
    int outputChannels = 0;
    bool instrument = false;
};

class PluginCatalog final : private juce::Thread
{
public:
    PluginCatalog();
    ~PluginCatalog() override;

    void startScan(bool forceRescan);
    void cancelScan();

    [[nodiscard]] bool isScanning() const noexcept;
    [[nodiscard]] float progress() const noexcept;
    [[nodiscard]] juce::String status() const;
    [[nodiscard]] std::vector<PluginCatalogEntry> entries() const;
    [[nodiscard]] juce::StringArray blacklistedFiles() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] juce::StringArray availableFormats() const;
    [[nodiscard]] juce::File dataDirectory() const;
    [[nodiscard]] std::optional<juce::PluginDescription> descriptionForIdentifier(
        const juce::String& identifier) const;

    static bool matchesQuery(const PluginCatalogEntry& entry, const juce::String& query)
    {
        const auto terms = juce::StringArray::fromTokens(query.toLowerCase(), " ", "");
        const auto searchable = (entry.name + " "
                                 + entry.manufacturer + " "
                                 + entry.category + " "
                                 + entry.format)
                                    .toLowerCase();

        for (const auto& term : terms)
            if (term.isNotEmpty() && !searchable.contains(term))
                return false;

        return true;
    }

private:
    void run() override;
    void load();
    juce::Result save();
    void updateState(juce::String message, float newProgress);

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::File catalogDirectory;
    juce::File catalogFile;
    juce::File deadMansPedalFile;
    std::shared_ptr<std::atomic<bool>> cancelRequested;
    std::atomic<bool> scanning { false };
    std::atomic<bool> forceNextScan { false };
    std::atomic<float> scanProgress { 0.0f };
    std::atomic<std::uint64_t> catalogRevision { 0 };
    mutable juce::CriticalSection stateLock;
    juce::String statusMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginCatalog)
};
}

#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace studio
{
class ProjectArchiveReader
{
public:
    static constexpr juce::int64 maximumArchiveBytes = 0x7fffffff;
    static constexpr juce::int64 maximumXmlBytes = 64 * 1024 * 1024;

    explicit ProjectArchiveReader(
        const juce::File& archive,
        juce::String formatLabel);

    juce::Result validate(const juce::StringArray& requiredEntries,
                          bool allowDirectoryEntries = false);
    [[nodiscard]] bool has(const juce::String& path) const;
    std::optional<juce::String> readText(
        const juce::String& path,
        juce::String& error,
        juce::int64 maximumBytes = maximumXmlBytes);
    bool readData(const juce::String& path,
                  juce::MemoryBlock& data,
                  juce::String& error,
                  juce::int64 maximumBytes = 512 * 1024 * 1024);
    bool copyTo(const juce::String& path,
                const juce::File& destination,
                juce::String& error);

private:
    struct EntryMetadata
    {
        int index = -1;
        juce::int64 uncompressedSize = 0;
        std::uint32_t crc = 0;
    };

    juce::File source;
    juce::String label;
    juce::ZipFile zip;
    std::map<juce::String, EntryMetadata> paths;

    juce::Result parseCentralDirectory(bool allowDirectoryEntries);
    bool readEntry(
        const juce::String& path,
        const EntryMetadata& metadata,
        juce::String& error,
        const std::function<bool(const void*, std::size_t)>& consumer);
};
}

#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace studio
{
enum class PluginSearchPathStyle
{
    posix,
    windows
};

struct PluginSearchPlan
{
    juce::FileSearchPath folders;
    juce::StringArray warnings;
};

class PluginSearchPaths final
{
public:
    explicit PluginSearchPaths(juce::File settingsFile);

    bool load(juce::String& error);
    juce::Result addCustomFolder(const juce::File& folder);
    juce::Result removeCustomFolder(const juce::File& folder);

    [[nodiscard]] juce::StringArray customFolders() const;
    [[nodiscard]] PluginSearchPlan createScanPlan(
        const juce::FileSearchPath& defaultFolders) const;

    [[nodiscard]] static juce::StringArray normalizeAndDeduplicate(
        const juce::StringArray& paths,
        PluginSearchPathStyle style);
    [[nodiscard]] static PluginSearchPathStyle nativePathStyle() noexcept;

private:
    [[nodiscard]] juce::Result save() const;

    juce::File file;
    juce::StringArray folders;
    bool writable = true;
};
}

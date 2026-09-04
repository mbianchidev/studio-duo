#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace studio
{
class ClapPluginFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override;
    void findAllTypesForFile(
        juce::OwnedArray<juce::PluginDescription>& results,
        const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType(
        const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier(
        const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning(
        const juce::PluginDescription& description) override;
    bool doesPluginStillExist(
        const juce::PluginDescription& description) override;
    bool canScanForPlugins() const override;
    bool isTrivialToScan() const override;
    juce::StringArray searchPathsForPlugins(
        const juce::FileSearchPath& directoriesToSearch,
        bool recursive,
        bool allowPluginsWhichRequireAsynchronousInstantiation) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation(
        const juce::PluginDescription&) const override;

private:
    void createPluginInstance(
        const juce::PluginDescription& description,
        double initialSampleRate,
        int initialBufferSize,
        PluginCreationCallback callback) override;
};
}

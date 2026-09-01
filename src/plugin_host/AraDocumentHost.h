#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace studio
{
class AraDocumentHost
{
public:
    juce::Result bind(juce::AudioPluginInstance& instance);
    [[nodiscard]] bool isBound() const noexcept;
    static juce::String reducedIsolationWarning();

private:
#if JUCE_PLUGINHOST_ARA
    std::unique_ptr<juce::ARAHostDocumentController> documentController;
    juce::ARAHostModel::PlugInExtensionInstance binding;
#endif
};
}

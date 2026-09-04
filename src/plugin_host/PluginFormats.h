#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace studio
{
class PluginFormats
{
public:
    static void addSupportedFormats(
        juce::AudioPluginFormatManager& manager);
};
}

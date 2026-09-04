#include "PluginFormats.h"

#include "ClapPluginFormat.h"

#include <algorithm>

namespace studio
{
void PluginFormats::addSupportedFormats(
    juce::AudioPluginFormatManager& manager)
{
    juce::addDefaultFormatsToManager(manager);
    const auto formats = manager.getFormats();
    const auto hasClap = std::any_of(
        formats.begin(),
        formats.end(),
        [](const auto* format)
        {
            return format != nullptr && format->getName() == "CLAP";
        });
    if (!hasClap)
        manager.addFormat(std::make_unique<ClapPluginFormat>());
}
}

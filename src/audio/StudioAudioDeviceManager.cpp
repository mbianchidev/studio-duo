#include "StudioAudioDeviceManager.h"

namespace studio
{
namespace
{
bool isGenericAsioWrapper(const juce::String& name)
{
    return name.containsIgnoreCase("asio4all")
        || name.containsIgnoreCase("generic low latency asio")
        || name.containsIgnoreCase("steinberg built-in asio")
        || name.containsIgnoreCase("fl studio asio")
        || name.containsIgnoreCase("flexasio");
}
}

juce::String preferredAsioDeviceName(
    const juce::StringArray& deviceNames)
{
    for (const auto& name : deviceNames)
    {
        if (name.isNotEmpty() && !isGenericAsioWrapper(name))
            return name;
    }

    return deviceNames.isEmpty() ? juce::String() : deviceNames[0];
}

void StudioAudioDeviceManager::createAudioDeviceTypes(
    juce::OwnedArray<juce::AudioIODeviceType>& types)
{
    juce::AudioDeviceManager::createAudioDeviceTypes(types);

#if JUCE_WINDOWS
    for (int index = 0; index < types.size(); ++index)
    {
        if (types[index]->getTypeName().equalsIgnoreCase("ASIO"))
        {
            types.move(index, 0);
            break;
        }
    }
#endif
}
}

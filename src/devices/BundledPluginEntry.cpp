#include "AmpDeviceProcessor.h"
#include "DrumDeviceProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
#if defined(STUDIO_DUO_PLUGIN_DRUM)
    return new studio::DrumDeviceProcessor();
#elif defined(STUDIO_DUO_PLUGIN_GUITAR_AMP)
    return new studio::AmpDeviceProcessor(
        studio::AmpDeviceType::guitar);
#elif defined(STUDIO_DUO_PLUGIN_BASS_AMP)
    return new studio::AmpDeviceProcessor(
        studio::AmpDeviceType::bass);
#else
    return nullptr;
#endif
}

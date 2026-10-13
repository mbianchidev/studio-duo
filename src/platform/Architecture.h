#pragma once

#include <juce_core/juce_core.h>

namespace studio
{
inline juce::String currentArchitecture()
{
#if defined(JUCE_ARM) && JUCE_ARM
 #if defined(JUCE_64BIT) && JUCE_64BIT
    return "arm64";
 #else
    return "arm";
 #endif
#elif defined(JUCE_64BIT) && JUCE_64BIT
    return "x86_64";
#else
    return "x86";
#endif
}
}

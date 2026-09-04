#pragma once

#include <juce_core/juce_core.h>

namespace studio
{
class ValidatedPluginStateTarget
{
public:
    virtual ~ValidatedPluginStateTarget() = default;

    virtual juce::Result saveValidatedState(
        juce::MemoryBlock& destination) = 0;
    virtual juce::Result restoreValidatedState(
        const void* data,
        int size) = 0;
};
}

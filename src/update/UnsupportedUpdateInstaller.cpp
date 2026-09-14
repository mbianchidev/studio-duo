#include "UpdateInstaller.h"

namespace studio
{
juce::Result launchUpdateInstaller(
    const juce::File&,
    const juce::String&)
{
    return juce::Result::fail(
        "Automatic updates are not available on this platform.");
}
}

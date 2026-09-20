#pragma once

#include <juce_core/juce_core.h>

namespace studio
{
class StudioPreferences
{
public:
    explicit StudioPreferences(
        juce::File settingsFile = {});

    [[nodiscard]] bool autosaveEnabled() const noexcept;
    [[nodiscard]] const juce::String& status() const noexcept;
    juce::Result setAutosaveEnabled(bool enabled);

private:
    static juce::File defaultSettingsFile();
    void load();
    juce::Result save();

    juce::File settingsFile;
    bool autosave = true;
    juce::String statusMessage;
};
}

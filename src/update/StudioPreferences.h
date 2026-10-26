#pragma once

#include "ui/StudioTheme.h"
#include "midi/ControlSurface.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace studio
{
struct RecentProject
{
    juce::File file;
    juce::String name;
    juce::Time lastEdited;
};

class StudioPreferences
{
public:
    explicit StudioPreferences(
        juce::File settingsFile = {},
        bool loadFromDisk = true);

    [[nodiscard]] bool autosaveEnabled() const noexcept;
    [[nodiscard]] bool scanPluginsAtStartup() const noexcept;
    [[nodiscard]] const juce::String& themePresetId() const noexcept;
    [[nodiscard]] StudioThemePalette themePalette() const;
    [[nodiscard]] const std::vector<RecentProject>&
        recentProjects() const noexcept;
    [[nodiscard]] juce::String suggestProjectName(
        const juce::String& preferredName,
        const juce::File& directory,
        const juce::String& activeProjectName = {}) const;
    [[nodiscard]] const juce::String& status() const noexcept;
    [[nodiscard]] const ControlSurfaceSettings& controlSurfaceSettings() const noexcept;

    juce::Result setAutosaveEnabled(bool enabled);
    juce::Result setScanPluginsAtStartup(bool enabled);
    juce::Result setControlSurfaceSettings(const ControlSurfaceSettings& settings);
    juce::Result setThemePreset(const juce::String& presetId);
    juce::Result setCustomThemePalette(
        const StudioThemePalette& palette);
    juce::Result resetTheme();
    juce::Result recordRecentProject(
        const juce::File& package,
        const juce::String& projectName,
        juce::Time editedAt = juce::Time::getCurrentTime());
    juce::Result removeRecentProject(
        const juce::File& package);

private:
    static juce::File defaultSettingsFile();
    void load();
    juce::Result save();

    juce::File settingsFile;
    bool autosave = true;
    bool scanAtStartup = true;
    ControlSurfaceSettings controlSurface;
    juce::String selectedThemePreset { "studio-gray" };
    StudioThemePalette customTheme;
    std::vector<RecentProject> recent;
    juce::String statusMessage;
};
}

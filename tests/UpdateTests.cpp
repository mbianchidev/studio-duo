#include "update/UpdateManifest.h"
#include "update/StudioPreferences.h"
#include "TestHarness.h"
#include "TestSuites.h"

namespace
{
juce::String validManifest()
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schemaVersion", 1);
    root->setProperty("version", "2.4.1");
    root->setProperty(
        "publishedAt",
        "2026-09-13T20:00:00Z");
    root->setProperty(
        "releaseNotesUrl",
        "https://github.com/mbianchidev/studio-duo/releases/tag/v2.4.1");

    auto platforms = std::make_unique<juce::DynamicObject>();
    auto mac = std::make_unique<juce::DynamicObject>();
    mac->setProperty(
        "url",
        "https://github.com/mbianchidev/studio-duo/releases/download/v2.4.1/Studio-Duo-2.4.1-macOS-universal.zip");
    mac->setProperty(
        "fileName",
        "Studio-Duo-2.4.1-macOS-universal.zip");
    mac->setProperty(
        "sha256",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    mac->setProperty("sizeBytes", static_cast<juce::int64>(123456));
    platforms->setProperty("macos", juce::var(mac.release()));

    auto windows = std::make_unique<juce::DynamicObject>();
    windows->setProperty(
        "url",
        "https://github.com/mbianchidev/studio-duo/releases/download/v2.4.1/Studio-Duo-2.4.1-Windows-x64-Setup.exe");
    windows->setProperty(
        "fileName",
        "Studio-Duo-2.4.1-Windows-x64-Setup.exe");
    windows->setProperty(
        "sha256",
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    windows->setProperty("sizeBytes", static_cast<juce::int64>(654321));
    platforms->setProperty(
        "windows",
        juce::var(windows.release()));

    root->setProperty(
        "platforms",
        juce::var(platforms.release()));
    return juce::JSON::toString(
        juce::var(root.release()),
        true);
}

void semanticVersionComparison()
{
    expect(
        studio::parseSemanticVersion("1.2.3").has_value(),
        "Stable semantic versions are accepted.");
    expect(
        !studio::parseSemanticVersion("1.2").has_value(),
        "Incomplete semantic versions are rejected.");
    expect(
        !studio::parseSemanticVersion("01.2.3").has_value(),
        "Semantic versions with leading zeroes are rejected.");
    expect(
        studio::isNewerSemanticVersion("1.3.0", "1.2.9"),
        "A newer minor version is detected.");
    expect(
        studio::isNewerSemanticVersion("2.0.0", "1.99.99"),
        "A newer major version is detected.");
    expect(
        !studio::isNewerSemanticVersion("1.2.3", "1.2.3"),
        "The current version is not treated as an update.");
    expect(
        !studio::isNewerSemanticVersion("1.2.2", "1.2.3"),
        "An older version is not treated as an update.");
}

void manifestParsing()
{
    juce::String error;
    const auto mac = studio::parseUpdateManifest(
        validManifest(),
        studio::UpdatePlatform::macOS,
        error);
    expect(mac.has_value(), error.toRawUTF8());
    expect(
        mac.has_value()
            && mac->asset.fileName
                == "Studio-Duo-2.4.1-macOS-universal.zip"
            && mac->asset.sizeBytes == 123456,
        "The macOS update package is parsed.");

    const auto windows = studio::parseUpdateManifest(
        validManifest(),
        studio::UpdatePlatform::windowsInstaller,
        error);
    expect(windows.has_value(), error.toRawUTF8());
    expect(
        windows.has_value()
            && windows->asset.fileName
                == "Studio-Duo-2.4.1-Windows-x64-Setup.exe"
            && windows->asset.sizeBytes == 654321,
        "The Windows update package is parsed.");

    const auto untrusted = validManifest().replace(
        "https://github.com/mbianchidev/studio-duo/releases/download/",
        "https://example.com/download/");
    expect(
        !studio::parseUpdateManifest(
             untrusted,
             studio::UpdatePlatform::macOS,
             error)
             .has_value(),
        "Update packages outside the official release repository are rejected.");

    const auto badHash = validManifest().replace(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "not-a-checksum");
    expect(
        !studio::parseUpdateManifest(
             badHash,
             studio::UpdatePlatform::macOS,
             error)
             .has_value(),
        "Update packages without a SHA-256 checksum are rejected.");

    expect(
        !studio::parseUpdateManifest(
             validManifest(),
             studio::UpdatePlatform::windowsPortable,
             error)
             .has_value(),
        "Portable Windows builds do not advertise an in-place update.");
}

void preferencePersistence()
{
    const auto directory =
        juce::File::getSpecialLocation(
            juce::File::tempDirectory)
            .getNonexistentChildFile(
                "StudioDuoPreferences",
                {},
                false);
    const auto file =
        directory.getChildFile("preferences.json");
    {
        studio::StudioPreferences preferences(file);
        expect(preferences.autosaveEnabled(),
               "Autosave recovery defaults to enabled.");
        expect(preferences.scanPluginsAtStartup(),
               "Startup plug-in scanning defaults to enabled.");
        expect(preferences.setAutosaveEnabled(false).wasOk()
                   && !preferences.autosaveEnabled(),
               "Autosave recovery can be disabled and persisted.");
        expect(preferences.setScanPluginsAtStartup(false).wasOk()
                   && !preferences.scanPluginsAtStartup(),
               "Startup plug-in scanning can be disabled and persisted.");
        expect(preferences.setThemePreset("slate-blue").wasOk()
                   && preferences.themePresetId() == "slate-blue",
               "A bundled theme can be selected.");
        expect(preferences.recordRecentProject(
                   directory.getChildFile("older.studioduo"),
                   "Older",
                   juce::Time(1000)).wasOk()
                   && preferences.recordRecentProject(
                       directory.getChildFile("newer.studioduo"),
                       "Newer",
                       juce::Time(2000)).wasOk(),
               "Recent projects can be recorded.");
    }
    {
        studio::StudioPreferences preferences(file);
        expect(!preferences.autosaveEnabled(),
               "Autosave recovery preference survives reload.");
        expect(!preferences.scanPluginsAtStartup(),
               "Startup plug-in scan preference survives reload.");
        expect(preferences.themePresetId() == "slate-blue"
                   && preferences.themePalette()
                       == *studio::studioThemePaletteForPreset(
                           "slate-blue"),
               "Theme selection survives reload.");
        expect(preferences.recentProjects().size() == 2
                   && preferences.recentProjects().front().name
                       == "Newer",
               "Recent projects reload in descending edit order.");
        expect(preferences.removeRecentProject(
                   directory.getChildFile("older.studioduo")).wasOk()
                   && preferences.recentProjects().size() == 1,
               "Recent projects can be removed.");
        auto inaccessible = preferences.themePalette();
        inaccessible.text = inaccessible.panel;
        expect(preferences.setCustomThemePalette(inaccessible).failed()
                   && preferences.themePresetId() == "slate-blue",
               "Custom themes that fail WCAG text contrast are rejected.");
        auto custom = preferences.themePalette();
        custom.orange = 0xff87b7e3;
        expect(preferences.setCustomThemePalette(custom).wasOk()
                   && preferences.themePresetId() == "custom",
               "Accessible custom themes are persisted.");
    }
    {
        studio::StudioPreferences preferences(file);
        expect(preferences.themePresetId() == "custom"
                   && preferences.themePalette().orange
                       == 0xff87b7e3,
               "Custom theme colors survive reload.");
    }
    expect(studio::studioThemePresets()
                   .front()
                   .palette.window
               != 0xff101214,
           "The default theme uses a lighter gray base than the previous near-black surface.");
    for (const auto& preset : studio::studioThemePresets())
    {
        juce::String error;
        expect(preset.palette.isAccessible(error),
               ("Bundled theme is accessible: "
                + juce::String(preset.name)
                + (error.isNotEmpty()
                       ? " (" + error + ")"
                       : juce::String()))
                   .toRawUTF8());
    }
    file.replaceWithText("{\"autosaveEnabled\":\"invalid\"}");
    {
        studio::StudioPreferences preferences(file);
        expect(preferences.autosaveEnabled()
                   && preferences.status().isNotEmpty(),
               "Invalid preference data reports the problem and restores defaults.");
    }
    directory.deleteRecursively();
}
}

void updateTests()
{
    semanticVersionComparison();
    manifestParsing();
    preferencePersistence();
}

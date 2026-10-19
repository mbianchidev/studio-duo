#include "update/UpdateManifest.h"
#include "update/UpdateDownload.h"
#include "update/StudioPreferences.h"
#include "TestHarness.h"
#include "TestSuites.h"

#include <juce_cryptography/juce_cryptography.h>

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
        expect(!preferences.controlSurfaceSettings().enabled(),
               "Legacy/default preferences never activate control-surface MIDI ports.");
        expect(preferences.setControlSurfaceSettings({
                   studio::ControlSurfaceProtocol::mackieControl, "mock-control-in", "mock-control-out"
               }).wasOk(), "Explicit synthetic control-surface settings can be saved without opening MIDI.");
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
        expect(preferences.controlSurfaceSettings().enabled()
                   && preferences.controlSurfaceSettings().inputDeviceId == "mock-control-in"
                   && preferences.controlSurfaceSettings().outputDeviceId == "mock-control-out",
               "A dedicated surface protocol and exact port identities survive reload.");
        auto incomplete = preferences.controlSurfaceSettings();
        incomplete.outputDeviceId.clear();
        expect(preferences.setControlSurfaceSettings(incomplete).failed()
                   && preferences.controlSurfaceSettings().outputDeviceId == "mock-control-out",
               "An invalid surface configuration cannot replace saved working settings.");
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
    file.replaceWithText("{\"autosaveEnabled\":true,\"controlSurface\":{\"protocol\":\"unsupported\","
                         "\"inputDeviceId\":\"mock-in\",\"outputDeviceId\":\"mock-out\"}}");
    {
        studio::StudioPreferences preferences(file);
        expect(!preferences.controlSurfaceSettings().enabled() && preferences.status().isNotEmpty(),
               "Unsupported stored control protocols are visibly rejected and cannot open a driver.");
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

void packageDownloadPreservation()
{
    juce::TemporaryFile fixture(".studio-duo-download-test");
    const auto root = fixture.getFile();
    expect(root.createDirectory().wasOk(), "The synthetic download directory can be created.");
    const auto target = root.getChildFile("update.zip");
    juce::MemoryBlock payload(192 * 1024, true);
    const auto hash = juce::SHA256(payload).toHexString();
    using Status = studio::UpdateDownloadResult::Status;
    const auto run = [&](juce::int64 inputBytes, juce::int64 expectedBytes,
                         const juce::String& expectedHash, bool cancelAfterProgress)
    {
        expect(target.replaceWithText("previous verified package"),
               "The previous synthetic package can be retained.");
        juce::MemoryInputStream input(payload.getData(), static_cast<std::size_t>(inputBytes), false);
        bool cancelled = false;
        const auto result = studio::downloadUpdatePackage(
            input, target, expectedBytes, expectedHash, [&] { return cancelled; },
            [&](double) { cancelled = cancelAfterProgress; });
        expect(root.findChildFiles(juce::File::findFiles, false, "*.part-*").isEmpty(),
               "Finished, cancelled and failed downloads clean their owned partial files.");
        return result;
    };
    const auto bytes = static_cast<juce::int64>(payload.getSize());
    juce::MemoryInputStream freshInput(payload, false);
    auto result = studio::downloadUpdatePackage(
        freshInput, target, bytes, hash, [] { return true; }, [](double) {});
    expect(result.status == Status::cancelled && !target.exists() && freshInput.getPosition() == 0,
           "Cancellation before transfer reads no bytes and publishes no package.");
    result = studio::downloadUpdatePackage(
        freshInput, target, bytes, hash, [] { return false; }, [](double) {});
    expect(result.status == Status::complete && juce::SHA256(target).toHexString() == hash,
           "A first download publishes a verified package when no previous destination exists.");
    result = run(bytes, bytes, hash, false);
    expect(result.status == Status::complete && juce::SHA256(target).toHexString() == hash,
           "Only a complete size/hash-verified package replaces the previous package.");
    const auto expectPreserved = [&](juce::int64 inputBytes, juce::int64 expectedBytes,
                                     const juce::String& expectedHash, bool cancel)
    {
        const auto failure = run(inputBytes, expectedBytes, expectedHash, cancel);
        expect(failure.status != Status::complete && failure.message.isNotEmpty()
                   && target.loadFileAsString() == "previous verified package",
               "Truncation, excess bytes, integrity failure and cancellation preserve the old package.");
    };
    expectPreserved(bytes - 1, bytes, hash, false);
    expectPreserved(bytes, bytes - 1, hash, false);
    expectPreserved(bytes, bytes, juce::String::repeatedString("0", 64), false);
    expectPreserved(bytes, bytes, hash, true);
    result = run(bytes, bytes, hash, true);
    expect(result.status == Status::cancelled, "User cancellation is distinct from an update failure.");
    juce::MemoryInputStream input(payload, false);
    const auto blocker = root.getChildFile("not-a-directory");
    expect(blocker.replaceWithText("preserve me"), "A synthetic unwritable-parent condition can be created.");
    result = studio::downloadUpdatePackage(
        input, blocker.getChildFile("update.zip"), bytes, hash, [] { return false; }, [](double) {});
    expect(result.status == Status::failed && result.message.isNotEmpty()
               && blocker.loadFileAsString() == "preserve me",
           "A non-directory destination parent fails visibly without modifying existing data.");
    const auto collision = root.getChildFile("publication-collision.zip");
    juce::MemoryInputStream collisionInput(payload, false);
    result = studio::downloadUpdatePackage(
        collisionInput, collision, bytes, hash, [] { return false; }, [&](double)
        {
            expect(collision.createDirectory().wasOk()
                       && collision.getChildFile("sentinel").replaceWithText("preserved"),
                   "A synthetic destination collision can be introduced before publication.");
        });
    expect(result.status == Status::failed && result.message.isNotEmpty()
               && collision.getChildFile("sentinel").loadFileAsString() == "preserved"
               && root.findChildFiles(juce::File::findFiles, false, "*.part-*").isEmpty(),
           "Failed final publication preserves the conflicting destination and removes its partial file.");
}
}

void updateTests()
{
    semanticVersionComparison();
    manifestParsing();
    preferencePersistence();
    packageDownloadPreservation();
}

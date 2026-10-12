#include "TestHarness.h"
#include "TestSuites.h"

#include "startup/StartupCoordinator.h"
#include "startup/StartupDiagnostics.h"
#include "startup/StartupRecovery.h"
#include "plugin_host/PluginCatalog.h"
#include "ui/StartupProgressComponent.h"
#include "update/StudioPreferences.h"

#include <cmath>
#include <algorithm>
#include <limits>

void startupTests()
{
    using namespace studio;
    StartupCoordinator startup({
        { StartupTaskId::preferences, "Preferences", {}, true },
        { StartupTaskId::pluginCatalog, "Plug-in catalog", {}, true },
        { StartupTaskId::workspace, "Workspace",
          { StartupTaskId::preferences, StartupTaskId::pluginCatalog }, true }
    });
    expect(startup.begin(100.0).wasOk(), "A valid startup graph can begin.");
    const auto independent = startup.takeRunnableTasks(100.0);
    expect(independent.size() == 2
               && startup.readiness() == StartupReadiness::loading,
           "Independent tasks start together without declaring the workspace ready.");
    expect(startup.complete(independent[0], {}, 150.0)
               && startup.takeRunnableTasks(150.0).empty(),
           "A dependent task waits for every prerequisite.");
    expect(startup.complete(independent[1], {}, 175.0),
           "The second prerequisite can complete independently.");
    const auto dependent = startup.takeRunnableTasks(175.0);
    expect(dependent.size() == 1
               && dependent.front().id == StartupTaskId::workspace,
           "The workspace starts only after both prerequisites complete.");
    expect(startup.complete(dependent.front(), {}, 190.0)
               && startup.readiness() == StartupReadiness::ready
               && startup.progress() == 1.0,
           "Only the completed required graph satisfies readiness.");

    {
        StartupCoordinator weighted({
            { StartupTaskId::audio, "Audio", {}, true, true, 3.0 },
            { StartupTaskId::pluginCatalog, "Catalog", {}, true, true, 1.0 }
        });
        expect(weighted.begin(0.0, juce::Time(1000000)).wasOk(), "Weighted startup can begin.");
        const auto tokens = weighted.takeRunnableTasks(0.0);
        expect(weighted.updateProgress(tokens[0], 0.5) && weighted.updateProgress(tokens[1], 0.25)
                   && std::abs(weighted.progress() - 0.4375) < 1.0e-9,
               "Required progress uses task weights instead of counting status strings.");
        expect(weighted.updateProgress(tokens[0], 0.1)
                   && std::abs(weighted.progress() - 0.4375) < 1.0e-9,
               "Progress cannot move backwards within an attempt.");
        expect(!weighted.updateProgress(tokens[0], std::numeric_limits<double>::quiet_NaN())
                   && !weighted.updateProgress(tokens[0], 1.1),
               "Invalid progress values cannot corrupt the readiness model.");
        expect(weighted.complete(tokens[1], { StartupState::warning, StartupIssue::catalogInvalid,
                   "A synthetic corrupt cache was bypassed." }, 20.0)
                   && weighted.result(StartupTaskId::pluginCatalog)->durationMs == 20.0
                   && weighted.result(StartupTaskId::pluginCatalog)->endedAt.toMilliseconds() == 1000020,
               "Warnings retain exact per-step duration and wall-clock timestamps.");
        expect(weighted.complete(tokens[0], {}, 30.0)
                   && weighted.readiness() == StartupReadiness::degraded,
               "An explicit warning permits entry, but never plain Ready.");
    }

    {
        StartupCoordinator bounded({
            { StartupTaskId::audio, "Audio", {}, true, true, 1.0, 100.0, 2 },
            { StartupTaskId::workspace, "Workspace", { StartupTaskId::audio }, true },
            { StartupTaskId::pluginRescan, "Optional scan", {}, false, true, 1.0, 1000.0 }
        });
        expect(bounded.begin(0.0).wasOk(), "A bounded retry graph can begin.");
        const auto first = bounded.takeRunnableTasks(0.0);
        const auto oldAudio = first[0];
        expect(bounded.takeRunnableTasks(100.0).empty(), "No dependent work starts after a required timeout.");
        expect(bounded.result(StartupTaskId::audio)->state == StartupState::timedOut
                   && bounded.result(StartupTaskId::workspace)->state == StartupState::blocked
                   && bounded.readiness() == StartupReadiness::blocked,
               "A required deadline blocks dependents and cannot silently report Ready.");
        expect(bounded.retry(StartupTaskId::audio).wasOk(), "A timed-out task permits its configured retry.");
        const auto second = bounded.takeRunnableTasks(110.0);
        expect(second.size() == 1 && second[0].attempt == 2
                   && !bounded.complete(oldAudio, {}, 111.0),
               "A late completion from the previous attempt cannot complete a retry.");
        expect(bounded.complete(second[0], {}, 120.0), "The current attempt can complete.");
        const auto readyWorkspace = bounded.takeRunnableTasks(120.0);
        expect(readyWorkspace.size() == 1 && bounded.complete(readyWorkspace[0], {}, 125.0)
                   && bounded.readiness() == StartupReadiness::ready,
               "Required completion enters the workspace while optional work is still running.");
        bounded.expire(1000.0);
        expect(bounded.readiness() == StartupReadiness::degraded
                   && bounded.result(StartupTaskId::pluginRescan)->state == StartupState::timedOut,
               "An optional timeout remains observable without blocking the workspace.");
        expect(bounded.retry(StartupTaskId::pluginRescan).failed(),
               "The retry policy is bounded rather than silently retrying forever.");
    }

    {
        StartupCoordinator failed({
            { StartupTaskId::audio, "Audio", {}, true, true, 1.0, 100.0, 2 },
            { StartupTaskId::midi, "MIDI", { StartupTaskId::audio }, true, true }
        });
        expect(failed.begin(0.0).wasOk(), "Audio recovery graph can begin.");
        const auto audio = failed.takeRunnableTasks(0.0).front();
        expect(failed.complete(audio, { StartupState::failed, StartupIssue::audioUnavailable,
            "Mock audio device unavailable." }, 10.0), "A device failure is a typed result.");
        expect(failed.takeRunnableTasks(10.0).empty(), "MIDI does not start after an unresolved audio failure.");
        expect(failed.readiness() == StartupReadiness::blocked,
               "Missing hardware is blocked until an explicit recovery action.");
        expect(failed.skip(StartupTaskId::audio, StartupIssue::safeAudio,
                   "Start without audio selected.", 15.0).wasOk(),
               "Start without audio explicitly resolves the required service.");
        const auto midi = failed.takeRunnableTasks(15.0);
        expect(midi.size() == 1 && failed.skip(StartupTaskId::midi, StartupIssue::safeAudio,
                   "MIDI deliberately disabled.", 16.0).wasOk()
                   && failed.readiness() == StartupReadiness::degraded
                   && !failed.complete(audio, {}, 17.0),
               "Audio-disabled startup is degraded and ignores a stale successful driver response.");
        expect(std::any_of(failed.history().begin(), failed.history().end(),
            [](const auto& entry) { return entry.state == StartupState::failed; }),
            "Recovery actions retain the original failure in step history.");
    }

    {
        StartupCoordinator cancelled({ { StartupTaskId::preferences, "Preferences", {}, true } });
        expect(cancelled.begin(0.0).wasOk(), "Cancellation graph can begin.");
        const auto token = cancelled.takeRunnableTasks(0.0).front();
        cancelled.cancel(5.0);
        expect(cancelled.readiness() == StartupReadiness::cancelled
                   && !cancelled.complete(token, {}, 6.0) && cancelled.takeRunnableTasks(6.0).empty(),
               "Cancellation prevents both late completions and new work.");
        expect(cancelled.skip(StartupTaskId::preferences, StartupIssue::none, {}, 6.0).failed(),
               "Non-skippable requirements cannot be turned into success-shaped fallbacks.");
    }

    {
        StartupCoordinator cycle({
            { StartupTaskId::preferences, "Preferences", { StartupTaskId::workspace } },
            { StartupTaskId::workspace, "Workspace", { StartupTaskId::preferences } }
        });
        StartupCoordinator missing({
            { StartupTaskId::workspace, "Workspace", { StartupTaskId::audio } }
        });
        StartupCoordinator duplicate({
            { StartupTaskId::audio, "Audio", {} }, { StartupTaskId::audio, "Other", {} }
        });
        expect(cycle.begin(0.0).failed() && missing.begin(0.0).failed() && duplicate.begin(0.0).failed(),
               "Cycles, missing prerequisites and duplicate stable IDs fail explicitly.");
    }
    {
        StartupCoordinator history({
            { StartupTaskId::audio, "Audio", {}, true, true, 1.0, 1000.0, 150 }
        });
        expect(history.begin(0.0).wasOk(), "Bounded-history graph can begin.");
        for (int index = 0; index < 150; ++index)
        {
            const auto token = history.takeRunnableTasks(index * 2.0).front();
            expect(history.complete(token, { StartupState::failed, StartupIssue::audioUnavailable,
                "Synthetic failure." }, index * 2.0 + 1.0), "Synthetic history attempt completes.");
            if (index < 149)
                expect(history.retry(StartupTaskId::audio).wasOk(), "Configured history retry is allowed.");
        }
        expect(history.history().size() == 128 && history.retry(StartupTaskId::audio).failed(),
               "History remains bounded even across many attempts and the final retry limit still applies.");
        expect(startupIssueCode(StartupIssue::pluginWorkerProtocol)
                   != startupIssueCode(StartupIssue::pluginValidationFailure)
                   && startupIssueCode(StartupIssue::pluginDiscoveryFailure)
                       != startupIssueCode(StartupIssue::pluginWorkerCrash),
               "Protocol/discovery failures are not mislabeled as deep validation or worker crashes.");
    }

    const auto fixture = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("studio-duo-startup-unit-" + juce::Uuid().toString());
    expect(fixture.createDirectory().wasOk(), "Synthetic startup fixture directory can be created.");
    {
        StartupCoordinator privateData({ { StartupTaskId::project, "MOCK_PRIVATE_PROJECT", {}, true } });
        expect(privateData.begin(0.0).wasOk(), "Privacy fixture graph can begin.");
        const auto token = privateData.takeRunnableTasks(0.0).front();
        privateData.complete(token, { StartupState::warning, StartupIssue::projectUnavailable,
            "MOCK_PERSONAL_METADATA /Users/mock-private/project.song C:\\Users\\mock-private\\audio.wav "
            "MOCK_PLUGIN_STATE MOCK_AUDIO_CONTENT" }, 10.0);
        StartupDeviceSummary devices;
        devices.sampleRate = 48000.0;
        devices.bufferSize = 256;
        StartupPluginSummary plugins;
        plugins.ready = 100;
        plugins.failures[3] = 2;
        const auto report = startupDiagnosticReport(privateData, devices, plugins, true, true, 12.0);
        const auto json = juce::JSON::toString(report);
        expect(!json.contains("MOCK_") && !json.contains("/Users/") && !json.contains("C:\\")
                   && json.contains("startup.project") && json.contains("durationMs")
                   && json.contains("project-unavailable") && json.contains("48000"),
               "Export uses stable categories and numeric summaries, never arbitrary task names or private details.");
        const auto reportDirectory = fixture.getChildFile("reports");
        for (int index = 0; index < 12; ++index)
        {
            StartupDiagnosticStore store(reportDirectory);
            expect(store.persist(report).wasOk(), "A redacted diagnostic snapshot can be persisted.");
        }
        expect(reportDirectory.findChildFiles(juce::File::findFiles, false, "startup-*.json").size() == 10,
               "Startup diagnostics retain at most ten bounded reports.");
        const auto blockedParent = fixture.getChildFile("not-a-directory");
        expect(blockedParent.replaceWithText("fixture")
                   && writeStartupDiagnostic(blockedParent.getChildFile("report.json"), report).failed(),
               "Diagnostic I/O failure returns an actionable error instead of claiming export success.");
    }
    {
        const auto markers = fixture.getChildFile("markers");
        StartupRecovery first(markers);
        expect(first.inspectAndMarkActive().result.wasOk(), "A clean launch creates its own recovery marker.");
        const auto firstValue = juce::JSON::parse(first.markerFile().loadFileAsString());
        const auto pid = static_cast<juce::int64>(firstValue.getProperty("pid", {}));
        expect(markers.getChildFile("active-dead.json").replaceWithText("{\"schemaVersion\":1,\"pid\":2147483647}")
                   && markers.getChildFile("active-corrupt.json").replaceWithText("{broken"),
               "Synthetic stale and corrupt markers can be created.");
        StartupRecovery second(markers, [pid](juce::int64 other) { return other == pid; });
        const auto check = second.inspectAndMarkActive();
        expect(check.result.wasOk() && check.unclean && check.previousSessions == 2
                   && first.markerFile().existsAsFile(),
               "Unclean/corrupt markers trigger recovery without mistaking a live session for a crash.");
        expect(second.acknowledgePreviousSessions().wasOk()
                   && first.markerFile().existsAsFile() && second.markerFile().existsAsFile(),
               "Acknowledgment removes only the stale markers, preserving live sessions.");
        expect(first.finishCleanShutdown().wasOk() && second.finishCleanShutdown().wasOk()
                   && markers.findChildFiles(juce::File::findFiles, false, "active-*.json").isEmpty(),
               "Clean shutdown clears only the corresponding per-session markers.");
    }
    {
        const auto directory = fixture.getChildFile("catalog");
        expect(directory.createDirectory().wasOk()
                   && directory.getChildFile("plugins.xml").replaceWithText("<corrupt"),
               "Synthetic corrupt catalog can be created.");
        PluginCatalog corrupt(directory);
        expect(corrupt.cacheWarning().isNotEmpty() && corrupt.cacheNeedsRescan()
                   && !corrupt.entries().empty() && !corrupt.isScanning(),
               "Corrupt catalog remains visible, preserves bundled devices and does not force a scan.");
        juce::KnownPluginList known;
        juce::PluginDescription plugin;
        plugin.name = "Mock Processor";
        plugin.manufacturerName = "Mock Vendor";
        plugin.pluginFormatName = "VST3";
        plugin.fileOrIdentifier = directory.getChildFile("Mock.vst3").getFullPathName();
        plugin.uniqueId = 123;
        known.addType(plugin);
        const auto xml = known.createXml();
        xml->setAttribute("studioDuoCatalogVersion", 1);
        xml->setAttribute("studioDuoAppVersion", STUDIO_DUO_VERSION);
        xml->setAttribute("studioDuoArchitecture", JUCE_ARM ? "arm64" : JUCE_64BIT ? "x86_64" : "x86");
        expect(directory.getChildFile("plugins.xml").replaceWithText(xml->toString()),
               "Synthetic warm catalog can be written.");
        PluginCatalog warm(directory);
        expect(warm.cacheWarning().isEmpty() && !warm.cacheNeedsRescan() && !warm.isScanning()
                   && warm.descriptionForIdentifier(plugin.createIdentifierString()).has_value(),
               "Warm startup restores the cached description without instantiating DSP or requiring a rescan.");
        expect(directory.getChildFile("preferences.json").replaceWithText(
            "{\"autosaveEnabled\":true,\"recentProjects\":[{\"path\":\"relative.song\",\"lastEditedMs\":1}]}"),
               "Synthetic corrupt recent metadata can be written.");
        StudioPreferences preferences(directory.getChildFile("preferences.json"));
        expect(preferences.recentProjects().empty() && preferences.status().isNotEmpty(),
               "Invalid recent paths produce an explicit warning and never assert in file construction.");
    }
    {
        StartupProgressComponent surface;
        surface.setSize(900, 600);
        surface.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        auto paintNotifications = 0;
        surface.onFirstPaint = [&] { ++paintNotifications; };
        juce::Image image(juce::Image::ARGB, 900, 600, true);
        juce::Graphics graphics(image);
        surface.paintEntireComponent(graphics, true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        expect(surface.hasPainted() && paintNotifications == 1,
               "The lightweight surface paints before notifying the startup owner to begin initialization.");
        surface.setSnapshot(startup, true);
        auto keyboardButtons = 0;
        auto accessibleProgress = false;
        for (auto* child : surface.getChildren())
        {
            if (auto* button = dynamic_cast<juce::TextButton*>(child); button != nullptr && button->isVisible())
                if (button->getWantsKeyboardFocus() && button->getAccessibilityHandler() != nullptr)
                    ++keyboardButtons;
            if (auto* handler = child->getAccessibilityHandler(); handler != nullptr
                && handler->getRole() == juce::AccessibilityRole::progressBar)
                accessibleProgress = handler->getValueInterface() != nullptr
                    && handler->getValueInterface()->getCurrentValue() == 1.0;
        }
        auto closed = false;
        surface.onClose = [&] { closed = true; };
        expect(keyboardButtons >= 3, "History/export/close use native keyboard buttons with accessibility handlers.");
        expect(accessibleProgress, "Startup progress exposes the completed numeric accessibility value.");
        expect(surface.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && closed,
               "Escape restores the workspace when its required services are available.");
    }
    expect(fixture.deleteRecursively(), "Synthetic startup fixtures are removed.");
}

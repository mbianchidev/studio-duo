#include "StartupShellComponent.h"

#include "MainComponent.h"
#include "audio/AudioDeviceProbe.h"
#include "logging/StudioLogger.h"
#include "platform/Architecture.h"

#include <algorithm>

namespace studio
{
namespace
{
double now() { return juce::Time::getMillisecondCounterHiRes(); }

bool usable(const StartupTaskResult* result)
{
    return result != nullptr && startupStateAllowsDependents(result->state);
}
}

std::vector<StartupTask> StartupShellComponent::tasks()
{
    using Id = StartupTaskId;
    return {
        { Id::welcome, "Welcome, build and local storage", {}, true, true, 1.0, 2000.0 },
        { Id::preferences, "Local preferences and recent projects", { Id::welcome }, true, true, 1.0, 5000.0 },
        { Id::recovery, "Previous-session recovery", { Id::welcome }, true, true, 1.0, 5000.0 },
        { Id::pluginCatalog, "Cached plug-in catalog", { Id::welcome }, true, true, 2.0, 15000.0 },
        { Id::workspace, "Workspace", { Id::preferences, Id::recovery, Id::pluginCatalog }, true, false, 2.0, 15000.0 },
        { Id::audio, "Audio-device safety check and engine", { Id::preferences, Id::recovery }, true, true, 3.0, 15000.0, 3 },
        { Id::midi, "MIDI services", { Id::workspace, Id::audio }, true, true, 1.0, 5000.0 },
        { Id::project, "Requested project", { Id::workspace, Id::audio, Id::midi }, false, true, 1.0, 120000.0 },
        { Id::pluginRescan, "Background catalog change check", { Id::workspace, Id::recovery }, false, true, 1.0, 120000.0 },
        { Id::diagnostics, "Redacted startup diagnostics", { Id::workspace, Id::audio, Id::midi }, false, true, 1.0, 5000.0 }
    };
}

StartupShellComponent::StartupShellComponent(StartupOptions selected)
    : options(std::move(selected))
{
    if (options.selfTest)
    {
        testDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("studio-duo-startup-test-" + juce::Uuid().toString());
        dataDirectory = testDirectory;
    }
    else
    {
        dataDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Studio Duo");
    }
    recovery = std::make_shared<StartupRecovery>(dataDirectory.getChildFile("Startup").getChildFile("Sessions"));
    diagnostics = std::make_shared<StartupDiagnosticStore>(dataDirectory.getChildFile("Startup").getChildFile("Reports"));
    setLookAndFeel(&theme);
    setOpaque(true);
    addAndMakeVisible(surface);
    surface.onFirstPaint = [this] { begin(); };
    surface.onRetry = [this] { retryAudio(); };
    surface.onWithoutAudio = [this] { continueWithoutAudio(); };
    surface.onSafeStart = [this] { enterSafeStart(); };
    surface.onResume = [this] { acceptRecovery(false); };
    surface.onOpenIo = [this] { openIo(); };
    surface.onCancelScan = [this]
    {
        if (workspace != nullptr)
            workspace->startupCatalog().cancelScan();
        if (scanToken && startup.accepts(*scanToken))
            startup.complete(*scanToken, { StartupState::cancelled, StartupIssue::cancelled,
                "The optional scan was cancelled. Cached plug-ins remain available." }, now());
    };
    surface.onExport = [this] { exportReport(); };
    surface.onClose = [this]
    {
        if (!requiredServicesAvailable())
            return;
        historyOpen = false;
        surface.setVisible(false);
        if (workspace != nullptr)
            workspace->grabKeyboardFocus();
    };
    setSize(1480, 900);
}

StartupShellComponent::~StartupShellComponent()
{
    prepareForShutdown();
    workspace.reset();
    setLookAndFeel(nullptr);
    if (testDirectory != juce::File() && !testDirectory.deleteRecursively())
        logError("startup.test", "Could not remove the isolated startup fixture directory.");
}

void StartupShellComponent::post(const std::shared_ptr<Mailbox>& target, Completion completion)
{
    const std::lock_guard lock(target->mutex);
    target->completed.push_back(std::move(completion));
}

void StartupShellComponent::begin()
{
    firstPaintMs = now() - constructedMs;
    if (const auto result = startup.begin(now()); result.failed())
    {
        logError("startup.graph", result.getErrorMessage());
        surface.setNotice(result.getErrorMessage());
        if (options.selfTest)
            juce::NullCheckedInvocation::invoke(onSelfTestFinished, false, report());
        return;
    }
    startTimer(20);
    surface.grabKeyboardFocus();
    timerCallback();
}

void StartupShellComponent::timerCallback()
{
    if (stopped || nativeAudioActivationInFlight)
        return;
    if (startup.result(StartupTaskId::pluginCatalog)->state == StartupState::running)
        ++responsiveWorkerTicks;
    collectCompletions();
    startup.expire(now());
    if (audioProbeInFlight && !startup.accepts({ StartupTaskId::audio,
                                                startup.result(StartupTaskId::audio)->attempt }))
        audioProbeCancellation->store(true, std::memory_order_release);
    if (workspace != nullptr && !options.selfTest && ++devicePollTicks >= 50)
    {
        devicePollTicks = 0;
        const auto audioState = startup.result(StartupTaskId::audio)->state;
        if (startupStateIsTerminal(audioState)
            && (!startupStateAllowsDependents(audioState) || audioState == StartupState::skipped)
            && workspace->startupDeviceSummary().audioEnabled)
        {
            deviceSummary = workspace->startupDeviceSummary();
            if (const auto resolved = startup.resolve(StartupTaskId::audio, {}, now()); resolved.failed())
                logError("startup.audio", resolved.getErrorMessage());
            if (startup.result(StartupTaskId::midi)->state == StartupState::skipped)
                if (const auto resolved = startup.resolve(StartupTaskId::midi,
                    workspace->initialiseStartupMidi(savedAudioSetup.get()), now()); resolved.failed())
                    logError("startup.midi", resolved.getErrorMessage());
        }
    }
    for (const auto token : startup.takeRunnableTasks(now()))
        startTask(token);
    activateAudio();
    updateScan();
    const auto available = requiredServicesAvailable();
    if (available && workspace != nullptr && !workspaceEntered)
    {
        workspaceEntered = true;
        workspaceReadiness = startup.readiness();
        workspace->setStartupAvailable(startup.readiness() == StartupReadiness::degraded);
        surface.setVisible(false);
        workspace->grabKeyboardFocus();
    }
    else if (available && workspace != nullptr && workspaceReadiness != startup.readiness())
    {
        workspaceReadiness = startup.readiness();
        workspace->setStartupAvailable(workspaceReadiness == StartupReadiness::degraded);
    }
    refreshSurface();
    const auto* diagnosticStep = startup.result(StartupTaskId::diagnostics);
    if (!diagnosticsDisabled && !persistInFlight && usable(diagnosticStep)
        && persistedRevision != startup.revision() && now() - lastPersistMs >= 250.0)
        persistReport({ StartupTaskId::diagnostics, 0 });
    checkSelfTest();
}

void StartupShellComponent::startTask(StartupToken token)
{
    const auto target = mailbox;
    const auto root = dataDirectory;
    const auto scenario = options.selfTest ? options.selfTestScenario : juce::String();
    switch (token.id)
    {
        case StartupTaskId::welcome:
            workers.addJob([target, root, token]
            {
                Completion completion { token };
                const auto created = root.createDirectory();
                completion.outcome = { created.failed() ? StartupState::warning : StartupState::succeeded,
                    created.failed() ? StartupIssue::storageUnavailable : StartupIssue::none,
                    "Studio Duo " + juce::String(STUDIO_DUO_VERSION) + " on "
                    + juce::SystemStats::getOperatingSystemName()
                    + ". Local AGPLv3 application; no account or host-license check. "
                    + (created.failed() ? "Local storage is unavailable; settings, recovery and diagnostics may not be saved."
                                        : "Local application-data storage is ready.") };
                post(target, std::move(completion));
            });
            break;
        case StartupTaskId::preferences:
            workers.addJob([target, root, token]
            {
                if (target->cancelled->load(std::memory_order_acquire))
                    return;
                Completion completion { token };
                completion.preferences = std::make_unique<StudioPreferences>(root.getChildFile("preferences.json"));
                const auto warning = completion.preferences->status();
                completion.outcome = { warning.isNotEmpty() ? StartupState::warning : StartupState::succeeded,
                    warning.isNotEmpty() ? StartupIssue::preferencesInvalid : StartupIssue::none,
                    warning.isNotEmpty() ? warning
                        : "Loaded local preferences and " + juce::String(static_cast<int>(
                            completion.preferences->recentProjects().size())) + " recent-project record(s)." };
                post(target, std::move(completion));
            });
            break;
        case StartupTaskId::recovery:
        {
            const auto marker = recovery;
            workers.addJob([target, marker, token, scenario]
            {
                if (target->cancelled->load(std::memory_order_acquire))
                    return;
                Completion completion { token };
                if (scenario == "recovery")
                {
                    const auto fixture = marker->markerFile().getSiblingFile("active-fixture.json");
                    if (!fixture.getParentDirectory().createDirectory()
                        || !fixture.replaceWithText("{\"schemaVersion\":1,\"pid\":2147483647}"))
                    {
                        completion.outcome = { StartupState::failed, StartupIssue::storageUnavailable,
                            "Could not create the synthetic recovery marker." };
                        post(target, std::move(completion));
                        return;
                    }
                }
                completion.recoveryCheck = marker->inspectAndMarkActive();
                const auto& check = completion.recoveryCheck;
                completion.outcome = {
                    check.unclean ? StartupState::failed
                        : check.result.failed() ? StartupState::warning : StartupState::succeeded,
                    check.unclean ? StartupIssue::recoveryRequired
                        : check.result.failed() ? StartupIssue::storageUnavailable : StartupIssue::none,
                    check.unclean
                        ? "A previous session did not close cleanly. Safe start bypasses audio/MIDI and third-party runtimes."
                        : check.result.failed() ? check.result.getErrorMessage()
                                               : "No unclean previous session. This session is protected by a recovery marker." };
                post(target, std::move(completion));
            });
            break;
        }
        case StartupTaskId::pluginCatalog:
            workers.addJob([target, root, token, scenario]
            {
                if (target->cancelled->load(std::memory_order_acquire))
                    return;
                Completion completion { token };
                if (scenario == "slow-catalog")
                    juce::Thread::sleep(300);
                const auto directory = root.getChildFile("Plugin Catalog");
                if (scenario == "warm-catalog" || scenario == "large-catalog")
                {
                    juce::MemoryOutputStream xml;
                    xml << "<KNOWNPLUGINS studioDuoCatalogVersion=\"1\" studioDuoAppVersion=\""
                        << STUDIO_DUO_VERSION << "\" studioDuoArchitecture=\""
                        << currentArchitecture() << "\">\n";
                    const auto count = scenario == "large-catalog" ? 20000 : 128;
                    for (int index = 0; index < count; ++index)
                    {
                        if (target->cancelled->load(std::memory_order_acquire))
                            return;
                        juce::PluginDescription plugin;
                        plugin.name = "Mock Processor " + juce::String(index);
                        plugin.manufacturerName = "Mock Vendor";
                        plugin.pluginFormatName = "VST3";
                        plugin.uniqueId = index + 1;
                        plugin.fileOrIdentifier = directory.getChildFile("Mock-" + juce::String(index) + ".vst3").getFullPathName();
                        xml << plugin.createXml()->toString() << "\n";
                    }
                    xml << "</KNOWNPLUGINS>\n";
                    if (!directory.createDirectory()
                        || !directory.getChildFile("plugins.xml").replaceWithText(xml.toString()))
                    {
                        completion.outcome = { StartupState::failed, StartupIssue::storageUnavailable,
                            "Could not create the synthetic warm/large catalog." };
                        post(target, std::move(completion));
                        return;
                    }
                }
                if (scenario == "corrupt-catalog")
                {
                    if (!directory.createDirectory()
                        || !directory.getChildFile("plugins.xml").replaceWithText("<corrupt"))
                    {
                        completion.outcome = { StartupState::failed, StartupIssue::storageUnavailable,
                            "Could not create the synthetic corrupt catalog." };
                        post(target, std::move(completion));
                        return;
                    }
                }
                completion.catalog = std::make_unique<PluginCatalog>(directory);
                const auto warning = completion.catalog->cacheWarning();
                const auto count = completion.catalog->entries().size();
                completion.outcome = { warning.isNotEmpty() ? StartupState::warning : StartupState::succeeded,
                    warning.isNotEmpty() ? StartupIssue::catalogInvalid : StartupIssue::none,
                    warning.isNotEmpty() ? warning
                        : "Restored " + juce::String(static_cast<int>(count)) + " plug-in/device entries; "
                            + juce::String(completion.catalog->blacklistedFiles().size()) + " blocked. "
                            + (completion.catalog->cacheNeedsRescan()
                                ? "Catalog metadata changed; an optional background check is available."
                                : "Warm catalog restored without a mandatory full scan.") };
                post(target, std::move(completion));
            });
            break;
        case StartupTaskId::workspace:
            if (restoredPreferences == nullptr || restoredCatalog == nullptr)
            {
                startup.complete(token, { StartupState::failed, StartupIssue::storageUnavailable,
                    "Required startup data is unavailable. Choose Safe start to use explicit defaults." }, now());
                break;
            }
            workspace = std::make_unique<MainComponent>(
                std::move(*restoredPreferences), std::move(restoredCatalog),
                options.safePlugins, options.defaultAudio, !options.selfTest);
            restoredPreferences.reset();
            workspace->onShowStartupDiagnostics = [this]
            {
                historyOpen = true;
                surface.setVisible(true);
                surface.toFront(false);
                surface.grabKeyboardFocus();
            };
            addAndMakeVisible(*workspace);
            surface.toFront(false);
            resized();
            startup.complete(token, { StartupState::succeeded, StartupIssue::none,
                options.safePlugins ? "Workspace created with third-party runtime restoration bypassed."
                                    : "Workspace created. Required services still control readiness." }, now());
            break;
        case StartupTaskId::audio:
            if ((options.safeAudio && !(options.selfTest && scenario == "missing-audio"))
                || (options.selfTest && scenario != "missing-audio"))
            {
                if (const auto skipped = startup.skip(token.id, StartupIssue::safeAudio,
                    "Audio/MIDI disabled for this launch. No device manager or driver is loaded.", now()); skipped.failed())
                    logError("startup.audio", skipped.getErrorMessage());
                break;
            }
            audioProbeCancellation = std::make_shared<std::atomic<bool>>(false);
            audioProbeInFlight = true;
            workers.addJob([target, root, token, scenario, cancellation = audioProbeCancellation,
                            defaultAudio = options.defaultAudio]
            {
                if (target->cancelled->load(std::memory_order_acquire))
                    return;
                Completion completion { token };
                if (scenario == "missing-audio")
                {
                    completion.outcome = { StartupState::failed, StartupIssue::audioUnavailable,
                        "The synthetic selected audio device is unavailable. Retry, Open I/O, or Start without audio." };
                    post(target, std::move(completion));
                    return;
                }
                const auto settings = root.getChildFile("audio-device.xml");
                if (!defaultAudio && settings.existsAsFile())
                {
                    completion.audioSetup = settings.getSize() <= 1024 * 1024 ? juce::parseXML(settings) : nullptr;
                    if (completion.audioSetup == nullptr || !completion.audioSetup->hasTagName("DEVICESETUP"))
                    {
                        completion.audioSetup.reset();
                        completion.invalidAudioSettings = true;
                    }
                }
                juce::XmlElement request("DEVICESETUP");
                request.setAttribute("deviceType", studioAudioStartupProbeType);
                if (completion.audioSetup != nullptr)
                    request.addChildElement(new juce::XmlElement(*completion.audioSetup));
                AudioDeviceProbeOptions probeOptions;
                probeOptions.cancelled = cancellation;
                const auto checked = probeAudioDeviceSetupDetailed(request, probeOptions);
                completion.audioPreflightPassed = checked.result.wasOk();
                completion.outcome = {
                    checked.failure == AudioDeviceProbeFailure::cancelled ? StartupState::cancelled
                        : checked.failure == AudioDeviceProbeFailure::timeout ? StartupState::timedOut
                        : checked.result.failed() ? StartupState::failed : StartupState::succeeded,
                    checked.failure == AudioDeviceProbeFailure::cancelled ? StartupIssue::cancelled
                        : checked.failure == AudioDeviceProbeFailure::timeout ? StartupIssue::timeout
                        : checked.result.failed() ? StartupIssue::audioUnavailable : StartupIssue::none,
                    checked.result.failed()
                        ? "Audio is disabled: " + checked.result.getErrorMessage()
                            + " Retry, Open I/O, or Start without audio."
                        : "The isolated driver startup check passed." };
                post(target, std::move(completion));
            });
            break;
        case StartupTaskId::midi:
            if (options.selfTest && scenario == "missing-midi")
            {
                deviceSummary.unavailableMidiDevices = 1;
                startup.complete(token, { StartupState::warning, StartupIssue::midiUnavailable,
                    "The synthetic saved MIDI input is unavailable; audio-independent workspace access remains available." }, now());
            }
            else if (!deviceSummary.audioEnabled)
            {
                if (const auto skipped = startup.skip(token.id, StartupIssue::safeAudio,
                    "Hardware MIDI is disabled with audio. Retrospective capture requires active audio processing.", now()); skipped.failed())
                    logError("startup.midi", skipped.getErrorMessage());
            }
            else
            {
                const auto outcome = workspace->initialiseStartupMidi(savedAudioSetup.get());
                deviceSummary = workspace->startupDeviceSummary();
                startup.complete(token, outcome, now());
            }
            break;
        case StartupTaskId::project:
            if (!workspaceEntered)
            {
                workspace->setStartupAvailable(startup.readiness() == StartupReadiness::degraded);
                workspaceEntered = true;
                workspaceReadiness = startup.readiness();
                surface.setVisible(false);
            }
            if (options.project == juce::File())
            {
                if (const auto skipped = startup.skip(token.id, StartupIssue::none,
                    "No startup project was requested. The local startup hub is available.", now()); skipped.failed())
                    logError("startup.project", skipped.getErrorMessage());
            }
            else
            {
                startup.complete(token, workspace->openStartupProject(options.project), now());
            }
            break;
        case StartupTaskId::pluginRescan:
            if (options.selfTest && scenario == "plugin-timeout")
            {
                startup.complete(token, { StartupState::timedOut, StartupIssue::pluginWorkerTimeout,
                    "The synthetic plug-in worker exceeded its deadline. Cached entries remain usable." }, now());
            }
            else if (options.selfTest || options.safePlugins || !scanOnStartup)
            {
                if (const auto skipped = startup.skip(token.id, StartupIssue::scanDisabled,
                    "Optional background discovery is disabled by the startup preference or safe/test mode.", now()); skipped.failed())
                    logError("startup.plugin-rescan", skipped.getErrorMessage());
            }
            else
            {
                scanToken = token;
                workspace->startupCatalog().startScan(workspace->startupCatalog().cacheNeedsRescan());
            }
            break;
        case StartupTaskId::diagnostics:
            persistReport(token);
            break;
    }
}

void StartupShellComponent::collectCompletions()
{
    std::vector<Completion> completed;
    {
        const std::lock_guard lock(mailbox->mutex);
        completed.swap(mailbox->completed);
    }
    for (auto& completion : completed)
    {
        if (completion.ioDiscovery)
        {
            ioDiscoveryInFlight = false;
            if (completion.outcome.state == StartupState::succeeded && workspace != nullptr)
                workspace->openStartupIo(true);
            else
            {
                surface.setNotice(completion.outcome.detail);
                logError("startup.io", completion.outcome.detail);
            }
            continue;
        }
        if (completion.token.id == StartupTaskId::audio)
            audioProbeInFlight = false;
        if (completion.token.id == StartupTaskId::diagnostics)
        {
            persistInFlight = false;
            if (completion.token.attempt == 0)
            {
                if (completion.outcome.state == StartupState::failed)
                {
                    diagnosticsDisabled = true;
                    surface.setNotice(completion.outcome.detail);
                    logError("startup.diagnostics", completion.outcome.detail);
                    if (const auto recovered = startup.resolve(StartupTaskId::diagnostics,
                        { StartupState::warning, StartupIssue::storageUnavailable, completion.outcome.detail }, now()); recovered.failed())
                        logError("startup.diagnostics", recovered.getErrorMessage());
                }
                continue;
            }
        }
        if (!startup.accepts(completion.token))
            continue;
        if (completion.preferences != nullptr)
        {
            scanOnStartup = completion.preferences->scanPluginsAtStartup();
            restoredPreferences = std::move(completion.preferences);
        }
        if (completion.catalog != nullptr)
        {
            restoredCatalog = std::move(completion.catalog);
            updatePluginSummary();
        }
        if (completion.audioPreflightPassed)
        {
            savedAudioSetup = std::move(completion.audioSetup);
            invalidAudioSettings = completion.invalidAudioSettings;
            pendingAudioActivation = completion.token;
            startup.updateProgress(completion.token, 0.8, "Driver safety check passed; attaching the engine on its owning message thread.");
            continue;
        }
        startup.complete(completion.token, std::move(completion.outcome), now());
        if (options.selfTest && completion.token.id == StartupTaskId::recovery
            && startup.result(StartupTaskId::recovery)->issue == StartupIssue::recoveryRequired)
            enterSafeStart();
        if (options.selfTest && completion.token.id == StartupTaskId::audio)
            continueWithoutAudio();
    }
}

void StartupShellComponent::activateAudio()
{
    if (!pendingAudioActivation || workspace == nullptr)
        return;
    const auto token = *pendingAudioActivation;
    pendingAudioActivation.reset();
    if (!startup.accepts(token))
        return;
    auto opened = juce::Result::ok();
    {
        const juce::ScopedValueSetter<bool> activationGuard(nativeAudioActivationInFlight, true);
        opened = workspace->initialiseStartupAudio(savedAudioSetup.get());
    }
    deviceSummary = workspace->startupDeviceSummary();
    const auto warning = invalidAudioSettings || workspace->startupAudioHasWarning();
    const auto detail = opened.failed() ? "Audio could not start: " + opened.getErrorMessage()
        : workspace->startupAudioDescription()
            + (invalidAudioSettings ? " Invalid saved settings were bypassed." : "")
            + (options.defaultAudio ? " The previously selected device was bypassed for this launch; saved settings are unchanged." : "");
    if (!startup.complete(token, { opened.failed() ? StartupState::failed
            : warning ? StartupState::warning : StartupState::succeeded,
        opened.failed() ? StartupIssue::audioUnavailable
            : workspace->startupAudioHasWarning() ? StartupIssue::audioUnavailable
            : invalidAudioSettings ? StartupIssue::preferencesInvalid : StartupIssue::none, detail }, now()))
    {
        workspace->disableStartupAudio();
        deviceSummary = {};
    }
    if (safeStartAfterActivation)
    {
        safeStartAfterActivation = false;
        withoutAudioAfterActivation = false;
        enterSafeStart();
    }
    else if (withoutAudioAfterActivation)
    {
        withoutAudioAfterActivation = false;
        continueWithoutAudio();
    }
    if (shutdownAfterActivation)
        juce::MessageManager::callAsync([] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
}

void StartupShellComponent::retryAudio()
{
    if (audioProbeInFlight || nativeAudioActivationInFlight)
    {
        surface.setNotice("The previous audio check is still stopping. Retry after it exits.");
        return;
    }
    if (const auto retried = startup.retry(StartupTaskId::audio); retried.failed())
    {
        logError("startup.audio", retried.getErrorMessage());
        surface.setNotice(retried.getErrorMessage());
    }
}

void StartupShellComponent::openIo()
{
    if (workspace == nullptr || ioDiscoveryInFlight || audioProbeInFlight || nativeAudioActivationInFlight)
    {
        surface.setNotice("Wait for the active device check to finish before opening I/O.");
        return;
    }
    ioDiscoveryInFlight = true;
    surface.setNotice("Checking hardware MIDI discovery before opening I/O. The workspace stays responsive.");
    const auto target = mailbox;
    workers.addJob([target]
    {
        Completion completion({ StartupTaskId::audio, 0 });
        completion.ioDiscovery = true;
        juce::XmlElement discovery("DEVICESETUP");
        discovery.setAttribute("deviceType", midiDeviceDiscoveryProbeType);
        AudioDeviceProbeOptions probeOptions;
        probeOptions.cancelled = target->cancelled;
        const auto checked = probeAudioDeviceSetupDetailed(discovery, probeOptions);
        completion.outcome = { checked.result.wasOk() ? StartupState::succeeded : StartupState::failed,
            checked.result.wasOk() ? StartupIssue::none : StartupIssue::midiUnavailable,
            checked.result.wasOk() ? "Hardware discovery passed."
                : "I/O discovery failed: " + checked.result.getErrorMessage() };
        post(target, std::move(completion));
    });
}

void StartupShellComponent::continueWithoutAudio()
{
    if (nativeAudioActivationInFlight)
    {
        withoutAudioAfterActivation = true;
        surface.setNotice("Audio activation is finishing. Audio will be disabled as soon as the driver returns.");
        return;
    }
    options.safeAudio = true;
    if (audioProbeCancellation != nullptr)
        audioProbeCancellation->store(true, std::memory_order_release);
    pendingAudioActivation.reset();
    if (workspace != nullptr && workspace->hasAudioDeviceManager())
        workspace->disableStartupAudio();
    deviceSummary = {};
    if (const auto skipped = startup.skip(StartupTaskId::audio, StartupIssue::safeAudio,
        "Audio is explicitly disabled for this launch. Open I/O later to select a device.", now()); skipped.failed())
        logError("startup.audio", skipped.getErrorMessage());
}

void StartupShellComponent::acceptRecovery(bool safe)
{
    const auto* step = startup.result(StartupTaskId::recovery);
    if (step->issue != StartupIssue::recoveryRequired)
        return;
    if (safe)
    {
        options.safePlugins = true;
        options.safeAudio = true;
    }
    if (const auto skipped = startup.skip(StartupTaskId::recovery, StartupIssue::recoveryRequired,
        safe ? "Safe start selected after an unclean session. Third-party runtimes and audio/MIDI are bypassed."
             : "Normal startup explicitly selected after an unclean session.", now()); skipped.failed())
        logError("startup.recovery", skipped.getErrorMessage());
    const auto marker = recovery;
    workers.addJob([marker]
    {
        if (const auto acknowledged = marker->acknowledgePreviousSessions(); acknowledged.failed())
            logError("startup.recovery", acknowledged.getErrorMessage());
    });
}

void StartupShellComponent::enterSafeStart()
{
    if (nativeAudioActivationInFlight)
    {
        safeStartAfterActivation = true;
        surface.setNotice("Safe start will be applied after the native driver returns.");
        return;
    }
    options.safeAudio = true;
    options.safePlugins = true;
    acceptRecovery(true);
    for (const auto id : { StartupTaskId::welcome, StartupTaskId::preferences,
                          StartupTaskId::pluginCatalog, StartupTaskId::recovery })
    {
        const auto* step = startup.result(id);
        if (startupStateIsTerminal(step->state) && !startupStateAllowsDependents(step->state))
        {
            if (id == StartupTaskId::preferences && restoredPreferences == nullptr)
                restoredPreferences = std::make_unique<StudioPreferences>(dataDirectory.getChildFile("preferences.json"), false);
            if (id == StartupTaskId::pluginCatalog && restoredCatalog == nullptr)
                restoredCatalog = std::make_unique<PluginCatalog>(dataDirectory.getChildFile("Plugin Catalog"), false);
            if (const auto skipped = startup.skip(id, StartupIssue::safePlugins,
                "Explicit safe defaults selected; the failed cached data was not restored.", now()); skipped.failed())
                logError(startupTaskId(id), skipped.getErrorMessage());
        }
    }
    if (workspace != nullptr && !usable(startup.result(StartupTaskId::workspace)))
        if (const auto resolved = startup.resolve(StartupTaskId::workspace,
            { StartupState::warning, StartupIssue::safePlugins,
              "The constructed workspace is available with explicit safe defaults." }, now()); resolved.failed())
            logError("startup.workspace", resolved.getErrorMessage());
    if (workspace != nullptr)
        workspace->bypassStartupPlugins();
    continueWithoutAudio();
}

bool StartupShellComponent::requiredServicesAvailable() const
{
    return startup.readiness() == StartupReadiness::ready || startup.readiness() == StartupReadiness::degraded;
}

void StartupShellComponent::refreshSurface()
{
    if (displayedRevision == startup.revision())
        return;
    displayedRevision = startup.revision();
    surface.setSnapshot(startup, workspace != nullptr);
    if (!requiredServicesAvailable() || historyOpen)
    {
        surface.setVisible(true);
        surface.toFront(false);
    }
}

void StartupShellComponent::updatePluginSummary()
{
    const auto* catalog = workspace != nullptr ? &workspace->startupCatalog() : restoredCatalog.get();
    if (catalog == nullptr)
        return;
    pluginSummary = {};
    for (const auto& entry : catalog->entries())
        entry.bundledDevice ? ++pluginSummary.bundled : ++pluginSummary.ready;
    pluginSummary.blocked = catalog->blacklistedFiles().size();
    for (const auto& failure : catalog->compatibilityRecords())
    {
        const auto index = static_cast<std::size_t>(failure.lastFailure);
        if (index < pluginSummary.failures.size())
            ++pluginSummary.failures[index];
        if (failure.validationStatus.isNotEmpty() && failure.validationStatus != "pass")
            ++pluginSummary.failedValidations;
        pluginSummary.scanCrashes += juce::jmax(0, failure.scanCrashCount);
        pluginSummary.runtimeCrashes += juce::jmax(0, failure.runtimeCrashCount);
        pluginSummary.timeouts += juce::jmax(0, failure.timeoutCount);
    }
}

void StartupShellComponent::updateScan()
{
    if (!scanToken || workspace == nullptr)
        return;
    auto& catalog = workspace->startupCatalog();
    if (!startup.accepts(*scanToken))
    {
        if (catalog.isScanning())
            catalog.cancelScan();
        scanToken.reset();
        return;
    }
    if (catalog.isScanning())
    {
        startup.updateProgress(*scanToken, catalog.progress(), catalog.status());
        return;
    }
    updatePluginSummary();
    const auto scanState = catalog.scanState();
    const auto timeout = scanState == PluginScanState::timedOut
        || catalog.scanFailure() == PluginFailureKind::timeout;
    const auto failure = catalog.scanFailure() != PluginFailureKind::none
        || scanState == PluginScanState::warning || scanState == PluginScanState::failed;
    const auto issue = scanState == PluginScanState::failed ? StartupIssue::storageUnavailable
        : catalog.scanFailure() == PluginFailureKind::scanCrash ? StartupIssue::pluginWorkerCrash
        : catalog.scanFailure() == PluginFailureKind::protocolMismatch ? StartupIssue::pluginWorkerProtocol
        : failure ? StartupIssue::pluginDiscoveryFailure : StartupIssue::none;
    startup.complete(*scanToken, {
        scanState == PluginScanState::cancelled ? StartupState::cancelled
            : timeout ? StartupState::timedOut : failure ? StartupState::warning : StartupState::succeeded,
        scanState == PluginScanState::cancelled ? StartupIssue::cancelled
            : timeout ? StartupIssue::pluginWorkerTimeout
            : issue, catalog.status() }, now());
    scanToken.reset();
}

juce::var StartupShellComponent::report() const
{
    return startupDiagnosticReport(startup, deviceSummary, pluginSummary,
                                   options.safePlugins, options.defaultAudio, firstPaintMs);
}

void StartupShellComponent::persistReport(StartupToken token)
{
    persistInFlight = true;
    lastPersistMs = now();
    persistedRevision = startup.revision();
    const auto snapshot = report();
    const auto store = diagnostics;
    const auto target = mailbox;
    workers.addJob([store, target, snapshot, token]
    {
        Completion completion { token };
        const auto written = store->persist(snapshot);
        completion.outcome = { written.failed() ? StartupState::failed : StartupState::succeeded,
            written.failed() ? StartupIssue::storageUnavailable : StartupIssue::none,
            written.failed() ? written.getErrorMessage() : "Bounded, redacted startup diagnostics saved." };
        post(target, std::move(completion));
    });
}

void StartupShellComponent::exportReport()
{
    if (exportChooser != nullptr)
        return;
    updatePluginSummary();
    if (workspace != nullptr)
        deviceSummary = workspace->startupDeviceSummary();
    exportChooser = std::make_unique<juce::FileChooser>(
        "Export redacted startup diagnostics",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("studio-duo-startup-diagnostics.json"), "*.json");
    exportChooser->launchAsync(juce::FileBrowserComponent::saveMode
        | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<StartupShellComponent>(this)](const juce::FileChooser& chooser)
        {
            const auto destination = chooser.getResult();
            if (safe == nullptr)
                return;
            safe->exportChooser.reset();
            if (destination == juce::File())
                return;
            const auto snapshot = safe->report();
            safe->workers.addJob([safe, snapshot, destination]
            {
                const auto written = writeStartupDiagnostic(destination.withFileExtension("json"), snapshot);
                if (written.failed())
                    logError("startup.export", written.getErrorMessage());
                juce::MessageManager::callAsync([safe, written]
                {
                    if (safe != nullptr)
                        safe->surface.setNotice(written.failed() ? written.getErrorMessage()
                                                               : "Redacted startup diagnostics exported.");
                });
            });
        });
}

void StartupShellComponent::checkSelfTest()
{
    if (!options.selfTest || selfTestFinished)
        return;
    const auto allFinished = std::all_of(startup.results().begin(), startup.results().end(),
        [](const auto& item) { return startupStateIsTerminal(item.state); });
    if (!allFinished && now() - constructedMs < 20000.0)
        return;
    selfTestFinished = true;
    const auto responsive = options.selfTestScenario != "slow-catalog" || responsiveWorkerTicks >= 5;
    const auto observed = [&](StartupTaskId id, StartupIssue issue, StartupState state)
    {
        return std::any_of(startup.history().begin(), startup.history().end(),
            [&](const auto& entry) { return entry.id == id && entry.issue == issue && entry.state == state; });
    };
    const auto& scenario = options.selfTestScenario;
    const auto injectedFailureObserved = scenario.isEmpty()
        || (scenario == "slow-catalog" && responsive)
        || (scenario == "warm-catalog" && pluginSummary.ready == 128)
        || (scenario == "large-catalog" && pluginSummary.ready == 20000)
        || (scenario == "corrupt-catalog" && observed(StartupTaskId::pluginCatalog, StartupIssue::catalogInvalid, StartupState::warning))
        || (scenario == "missing-audio" && observed(StartupTaskId::audio, StartupIssue::audioUnavailable, StartupState::failed))
        || (scenario == "missing-midi" && observed(StartupTaskId::midi, StartupIssue::midiUnavailable, StartupState::warning))
        || (scenario == "recovery" && options.safePlugins
            && observed(StartupTaskId::recovery, StartupIssue::recoveryRequired, StartupState::failed))
        || (scenario == "plugin-timeout" && observed(StartupTaskId::pluginRescan, StartupIssue::pluginWorkerTimeout, StartupState::timedOut));
    const auto recoveryAvailable = usable(startup.result(StartupTaskId::recovery))
        && startup.result(StartupTaskId::recovery)->issue != StartupIssue::storageUnavailable;
    const auto passed = requiredServicesAvailable() && recoveryAvailable && workspace != nullptr
        && !workspace->hasAudioDeviceManager() && surface.hasPainted()
        && firstPaintMs < 2000.0 && now() - constructedMs < 10000.0 && responsive && injectedFailureObserved;
    auto snapshot = report();
    snapshot.getDynamicObject()->setProperty("responsiveWorkerTicks", responsiveWorkerTicks);
    snapshot.getDynamicObject()->setProperty("totalMs", now() - constructedMs);
    snapshot.getDynamicObject()->setProperty("selfTestPassed", passed);
    juce::NullCheckedInvocation::invoke(onSelfTestFinished, passed, snapshot);
}

bool StartupShellComponent::prepareForShutdown()
{
    if (nativeAudioActivationInFlight)
    {
        shutdownAfterActivation = true;
        surface.setNotice("Waiting for native audio activation to finish before closing its handles.");
        return false;
    }
    if (stopped)
        return true;
    if (workspace != nullptr && !workspace->prepareForShutdown())
        return false;
    stopped = true;
    stopTimer();
    mailbox->cancelled->store(true, std::memory_order_release);
    if (audioProbeCancellation != nullptr)
        audioProbeCancellation->store(true, std::memory_order_release);
    if (workspace != nullptr)
        workspace->startupCatalog().cancelScan();
    if (!workers.removeAllJobs(true, 15000))
    {
        logError("startup.shutdown", "Startup background work did not stop before its shutdown deadline.");
        logError("startup.recovery", "Recovery state was retained because background work could not be joined safely.");
        return true;
    }
    if (!requiredServicesAvailable())
        startup.cancel(now());
    else if (scanToken && startup.accepts(*scanToken))
        startup.complete(*scanToken, { StartupState::cancelled, StartupIssue::cancelled,
            "The optional scan was cancelled during clean shutdown." }, now());
    if (const auto written = diagnostics->persist(report()); written.failed())
        logError("startup.diagnostics", written.getErrorMessage());
    if (const auto cleaned = recovery->finishCleanShutdown(); cleaned.failed())
        logError("startup.recovery", cleaned.getErrorMessage());
    return true;
}

void StartupShellComponent::resized()
{
    if (workspace != nullptr)
        workspace->setBounds(getLocalBounds());
    surface.setBounds(getLocalBounds());
}
}

#include "TestHarness.h"
#include "TestSuites.h"
#include "ReleaseFixtures.h"
#include "ScopedTestEnvironment.h"

#include "plugin_host/PluginCompatibilityValidator.h"
#include "plugin_host/ScreamForgeValidation.h"
#include "plugin_host/ClapPluginFormat.h"
#include "plugin_host/ClapPluginInstance.h"
#include "plugin_host/PluginFormats.h"
#include "plugin_host/PluginTransport.h"
#include "audio/StudioAudioEngine.h"
#include "platform/Architecture.h"
#include "plugin_host/PluginStateStore.h"
#include "project_io/ProjectFile.h"
#include "ui/PluginParameterPanel.h"

#include <juce_cryptography/juce_cryptography.h>

#include <iostream>
#include <cmath>

namespace
{
juce::Result sandboxEffectProbe(const juce::PluginDescription& description,
                               const juce::File& directory,
                               std::vector<studio::PluginValidationCheck>& checks)
{
    const auto check = [&](const char* name, bool passed, const juce::String& detail)
    {
        checks.push_back({ name, passed ? "pass" : "fail", detail });
        return passed;
    };
    if (description.isInstrument)
        return juce::Result::fail("The effect-sandbox procedure requires an effect, not an unconfigured instrument.");
    if (directory.exists())
        return juce::Result::fail("The synthetic sandbox workspace already exists.");
    if (const auto created = directory.createDirectory(); created.failed())
        return created;
    const auto exercise = [&]() -> juce::Result
    {
        const auto source = directory.getChildFile("synthetic.wav");
        if (const auto media = release_fixture::writeMedia(source); media.failed())
            return media;
        auto project = studio::Project::createDefault();
        project.metronomeEnabled = false;
        project.loopEnabled = true;
        project.loopEndSeconds = 0.125;
        studio::AudioClip clip;
        clip.sourceFile = source;
        clip.durationSeconds = clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 0.125;
        project.tracks.front().clips.push_back(clip);
        studio::PluginInsert insert;
        insert.pluginIdentifier = description.createIdentifierString();
        insert.name = description.name;
        insert.format = description.pluginFormatName;
        insert.version = description.version;
        insert.bridgeMode = studio::PluginBridgeMode::sandboxed;
        project.tracks.front().inserts.push_back(insert);
        studio::StudioAudioEngine::PluginRuntimeRequest request;
        request.trackId = project.tracks.front().id;
        request.insertId = insert.id;
        request.name = insert.name;
        request.description = description;
        request.bridgeMode = studio::PluginBridgeMode::sandboxed;
        auto engine = std::make_unique<studio::StudioAudioEngine>(juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH));
        if (const auto configured = engine->configureAudioForTesting(48000.0, 256); configured.failed())
            return configured;
        if (const auto started = engine->updateProject(project, { request }); started.failed())
            return started;
        using State = studio::StudioAudioEngine::PluginRuntimeStatus::State;
        const auto waitFor = [&](State expected, int timeout)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeout;
            while (juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                const auto statuses = engine->pluginRuntimeStatuses();
                const auto status = std::find_if(statuses.begin(), statuses.end(),
                    [&](const auto& value) { return value.insertId == insert.id; });
                if (status != statuses.end() && status->state == expected
                    && (expected != State::ready || !engine->pluginRuntimeTransitionPending()))
                    return true;
                if (expected == State::ready && status != statuses.end()
                    && (status->state == State::failed || status->state == State::missing)
                    && !engine->pluginRuntimeTransitionPending())
                    return false;
                juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            }
            return false;
        };
        if (!check("sandbox-ready", waitFor(State::ready, 15000), "The exact selected effect must become ready in its worker."))
            return juce::Result::fail("The selected vendor worker did not become ready.");
        const auto saved = engine->capturePluginStates(2000);
        const auto capture = std::find_if(saved.begin(), saved.end(),
            [&](const auto& value) { return value.insertId == insert.id; });
        if (!check("sandbox-state-capture", capture != saved.end() && capture->result.wasOk()
                && !capture->state.isEmpty(), "A ready worker supplies a nonempty saved state."))
            return juce::Result::fail("The vendor did not supply a usable sandbox state.");
        request.state = capture->state;
        juce::String error;
        const auto package = directory.getChildFile("synthetic.studioduo");
        auto reference = studio::PluginStateStore::store(package, capture->state, error);
        if (!reference)
            return juce::Result::fail(error);
        auto& stored = project.tracks.front().inserts.front();
        stored.stateFile = reference->relativePath;
        stored.stateHash = reference->hash;
        stored.stateFormat = description.pluginFormatName == "CLAP"
            ? studio::PluginStateFormat::clapPreset : studio::PluginStateFormat::hostOpaque;
        if (const auto persisted = studio::ProjectFile::save(project, package); persisted.failed())
            return persisted;
        auto before = engine->pluginRuntimeStatuses();
        engine->play();
        juce::AudioBuffer<float> audio(2, 256);
        auto peak = 0.0f;
        auto finite = true;
        auto steadyLateBlocks = std::uint64_t { 0 };
        for (int block = 0; block < 64; ++block)
        {
            if (block == 16)
                steadyLateBlocks = engine->pluginLateBlockCount();
            engine->renderActiveBlockForTesting(audio);
            peak = std::max(peak, audio.getMagnitude(0, audio.getNumSamples()));
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                    finite = finite && std::isfinite(audio.getSample(channel, sample));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        }
        const auto stillReady = waitFor(State::ready, 1000);
        const auto lateAfterPlayback = engine->pluginLateBlockCount();
        if (!check("sandbox-playback", finite && peak > 0.001f
                && stillReady && lateAfterPlayback == steadyLateBlocks,
                "Synthetic exchange: finite " + juce::String(finite ? 1 : 0)
                    + ", peak " + juce::String(peak, 6) + ", ready " + juce::String(stillReady ? 1 : 0)
                    + ", late blocks " + juce::String(steadyLateBlocks) + " -> " + juce::String(lateAfterPlayback)
                    + ". 30 ms per block is not deadline qualification."))
            return juce::Result::fail("The default effect state did not produce a stable observable response.");
        before = engine->pluginRuntimeStatuses();
        if (before.size() != 1)
            return juce::Result::fail("The selected effect has no unambiguous parameter snapshot.");
        const auto continuous = std::find_if(before.front().parameters.begin(), before.front().parameters.end(),
            [](const auto& parameter) { return parameter.automatable && parameter.steps == 0; });
        if (continuous == before.front().parameters.end())
            return juce::Result::fail("This procedure requires a reported continuous control to prove non-default state restoration.");
        const auto editedParameter = *continuous;
        const auto selectedPosition = static_cast<int>(std::distance(before.front().parameters.begin(), continuous));
        const auto targetValue = editedParameter.value < 0.5f ? 0.75f : 0.25f;
        studio::PluginParameterPanel genericEditor(description.name, insert.id, before.front().parameters);
        auto starts = 0, changes = 0, ends = 0;
        auto editAccepted = false;
        genericEditor.onGestureStarted = [&](const auto&, const auto&, const auto&, int, float) { ++starts; };
        genericEditor.onGestureEnded = [&](const auto&, const auto&, const auto&, int, float) { ++ends; };
        genericEditor.onValueChanged = [&](const auto& id, const auto& parameterId, const auto& parameterName, int index, float value)
        {
            ++changes;
            editAccepted = id == insert.id && parameterId == editedParameter.id
                && parameterName == editedParameter.name && index == editedParameter.index
                && engine->setPluginParameter(id, index, value, error);
        };
        juce::ComboBox* parameterSelector = nullptr;
        juce::Slider* parameterValue = nullptr;
        for (auto* child : genericEditor.getChildren())
        {
            if (auto* selector = dynamic_cast<juce::ComboBox*>(child))
                parameterSelector = selector;
            if (auto* slider = dynamic_cast<juce::Slider*>(child))
                parameterValue = slider;
        }
        if (parameterSelector != nullptr && parameterValue != nullptr)
        {
            parameterSelector->setSelectedId(selectedPosition + 1, juce::sendNotificationSync);
            parameterValue->setValue(targetValue, juce::sendNotificationSync);
        }
        if (!check("sandbox-generic-editor", editAccepted && starts == 1 && changes == 1 && ends == 1,
                "The real generic native editor applies a named non-drag edit with one complete host gesture."))
            return juce::Result::fail("The generic editor could not apply the selected vendor control: " + error);
        const auto metadataDeadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        auto valueObserved = false;
        while (juce::Time::getMillisecondCounterHiRes() < metadataDeadline)
        {
            engine->renderActiveBlockForTesting(audio);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
            before = engine->pluginRuntimeStatuses();
            if (!before.empty())
            {
                const auto parameter = std::find_if(before.front().parameters.begin(), before.front().parameters.end(),
                    [&](const auto& value) { return value.id == editedParameter.id; });
                valueObserved = parameter != before.front().parameters.end()
                    && std::abs(parameter->value - targetValue) < 1e-5f;
            }
            if (valueObserved)
                break;
        }
        if (!check("sandbox-parameter-feedback", valueObserved,
                "A non-default continuous edit is observed in worker metadata, not just an optimistic host value."))
            return juce::Result::fail("The worker did not report the changed control value.");
        const auto editedStates = engine->capturePluginStates(2000);
        const auto editedState = std::find_if(editedStates.begin(), editedStates.end(),
            [&](const auto& value) { return value.insertId == insert.id; });
        if (!check("sandbox-nondefault-state", editedState != editedStates.end() && editedState->result.wasOk()
                && !editedState->state.isEmpty() && editedState->state != capture->state,
                "The edited vendor state differs from initial state before being stored and reloaded."))
            return juce::Result::fail("The non-default control did not produce a distinct usable state blob.");
        request.state = editedState->state;
        reference = studio::PluginStateStore::store(package, request.state, error);
        if (!reference)
            return juce::Result::fail(error);
        stored.stateFile = reference->relativePath;
        stored.stateHash = reference->hash;
        if (const auto persisted = studio::ProjectFile::save(project, package); persisted.failed())
            return persisted;
        before = engine->pluginRuntimeStatuses();
        if (!engine->simulatePluginCrashForTesting(insert.id)
            || !check("sandbox-crash-visible", waitFor(State::failed, 3000),
                "Terminating only this ready worker produces a visible failure without terminating its parent."))
            return juce::Result::fail("The selected worker could not be terminated or its failure was not observed.");
        auto fallbackPeak = 0.0f;
        for (int block = 0; block < 16; ++block)
        {
            engine->renderActiveBlockForTesting(audio);
            fallbackPeak = std::max(fallbackPeak, audio.getMagnitude(0, audio.getNumSamples()));
        }
        if (!check("sandbox-dry-fallback", fallbackPeak > 0.001f && engine->isPlaying(),
                "The synthetic loop keeps producing dry fallback after only its worker exits."))
            return juce::Result::fail("Worker termination interrupted the parent transport or dry fallback.");
        const auto failedStates = engine->capturePluginStates(2000);
        const auto failed = std::find_if(failedStates.begin(), failedStates.end(),
            [&](const auto& value) { return value.insertId == insert.id; });
        if (!check("sandbox-failed-state-preserved", failed != failedStates.end()
                && failed->result.failed() && failed->preservePreviousState,
                "State capture after disconnect explicitly preserves the previous valid blob."))
            return juce::Result::fail("Disconnected state capture did not preserve the previous saved state.");
        if (const auto persisted = studio::ProjectFile::save(project, package); persisted.failed())
            return persisted;
        const auto reopened = studio::ProjectFile::load(package, error);
        if (!check("sandbox-package-valid", reopened
                && reopened->tracks.front().inserts.front().stateHash == reference->hash,
                "The real native package reopens with its last verified state hash after the worker exits."))
            return juce::Result::fail("The native package did not preserve the saved vendor state.");
        engine->stop();
        if (const auto reloaded = engine->forcePluginRuntimeReload(project, { request }, insert.id); reloaded.failed())
            return reloaded;
        if (!check("sandbox-explicit-reload", waitFor(State::ready, 15000), "Explicit reload restores the saved vendor instance."))
            return juce::Result::fail("The vendor did not become ready after explicit saved-state reload.");
        engine->play();
        for (int block = 0; block < 8; ++block)
        {
            engine->renderActiveBlockForTesting(audio);
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                    finite = finite && std::isfinite(audio.getSample(channel, sample));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        }
        engine->stop();
        if (!check("sandbox-restored-processing", finite && waitFor(State::ready, 1000),
                "The explicitly restored non-default state remains finite and ready when processing resumes."))
            return juce::Result::fail("The restored non-default state did not resume healthy processing.");
        const auto after = engine->pluginRuntimeStatuses();
        auto parametersRestored = before.size() == after.size() && !before.empty();
        juce::String differences;
        if (parametersRestored)
            for (const auto& parameter : before.front().parameters)
            {
                if (!parameter.automatable)
                    continue;
                const auto restored = std::find_if(after.front().parameters.begin(), after.front().parameters.end(),
                    [&](const auto& value) { return value.id == parameter.id; });
                const auto matches = restored != after.front().parameters.end()
                    && std::abs(restored->value - parameter.value) < 1e-5f;
                if (!matches && differences.length() < 1000)
                    differences << " " << parameter.id << ":" << juce::String(parameter.value, 6)
                                << "->" << (restored == after.front().parameters.end()
                                    ? juce::String("missing") : juce::String(restored->value, 6));
                parametersRestored = parametersRestored && matches;
            }
        return check("sandbox-restored-parameters", parametersRestored,
            "Saved automatable parameter IDs/values survive worker termination and explicit reload." + differences)
            ? juce::Result::ok() : juce::Result::fail("Reloaded vendor parameters differ from the saved state.");
    };
    const auto result = exercise();
    if (!directory.deleteRecursively())
        return juce::Result::fail("Could not remove the owned synthetic sandbox workload.");
    return result;
}
}

std::optional<int> runVendorPluginQualification(const juce::StringArray& arguments)
{
    const auto requested = std::any_of(arguments.begin(), arguments.end(),
        [](const auto& argument) { return argument.startsWith("--qualify-plugin"); });
    if (!requested)
        return std::nullopt;
    juce::String source, outputPath;
    bool sourceSeen = false, outputSeen = false;
    bool omitPlatformTools = false;
    bool exerciseSandboxEffect = false;
    for (const auto& argument : arguments)
    {
        if (argument.startsWith("--qualify-plugin=") && !sourceSeen)
        {
            sourceSeen = true;
            source = argument.fromFirstOccurrenceOf("=", false, false);
        }
        else if (argument.startsWith("--qualification-output=") && !outputSeen)
        {
            outputSeen = true;
            outputPath = argument.fromFirstOccurrenceOf("=", false, false);
        }
        else if (argument == "--omit-platform-tools" && !omitPlatformTools)
            omitPlatformTools = true;
        else if (argument == "--exercise-sandbox-effect" && !exerciseSandboxEffect)
            exerciseSandboxEffect = true;
        else
        {
            std::cerr << "Unknown or duplicate vendor qualification argument.\n";
            return 2;
        }
    }
    const auto nativeAudioUnit = source.startsWith("AudioUnit:");
    if (source.isEmpty() || !juce::File::isAbsolutePath(outputPath)
        || (!nativeAudioUnit && !juce::File::isAbsolutePath(source)))
    {
        std::cerr << "Use an absolute VST3/CLAP path or registered AudioUnit identifier and a new absolute output directory.\n";
        return 2;
    }
    const juce::File output(outputPath);
    juce::InterProcessLock reservation("StudioDuoQualification-"
        + juce::SHA256(output.getFullPathName().toLowerCase().toUTF8()).toHexString());
    if (!reservation.enter(0) || output.exists())
    {
        std::cerr << "Vendor qualification output is already owned or exists; evidence is never overwritten.\n";
        return 2;
    }
    if (const auto result = output.createDirectory(); result.failed())
    {
        std::cerr << result.getErrorMessage() << '\n';
        return 1;
    }
    juce::DynamicObject::Ptr report = new juce::DynamicObject();
    report->setProperty("schemaVersion", 1);
    report->setProperty("scope", exerciseSandboxEffect
        ? "vendor-headless-sandbox-effect-probe" : "vendor-headless-public-standard-probe");
    report->setProperty("qualificationStatus", "not-qualified");
    report->setProperty("candidateSha", STUDIO_DUO_BUILD_COMMIT);
    report->setProperty("buildRevision", STUDIO_DUO_BUILD_REVISION);
    report->setProperty("architecture", studio::currentArchitecture());
    report->setProperty("os", juce::SystemStats::getOperatingSystemName());
    report->setProperty("hostAudioDeviceManagerCreated", false);
    report->setProperty("supplementalTools", omitPlatformTools ? "explicitly-omitted" : "run");
    report->setProperty("effectSandboxRequested", exerciseSandboxEffect);
    report->setProperty("source", nativeAudioUnit ? source : juce::File(source).getFileName());
    juce::Array<juce::var> processors;
    const auto write = [&](const juce::String& status, const juce::String& detail)
    {
        report->setProperty("status", status);
        report->setProperty("detail", detail);
        report->setProperty("processors", processors);
        if (status == "blocked" || status == "fail")
            std::cerr << detail << '\n';
        auto json = juce::JSON::toString(juce::var(report.get()), true);
        if (!nativeAudioUnit)
            json = json.replace(source, "<vendor-plugin>");
        json = json.replace(output.getFullPathName(), "<qualification-output>");
        json = json.replace(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName(), "<home>");
        if (!output.getChildFile("report.json").replaceWithText(json))
        {
            std::cerr << "Cannot retain the vendor probe report.\n";
            return false;
        }
        return true;
    };
    if (!write("running", "A subprocess timeout/crash must remain incomplete, never a qualification pass."))
        return 1;
    if (!nativeAudioUnit && !juce::File(source).exists())
        return write("blocked", "The selected vendor plugin is not installed at the explicit source path.") ? 2 : 1;
    const auto formatName = nativeAudioUnit ? juce::String("AudioUnit")
        : juce::File(source).hasFileExtension("clap") ? juce::String("CLAP")
        : juce::File(source).hasFileExtension("vst3") ? juce::String("VST3") : juce::String();
    if (formatName.isEmpty())
        return write("blocked", "Only explicit VST3/CLAP files and registered AudioUnit identifiers are supported.") ? 2 : 1;

    juce::ScopedJuceInitialiser_GUI initialiser;
    juce::AudioPluginFormatManager formats;
    studio::PluginFormats::addSupportedFormats(formats);
    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (auto* format : formats.getFormats())
        if (format->getName() == formatName)
            format->findAllTypesForFile(descriptions, source);
    if (descriptions.isEmpty())
        return write("blocked", "The requested native format could not discover a processor; no alternative format was substituted.") ? 2 : 1;
    auto passed = true;
    for (const auto* description : descriptions)
    {
        auto validation = studio::PluginCompatibilityValidator::validate(*description,
            omitPlatformTools
                ? studio::PluginCompatibilityValidator::SupplementalTools::omitForIsolatedProbe
                : studio::PluginCompatibilityValidator::SupplementalTools::run);
        if (exerciseSandboxEffect)
        {
            const auto backend = sandboxEffectProbe(*description,
                output.getChildFile("sandbox-workload"), validation.checks);
            if (backend.failed())
            {
                validation.status = "fail";
                validation.checks.push_back({ "sandbox-procedure", "fail", backend.getErrorMessage() });
            }
        }
        auto value = validation.toVar();
        value.getDynamicObject()->setProperty("manufacturer", description->manufacturerName);
        value.getDynamicObject()->setProperty("role", description->isInstrument ? "instrument" : "effect");
        value.getDynamicObject()->setProperty("architecture", studio::currentArchitecture());
        processors.add(std::move(value));
        passed = passed && validation.status == "pass";
        if (!write("running", "Per-check results are retained; unsupported checks remain explicit skips."))
            return 1;
    }
    if (!write(passed ? "pass" : "fail",
            "Only the explicit standard/backend checks are measured. Full vendor/mode workflows, hang-during-capture, routing, deterministic nulls, and release approval are not certified."))
        return 1;
    std::cout << juce::JSON::toString(juce::JSON::parse(output.getChildFile("report.json")), true) << '\n';
    return passed ? 0 : 1;
}

void pluginCompatibilityTests()
{
    studio::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions,
                               STUDIO_DUO_CLAP_FIXTURE_PATH);
    expect(descriptions.size() == 1,
           "Compatibility fixture can be discovered.");
    if (descriptions.isEmpty())
        return;
    {
        ScopedTestEnvironment transportMode("STUDIO_DUO_TEST_CLAP_TRANSPORT", "1");
        expect(transportMode.status().wasOk(), "The synthetic transport fixture has a process-local configuration.");
        studio::PluginHostPlayHead playHead;
        studio::PluginTransport clock;
        clock.samples = 12000;
        clock.seconds = 0.25;
        clock.bpm = 150.0;
        clock.ppq = 1.5;
        clock.numerator = 7;
        clock.denominator = 8;
        clock.loopEndPpq = 4.0;
        clock.loopEndSeconds = 1.6;
        clock.flags = studio::PluginTransport::valid | studio::PluginTransport::playing
            | studio::PluginTransport::recording | studio::PluginTransport::looping;
        playHead.setTransport(clock);
        juce::String transportError;
        auto processor = studio::ClapPluginInstance::create(*descriptions[0], 48000.0, 256, transportError);
        expect(processor != nullptr, transportError.toRawUTF8());
        if (processor != nullptr)
        {
            processor->setPlayHead(&playHead);
            processor->prepareToPlay(48000.0, 256);
            juce::AudioBuffer<float> signal(2, 256);
            signal.clear();
            juce::MidiBuffer midi;
            processor->processBlock(signal, midi);
            expect(std::abs(signal.getSample(0, 0) - 0.15f) < 1e-6f
                       && std::abs(signal.getSample(0, 1) - 7.0f / 64.0f) < 1e-6f
                       && std::abs(signal.getSample(0, 2) - 8.0f / 64.0f) < 1e-6f
                       && std::abs(signal.getSample(0, 3) - 1.0f) < 1e-6f
                       && std::abs(signal.getSample(0, 4) * 1024.0f - 1.5f) < 1e-6f
                       && std::abs(signal.getSample(0, 5) * 10.0f - 0.25f) < 1e-6f,
                   "CLAP process transport contains real host tempo, meter, state, PPQ and seconds rather than nullptr.");
            processor->releaseResources();
        }
    }
    ScopedTestEnvironment activationEnvironment("STUDIO_DUO_TEST_CLAP_ACTIVATION_VALUES", "1");
    expect(activationEnvironment.status().wasOk(), "The synthetic activation-value case can configure its child-only test environment.");
    juce::String activationError;
    auto activated = studio::ClapPluginInstance::create(*descriptions[0], 48000.0, 256, activationError);
    auto pending = studio::ClapPluginInstance::create(*descriptions[0], 48000.0, 256, activationError);
    if (activated != nullptr)
        activated->prepareToPlay(48000.0, 256);
    if (pending != nullptr && !pending->getParameters().isEmpty())
    {
        pending->getParameters()[0]->setValue(0.9f);
        pending->prepareToPlay(48000.0, 256);
    }
    expect(activationEnvironment.restore().wasOk(),
           "The activation case restores the previous process environment.");
    expect(activated != nullptr && !activated->getParameters().isEmpty()
               && std::abs(activated->getParameters()[0]->getValue() - 0.75f) < 1e-6f,
           "CLAP values initialized during activation are refreshed before native/generic editors and worker-ready metadata read them.");
    expect(pending != nullptr && !pending->getParameters().isEmpty()
               && std::abs(pending->getParameters()[0]->getValue() - 0.9f) < 1e-6f,
           "Activation refresh must preserve a host edit that has not yet been delivered.");
    activated.reset();
    pending.reset();

    const auto report = studio::PluginCompatibilityValidator::validate(
        *descriptions[0]);
    auto failedChecks = juce::String();
    for (const auto& check : report.checks)
        if (check.status == "fail")
            failedChecks << check.name << ": " << check.message << "; ";
    expect(report.status == "pass"
               && std::all_of(
                   report.checks.cbegin(),
                   report.checks.cend(),
                   [](const auto& check)
                   {
                       return check.status == "pass"
                           || check.status == "skip";
                   }),
           ("Public-standard compatibility validation passes the CLAP fixture"
            + (failedChecks.isNotEmpty()
                   ? " (" + failedChecks + ")"
                   : juce::String())
            + ".")
               .toRawUTF8());
    juce::String error;
    const auto restored =
        studio::PluginValidationReport::fromVar(report.toVar(), error);
    expect(restored.has_value()
               && restored->pluginIdentifier == report.pluginIdentifier,
           "Compatibility reports are stable and serializable.");
    const auto output = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("StudioDuoVendorProbe-", "", false);
    const juce::StringArray probeArguments {
        "--qualify-plugin=" + juce::String(STUDIO_DUO_CLAP_FIXTURE_PATH),
        "--qualification-output=" + output.getFullPathName(),
        "--omit-platform-tools",
        "--exercise-sandbox-effect"
    };
    expect(runVendorPluginQualification(probeArguments) == std::optional<int>(0),
           "The explicit-source vendor probe can run without touching a persistent catalog or audio device manager.");
    const auto probe = juce::JSON::parse(output.getChildFile("report.json"));
    expect(probe["scope"].toString() == "vendor-headless-sandbox-effect-probe"
               && probe["qualificationStatus"].toString() == "not-qualified"
               && probe["supplementalTools"].toString() == "explicitly-omitted"
               && probe["processors"].isArray() && probe["processors"].getArray()->size() == 1,
           "A supplemental standard probe is never serialized as full vendor qualification.");
    expect(runVendorPluginQualification(probeArguments) == std::optional<int>(2),
           "A vendor probe cannot overwrite existing candidate evidence.");
    expect(runVendorPluginQualification({ "--qualify-plugin" }) == std::optional<int>(2),
           "A malformed vendor command fails instead of falling through to the complete native test suite.");
    expect(output.deleteRecursively(), "The synthetic vendor probe removes only its owned test output.");

    juce::AudioPluginFormatManager araFormats;
    studio::PluginFormats::addSupportedFormats(araFormats);
    juce::OwnedArray<juce::PluginDescription> araDescriptions;
    for (auto* pluginFormat : araFormats.getFormats())
    {
        if (pluginFormat->getName() == "VST3")
        {
            pluginFormat->findAllTypesForFile(
                araDescriptions,
                STUDIO_DUO_ARA_FIXTURE_PATH);
            break;
        }
    }
    expect(araDescriptions.size() == 1
               && araDescriptions[0]->hasARAExtension,
           "The public ARA fixture advertises ARA 2.");
    if (!araDescriptions.isEmpty())
    {
        const auto araReport =
            studio::PluginCompatibilityValidator::validate(
                *araDescriptions[0]);
        const auto passed = [&araReport](const juce::String& name)
        {
            const auto check = std::find_if(
                araReport.checks.cbegin(),
                araReport.checks.cend(),
                [&name](const auto& candidate)
                {
                    return candidate.name == name;
                });
            return check != araReport.checks.cend()
                && check->status == "pass";
        };
        expect(araReport.status == "pass"
                   && passed("ara2")
                   && passed("ara-audio-source")
                   && passed("ara-archive"),
               "ARA validation covers document binding, audio access, and archive restore.");

        const auto sourceFile = juce::File::getSpecialLocation(
                                    juce::File::tempDirectory)
                                    .getNonexistentChildFile(
                                        "StudioDuoAraLiveState",
                                        ".wav",
                                        false);
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream =
                sourceFile.createOutputStream();
            auto writer = wav.createWriterFor(
                stream,
                juce::AudioFormatWriterOptions {}
                    .withSampleRate(48000.0)
                    .withNumChannels(2)
                    .withBitsPerSample(24));
            juce::AudioBuffer<float> source(2, 480);
            source.clear();
            expect(writer != nullptr
                       && writer->writeFromAudioSampleBuffer(
                           source,
                           0,
                           source.getNumSamples()),
                   "ARA live-state source can be written.");
        }
        auto araProject = studio::Project::createDefault();
        studio::AudioClip clip;
        clip.sourceFile = sourceFile;
        clip.durationSeconds = 0.01;
        clip.sourceLengthSeconds = 0.01;
        clip.sourceRangeEndSeconds = 0.01;
        araProject.tracks.front().clips.push_back(clip);
        studio::PluginInsert araInsert;
        araInsert.id = juce::Uuid().toString();
        araInsert.name = "ARA Fixture";
        araInsert.pluginIdentifier =
            araDescriptions[0]->createIdentifierString();
        araInsert.format = "VST3";
        araInsert.bridgeMode =
            studio::PluginBridgeMode::araCompatibility;
        araInsert.araCapable = true;
        araProject.tracks.front().inserts.push_back(araInsert);

        studio::StudioAudioEngine araEngine;
        studio::StudioAudioEngine::PluginRuntimeRequest araRequest;
        araRequest.trackId = araProject.tracks.front().id;
        araRequest.insertId = araInsert.id;
        araRequest.name = araInsert.name;
        araRequest.description = *araDescriptions[0];
        araRequest.bridgeMode =
            studio::PluginBridgeMode::araCompatibility;
        araRequest.araDocument =
            studio::AraDocumentHost::describeProject(
                araProject,
                araRequest.trackId);
        expect(araEngine.updateProject(
                   araProject,
                   { araRequest })
                   .wasOk(),
               "ARA fixture runtime accepts project audio.");
        const auto waitForAraRuntime = [&araEngine]
        {
            for (int attempt = 0;
                 attempt < 500
                 && araEngine.pluginRuntimeTransitionPending();
                 ++attempt)
            {
                if (auto* messages =
                        juce::MessageManager::getInstanceWithoutCreating())
                {
                    messages->runDispatchLoopUntil(10);
                }
                else
                {
                    juce::Thread::sleep(10);
                }
            }
        };
        waitForAraRuntime();
        juce::String parameterError;
        expect(araEngine.setPluginParameter(
                   araInsert.id,
                   0,
                   0.25f,
                   parameterError),
               parameterError.toRawUTF8());

        araProject.tracks.front().clips.front().startSeconds = 0.25;
        araRequest.araDocument =
            studio::AraDocumentHost::describeProject(
                araProject,
                araRequest.trackId);
        expect(araEngine.updateProject(
                   araProject,
                   { araRequest })
                   .wasOk(),
               "ARA graph changes schedule a state-preserving rebuild.");
        waitForAraRuntime();
        const auto rebuiltStatuses = araEngine.pluginRuntimeStatuses();
        const auto preservedParameter = !rebuiltStatuses.empty()
            && !rebuiltStatuses.front().parameters.empty()
            ? rebuiltStatuses.front().parameters.front().value
            : -1.0f;
        const auto liveCaptures = araEngine.capturePluginStates(5000);
        juce::MemoryBlock processorState;
        juce::MemoryBlock araState;
        const auto unpacked = !liveCaptures.empty()
            && studio::AraDocumentHost::unpackState(
                liveCaptures.front().state,
                processorState,
                araState);
        expect(!liveCaptures.empty()
                   && liveCaptures.front().result.wasOk()
                   && std::abs(preservedParameter - 0.25f) < 0.0001f
                   && unpacked
                   && !processorState.isEmpty()
                   && !araState.isEmpty(),
               "ARA graph rebuilds preserve unsaved processor and document state.");
        if (!liveCaptures.empty()
            && liveCaptures.front().result.wasOk())
        {
            araRequest.state = liveCaptures.front().state;
        }
        juce::AudioBuffer<float> araRender;
        expect(araEngine.renderToBuffer(
                   araProject,
                   araRender,
                   48000.0,
                   { araRequest })
                   .wasOk(),
               "ARA processors render while message-thread creation remains responsive.");
        araEngine.shutdown();
        sourceFile.deleteFile();
    }

    juce::PluginDescription screamForge;
    screamForge.name = "Scream Forge";
    screamForge.manufacturerName = "Studio Duo";
    screamForge.pluginFormatName = "VST3";
    expect(studio::ScreamForgeValidation::matches(screamForge),
           "Scream Forge selection uses public plugin metadata.");
    const auto unavailable =
        studio::ScreamForgeValidation::validateInstalled({});
    expect(unavailable.status == "not-installed",
           "Absent Scream Forge builds report not-installed explicitly.");
}

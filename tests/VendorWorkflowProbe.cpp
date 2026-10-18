#include "VendorWorkflowProbe.h"

#include "audio/StudioAudioEngine.h"
#include "plugin_host/ClapPluginInstance.h"
#include "plugin_host/PluginFormats.h"
#include "plugin_host/ValidatedPluginStateTarget.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace
{
constexpr double rate = 48000.0;
constexpr int quantum = 512;
constexpr int contentSamples = 24000;

bool finite(const juce::AudioBuffer<float>& audio)
{
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            if (!std::isfinite(audio.getSample(channel, sample)))
                return false;
    return true;
}

float difference(const juce::AudioBuffer<float>& first, const juce::AudioBuffer<float>& second)
{
    if (first.getNumChannels() != second.getNumChannels() || first.getNumSamples() != second.getNumSamples())
        return std::numeric_limits<float>::infinity();
    auto maximum = 0.0f;
    for (int channel = 0; channel < first.getNumChannels(); ++channel)
        for (int sample = 0; sample < first.getNumSamples(); ++sample)
            maximum = std::max(maximum, std::abs(first.getSample(channel, sample) - second.getSample(channel, sample)));
    return maximum;
}

juce::var differenceSpans(const juce::AudioBuffer<float>& first, const juce::AudioBuffer<float>& second)
{
    auto result = std::make_unique<juce::DynamicObject>();
    auto beginning = -1, end = -1;
    juce::Array<juce::var> blocks;
    for (int start = 0; start < first.getNumSamples(); start += quantum)
    {
        auto maximum = 0.0f;
        for (int sample = start; sample < std::min(first.getNumSamples(), start + quantum); ++sample)
            for (int channel = 0; channel < first.getNumChannels(); ++channel)
            {
                const auto delta = std::abs(first.getSample(channel, sample) - second.getSample(channel, sample));
                maximum = std::max(maximum, delta);
                if (delta > 1e-5f)
                {
                    if (beginning < 0) beginning = sample;
                    end = sample;
                }
            }
        blocks.add(static_cast<double>(maximum));
    }
    result->setProperty("first", beginning);
    result->setProperty("last", end);
    result->setProperty("blocks", blocks);
    return juce::var(result.release());
}

std::unique_ptr<juce::AudioPluginInstance> create(const juce::PluginDescription& description, juce::String& error)
{
    if (description.pluginFormatName == "CLAP")
        return studio::ClapPluginInstance::create(description, rate, quantum, error);
    juce::AudioPluginFormatManager formats;
    studio::PluginFormats::addSupportedFormats(formats);
    return formats.createPluginInstance(description, rate, quantum, error);
}

juce::Result writeSource(const juce::File& file)
{
    juce::AudioBuffer<float> audio(2, contentSamples);
    for (int sample = 0; sample < contentSamples; ++sample)
    {
        const auto phase = juce::MathConstants<double>::twoPi * 233.0 * sample / rate;
        audio.setSample(0, sample, static_cast<float>(0.05 * std::sin(phase)));
        audio.setSample(1, sample, static_cast<float>(0.04 * std::sin(phase * 1.37)));
    }
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions()
        .withSampleRate(rate).withNumChannels(2).withBitsPerSample(24));
    if (writer == nullptr || !writer->writeFromAudioSampleBuffer(audio, 0, contentSamples))
        return juce::Result::fail("Cannot create the synthetic vendor workflow source.");
    writer->flush();
    return juce::Result::ok();
}
}

juce::Result exerciseVendorWorkflows(
    const juce::PluginDescription& description, const juce::File& directory,
    std::vector<studio::PluginValidationCheck>& checks, juce::var& measurements, const juce::var& profile)
{
    if (directory.exists())
        return juce::Result::fail("The owned vendor workflow directory already exists.");
    if (const auto created = directory.createDirectory(); created.failed())
        return created;
    auto metrics = std::make_unique<juce::DynamicObject>();
    metrics->setProperty("sampleRate", rate);
    metrics->setProperty("bufferSize", quantum);
    metrics->setProperty("contentSamples", contentSamples);
    metrics->setProperty("livePacingMilliseconds", 25);
    metrics->setProperty("nullTolerance", 1e-5);
    metrics->setProperty("transportGranularity", "processing-quantum-start");
    const auto check = [&](const char* name, bool passed, juce::String detail)
    {
        checks.push_back({ name, passed ? "pass" : "fail", std::move(detail) });
        return passed;
    };
    const auto run = [&]() -> juce::Result
    {
        juce::String error;
        auto first = create(description, error);
        auto second = create(description, error);
        if (!first || !second)
            return juce::Result::fail("Cannot create two independent vendor instances: " + error);
        first->prepareToPlay(rate, quantum);
        second->prepareToPlay(rate, quantum);
        struct Release
        {
            ~Release() { first.releaseResources(); second.releaseResources(); }
            juce::AudioProcessor& first;
            juce::AudioProcessor& second;
        } release { *first, *second };
        metrics->setProperty("mainInputChannels", first->getMainBusNumInputChannels());
        metrics->setProperty("mainOutputChannels", first->getMainBusNumOutputChannels());
        metrics->setProperty("inputBuses", first->getBusCount(true));
        metrics->setProperty("outputBuses", first->getBusCount(false));
        const auto& parameters = first->getParameters();
        const auto idFor = [](const juce::AudioProcessorParameter* parameter, int index)
        {
            const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter);
            return hosted != nullptr ? hosted->getParameterID() : juce::String(index);
        };
        if (!profile.isVoid())
        {
            const auto* object = profile.getDynamicObject();
            if (object == nullptr || !profile["schemaVersion"].isInt() || static_cast<int>(profile["schemaVersion"]) != 1
                || profile["name"].toString() != description.name
                || profile["manufacturer"].toString() != description.manufacturerName
                || profile["version"].toString() != description.version || !profile["parameters"].isArray())
                return juce::Result::fail("The explicit workflow profile does not match this exact vendor/version.");
            if (profile["format"].toString() != description.pluginFormatName)
                return juce::Result::fail("The workflow profile's parameter-ID namespace does not match this native format.");
            std::set<juce::String> seen;
            std::vector<std::pair<int, float>> overrides;
            for (const auto& value : *profile["parameters"].getArray())
            {
                const auto id = value["id"].toString();
                const auto number = value["value"];
                const auto normalised = static_cast<double>(number);
                if (id.isEmpty() || !seen.insert(id).second || (!number.isDouble() && !number.isInt())
                    || !std::isfinite(normalised) || normalised < 0.0 || normalised > 1.0)
                    return juce::Result::fail("Workflow profile controls must be unique, finite, normalized values.");
                auto found = -1;
                for (int parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex)
                    if (idFor(parameters[parameterIndex], parameterIndex) == id)
                        found = parameterIndex;
                if (found < 0 || parameters[found]->getName(128) != value["name"].toString()
                    || found >= second->getParameters().size()
                    || idFor(second->getParameters()[found], found) != id)
                    return juce::Result::fail("Workflow profile control " + id + " expects "
                        + value["name"].toString() + "; observed "
                        + (found >= 0 ? parameters[found]->getName(128) : juce::String("no matching public ID")) + ".");
                overrides.emplace_back(found, static_cast<float>(normalised));
            }
            for (const auto& [parameterIndex, value] : overrides)
            {
                parameters[parameterIndex]->setValueNotifyingHost(value);
                second->getParameters()[parameterIndex]->setValueNotifyingHost(value);
            }
            for (auto* processor : { first.get(), second.get() })
            {
                juce::AudioBuffer<float> silence(std::max({ 2, processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels() }), quantum);
                silence.clear();
                juce::MidiBuffer events;
                processor->processBlock(silence, events);
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
            for (const auto& [parameterIndex, value] : overrides)
                if (std::abs(parameters[parameterIndex]->getValue() - value) > 1e-5f
                    || std::abs(second->getParameters()[parameterIndex]->getValue() - value) > 1e-5f)
                    return juce::Result::fail("A workflow profile control did not retain its requested value after processing.");
            metrics->setProperty("explicitProfile", profile);
        }
        juce::MemoryBlock initialState;
        first->getStateInformation(initialState);
        if (initialState.isEmpty())
            return juce::Result::fail("Workflow qualification requires nonempty repeatable initial state.");
        juce::Array<juce::var> inventory;
        juce::Array<juce::var> determinismControls;
        for (int parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex)
        {
            const auto* parameter = parameters[parameterIndex];
            const auto name = parameter->getName(128);
            auto control = std::make_unique<juce::DynamicObject>();
            control->setProperty("index", parameterIndex);
            const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter);
            control->setProperty("id", hosted != nullptr ? hosted->getParameterID() : juce::String(parameterIndex));
            control->setProperty("name", name);
            control->setProperty("value", parameter->getValue());
            control->setProperty("discrete", parameter->isDiscrete());
            juce::var data(control.release());
            inventory.add(data);
            if (name.containsIgnoreCase("phase") || name.containsIgnoreCase("retrigger")
                || name.containsIgnoreCase("drift") || name.containsIgnoreCase("random")
                || name.containsIgnoreCase("noise") || name.containsIgnoreCase("unison") || name.containsIgnoreCase("master"))
                determinismControls.add(data);
        }
        metrics->setProperty("parameterInventory", inventory);
        metrics->setProperty("determinismControls", determinismControls);
        const auto selectedId = profile.isVoid() ? juce::String() : profile["automatedParameterId"].toString();
        const auto continuous = std::find_if(parameters.begin(), parameters.end(),
            [&](const auto* parameter)
            {
                const auto index = static_cast<int>(std::find(parameters.begin(), parameters.end(), parameter) - parameters.begin());
                return parameter->isAutomatable() && !parameter->isDiscrete()
                    && (selectedId.isEmpty() || idFor(parameter, index) == selectedId);
            });
        if (continuous == parameters.end())
            return juce::Result::fail("Workflow qualification requires a continuous automatable control.");
        const auto index = static_cast<int>(std::distance(parameters.begin(), continuous));
        const auto* identified = dynamic_cast<const juce::HostedAudioProcessorParameter*>(*continuous);
        const auto parameterId = identified != nullptr ? identified->getParameterID() : juce::String(index);
        const auto originalValue = (*continuous)->getValue();
        const auto targetValue = originalValue < 0.5f ? 0.75f : 0.25f;
        (*continuous)->setValueNotifyingHost(targetValue);
        juce::AudioBuffer<float> scratch(std::max({ 2, first->getTotalNumInputChannels(), first->getTotalNumOutputChannels() }), quantum);
        scratch.clear();
        juce::MidiBuffer midi;
        first->processBlock(scratch, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        juce::MemoryBlock editedState;
        first->getStateInformation(editedState);
        const auto independent = index < second->getParameters().size()
            && std::abs(second->getParameters()[index]->getValue() - originalValue) < 1e-5f;
        if (!check("workflow-instance-independence", independent && editedState != initialState && !editedState.isEmpty(),
                "A changed control/state on the first instance leaves the second instance's control untouched."))
            return juce::Result::fail("Vendor instances did not preserve independent editable state.");
        metrics->setProperty("automatedParameterId", parameterId);
        metrics->setProperty("automatedParameterName", (*continuous)->getName(128));
        const auto source = directory.getChildFile("synthetic-workflow.wav");
        if (const auto written = writeSource(source); written.failed())
            return written;
        auto project = studio::Project::createDefault();
        project.metronomeEnabled = false;
        project.tempoChanges = { { 0.0, 120.0, false }, { 0.1, 180.0, false } };
        auto& track = project.tracks.front();
        if (description.isInstrument)
        {
            track.type = studio::TrackType::instrument;
            studio::MidiClip clip;
            clip.durationBeats = 4.0;
            studio::MidiNote note;
            note.pitch = 60;
            note.velocity = 96;
            note.durationBeats = 0.5;
            clip.notes.push_back(note);
            track.midiClips.push_back(clip);
        }
        else
        {
            studio::AudioClip clip;
            clip.sourceFile = source;
            clip.durationSeconds = clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 0.5;
            track.clips.push_back(clip);
        }
        studio::PluginInsert insert;
        insert.pluginIdentifier = description.createIdentifierString();
        insert.name = description.name;
        insert.format = description.pluginFormatName;
        insert.bridgeMode = studio::PluginBridgeMode::sandboxed;
        track.inserts.push_back(insert);
        studio::StudioAudioEngine::PluginRuntimeRequest request;
        request.trackId = track.id;
        request.insertId = insert.id;
        request.name = insert.name;
        request.description = description;
        request.bridgeMode = insert.bridgeMode;
        request.state = initialState;
        studio::AutomationLane lane;
        lane.name = "Synthetic vendor parameter steps";
        lane.target.type = studio::AutomationTargetType::pluginParameter;
        lane.target.trackId = track.id;
        lane.target.insertId = insert.id;
        lane.target.parameterIndex = index;
        lane.target.parameterId = parameterId;
        lane.interpolation = studio::AutomationInterpolation::step;
        lane.points = { { juce::Uuid().toString(), 0.0, originalValue },
                        { juce::Uuid().toString(), 0.13, 0.25 },
                        { juce::Uuid().toString(), 0.31, 0.75 } };
        project.automationLanes.push_back(lane);
        const auto render = [&](const studio::Project& model,
                                const std::vector<studio::StudioAudioEngine::PluginRuntimeRequest>& requests,
                                juce::AudioBuffer<float>& output, const char* label) -> juce::Result
        {
            auto engine = std::make_unique<studio::StudioAudioEngine>(juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH));
            if (const auto configured = engine->configureAudioForTesting(rate, quantum); configured.failed())
                return configured;
            const auto started = juce::Time::getMillisecondCounterHiRes();
            const auto result = engine->renderRangeToBuffer(model, output, rate, { 0.0, 0.5, 0.0 }, requests);
            metrics->setProperty(label, juce::Time::getMillisecondCounterHiRes() - started);
            if (result.failed())
                return result;
            if (engine->pluginLateBlockCount() != 0 || engine->pluginAutomationEventDropCount() != 0)
                return juce::Result::fail("Offline workflow encountered late/fallback or lost automation.");
            return output.getNumChannels() == 2 && output.getNumSamples() == contentSamples && finite(output)
                ? juce::Result::ok() : juce::Result::fail("Vendor workflow produced invalid or incomplete audio.");
        };
        juce::AudioBuffer<float> flat, repeated;
        if (const auto rendered = render(project, { request }, flat, "flatRenderMilliseconds"); rendered.failed())
            return rendered;
        if (!check(description.isInstrument ? "workflow-midi-instrument" : "workflow-effect-audio",
                flat.getMagnitude(0, contentSamples) > 1e-5f,
                "A synthetic MIDI note or deterministic stereo source produces finite audible-level buffer data without an audio device."))
            return juce::Result::fail("The configured vendor did not produce an observable response.");
        if (const auto rendered = render(project, { request }, repeated, "repeatRenderMilliseconds"); rendered.failed())
            return rendered;
        const auto repeatError = difference(flat, repeated);
        metrics->setProperty("repeatMaximumNullError", static_cast<double>(repeatError));
        auto firstDifference = -1, lastDifference = -1;
        juce::Array<juce::var> blockErrors;
        for (int blockStart = 0; blockStart < contentSamples; blockStart += quantum)
        {
            auto maximum = 0.0f;
            for (int sample = blockStart; sample < std::min(contentSamples, blockStart + quantum); ++sample)
                for (int channel = 0; channel < 2; ++channel)
                {
                    const auto delta = std::abs(flat.getSample(channel, sample) - repeated.getSample(channel, sample));
                    maximum = std::max(maximum, delta);
                    if (delta > 1e-5f)
                    {
                        if (firstDifference < 0)
                            firstDifference = sample;
                        lastDifference = sample;
                    }
                }
            blockErrors.add(static_cast<double>(maximum));
        }
        metrics->setProperty("repeatFirstDifferenceSample", firstDifference);
        metrics->setProperty("repeatLastDifferenceSample", lastDifference);
        metrics->setProperty("repeatBlockMaximumErrors", blockErrors);
        const auto deterministic = check("workflow-repeat-null", repeatError <= 1e-5f,
            "Independent fresh workers render identical state/tempo/step-automation within 1e-5; observed " + juce::String(repeatError, 9));

        auto liveEngine = std::make_unique<studio::StudioAudioEngine>(juce::File(STUDIO_DUO_BRIDGE_WORKER_PATH));
        if (const auto configured = liveEngine->configureAudioForTesting(rate, quantum); configured.failed())
            return configured;
        if (const auto started = liveEngine->updateProject(project, { request }); started.failed())
            return started;
        const auto readyDeadline = juce::Time::getMillisecondCounterHiRes() + 15000.0;
        auto ready = false;
        while (juce::Time::getMillisecondCounterHiRes() < readyDeadline)
        {
            const auto statuses = liveEngine->pluginRuntimeStatuses();
            ready = !liveEngine->pluginRuntimeTransitionPending() && !statuses.empty()
                && std::all_of(statuses.begin(), statuses.end(), [](const auto& status)
                    { return status.state == studio::StudioAudioEngine::PluginRuntimeStatus::State::ready; });
            if (ready)
                break;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        }
        if (!ready)
            return juce::Result::fail("The live vendor workflow did not become ready.");
        const auto latency = liveEngine->outputLatencySamplesForTesting();
        metrics->setProperty("declaredLiveLatencySamples", latency);
        juce::AudioBuffer<float> capture(2, contentSamples + latency), block(2, quantum);
        capture.clear();
        liveEngine->play();
        for (int position = 0; position < capture.getNumSamples(); position += quantum)
        {
            liveEngine->renderActiveBlockForTesting(block);
            const auto count = std::min(quantum, capture.getNumSamples() - position);
            for (int channel = 0; channel < 2; ++channel)
                capture.copyFrom(channel, position, block, channel, 0, count);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(25);
        }
        liveEngine->stop();
        if (liveEngine->pluginLateBlockCount() != 0 || liveEngine->pluginAutomationEventDropCount() != 0)
            return juce::Result::fail("The paced exchange lost wet DSP or automation; it is not deadline qualification.");
        juce::AudioBuffer<float> aligned(2, contentSamples);
        for (int channel = 0; channel < 2; ++channel)
            aligned.copyFrom(channel, 0, capture, channel, latency, contentSamples);
        const auto liveError = difference(flat, aligned);
        metrics->setProperty("liveOfflineMaximumNullError", static_cast<double>(liveError));
        metrics->setProperty("liveOfflineDifferenceSpans", differenceSpans(flat, aligned));
        const auto liveMatches = check("workflow-live-offline-null", finite(aligned) && liveError <= 1e-5f,
            "Complete live/offline windows use the graph's declared latency, not a fitted offset; observed " + juce::String(liveError, 9));
        liveEngine->shutdown();
        liveEngine.reset();

        auto nested = project;
        studio::PluginInsert outer, inner;
        outer.name = "Synthetic outer rack";
        outer.kind = studio::PluginInsertKind::serialRack;
        inner.name = "Synthetic inner rack";
        inner.kind = studio::PluginInsertKind::serialRack;
        inner.containerId = outer.id;
        auto leaf = insert;
        leaf.containerId = inner.id;
        nested.tracks.front().inserts = { outer, inner, leaf };
        juce::AudioBuffer<float> nestedAudio;
        if (const auto rendered = render(nested, { request }, nestedAudio, "nestedRenderMilliseconds"); rendered.failed())
            return rendered;
        const auto nestedError = difference(flat, nestedAudio);
        metrics->setProperty("nestedMaximumNullError", static_cast<double>(nestedError));
        metrics->setProperty("nestedDifferenceSpans", differenceSpans(flat, nestedAudio));
        const auto nestedMatches = check("workflow-nested-rack-null", nestedError <= 1e-5f,
            "Two serial rack levels retain stable leaf state, automation and output; observed " + juce::String(nestedError, 9));
        nested.tracks.front().inserts.front().bypassed = true;
        auto dry = project;
        dry.tracks.front().inserts.clear();
        dry.automationLanes.clear();
        juce::AudioBuffer<float> bypassed, reference;
        if (const auto rendered = render(nested, { request }, bypassed, "bypassRenderMilliseconds"); rendered.failed())
            return rendered;
        if (const auto rendered = render(dry, {}, reference, "dryRenderMilliseconds"); rendered.failed())
            return rendered;
        const auto bypassError = difference(bypassed, reference);
        metrics->setProperty("bypassMaximumNullError", static_cast<double>(bypassError));
        const auto bypassMatches = check("workflow-parent-bypass", bypassError <= 1e-5f,
            "Bypassing the containing rack yields the explicit dry/no-instrument reference; observed " + juce::String(bypassError, 9));
        checks.push_back({ "workflow-sidechain", "skip", first->getBusCount(true) > 1
            ? "An advertised sidechain still requires an explicit configured route qualification."
            : "This exact processor advertises no auxiliary input bus." });
        checks.push_back({ "workflow-multi-output", "skip", first->getBusCount(false) > 1
            ? "Advertised auxiliary outputs still require explicit per-bus route qualification."
            : "This exact processor advertises only its main output bus." });
        return deterministic && liveMatches && nestedMatches && bypassMatches
            ? juce::Result::ok() : juce::Result::fail("One or more configured vendor workflow/null checks failed.");
    };
    const auto result = run();
    measurements = juce::var(metrics.release());
    if (!directory.deleteRecursively())
        return juce::Result::fail("Cannot remove the owned synthetic vendor workflow.");
    return result;
}

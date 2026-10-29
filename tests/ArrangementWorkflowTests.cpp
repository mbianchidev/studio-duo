#include "TestHarness.h"
#include "TestSuites.h"

#include "model/ProjectCommands.h"
#include "model/ArrangementEditing.h"
#include "audio/StudioAudioEngine.h"
#include "ui/TimelineComponent.h"
#include "render/ArrangementRender.h"
#include "ReleaseFixtures.h"
#include "devices/DeviceRegistry.h"
#include "audio/AudioMemoryBudget.h"
#include "ui/ClipEffectsComponent.h"
#include "dawproject_io/DawProjectIO.h"
#include "project_io/ProjectFile.h"

#include <cmath>

namespace
{
juce::MouseEvent eventFor(juce::Component& component, juce::Point<float> point,
                          juce::Point<float> start, juce::ModifierKeys modifiers)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point, modifiers,
             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, now, start, now, 1, point != start };
}

void nativeTimelineGestures()
{
    auto project = studio::Project::createDefault();
    studio::AudioClip clip;
    clip.startSeconds = 1.0;
    clip.sourceOffsetSeconds = 2.0;
    clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 12.0;
    clip.durationSeconds = 3.0;
    project.tracks[0].clips = { clip };
    auto other = clip;
    other.id = juce::Uuid().toString();
    project.tracks[1].clips = { other };
    studio::TimelineComponent timeline;
    timeline.setProject(&project);
    timeline.setSnapEnabled(false);
    timeline.setSize(1200, 500);
    timeline.onClipSelected = [&](const auto& track, const auto& id) { timeline.setSelection(track, id); };
    const auto x = timeline.xForSeconds(2.0);
    timeline.mouseDown(eventFor(timeline, { x, 125.0f }, { x, 125.0f }, {}));
    timeline.mouseUp(eventFor(timeline, { x, 125.0f }, { x, 125.0f }, {}));
    timeline.mouseDown(eventFor(timeline, { x, 213.0f }, { x, 213.0f }, juce::ModifierKeys::commandModifier));
    expect(timeline.selectedClipIds().size() == 2 && timeline.selectedTrackIds().size() == 2,
           "Command/Ctrl-click retains clips and their owners across timeline tracks.");

    double slipDelta = 0.0, stretchedDuration = 0.0;
    int moved = 0, gains = 0, fadeIns = 0, fadeOuts = 0;
    timeline.onClipMoved = [&](const auto&, const auto&, double) { ++moved; };
    timeline.onClipSlipped = [&](const auto&, double delta, bool) { slipDelta = delta; return juce::Result::ok(); };
    timeline.onClipStretched = [&](const auto&, double duration) { stretchedDuration = duration; return juce::Result::ok(); };
    timeline.onClipGainChanged = [&](const auto&, float) { ++gains; };
    timeline.onClipFadeChanged = [&](const auto&, bool fadeIn, double, float)
    { if (fadeIn) ++fadeIns; else ++fadeOuts; };
    const auto drag = [&](juce::Point<float> start, juce::Point<float> end, juce::ModifierKeys modifiers)
    {
        timeline.mouseDown(eventFor(timeline, start, start, modifiers));
        timeline.mouseDrag(eventFor(timeline, end, start, modifiers));
        timeline.mouseUp(eventFor(timeline, end, start, modifiers));
    };
    drag({ x, 125.0f }, { x + 24.0f, 125.0f }, juce::ModifierKeys::altModifier);
    expect(std::abs(slipDelta + 0.25) < 1.0e-9 && moved == 0,
           "Alt-drag slips source audio horizontally instead of moving the timeline clip.");
    timeline.setStretchToolEnabled(true);
    const auto right = timeline.xForSeconds(4.0);
    drag({ right - 2.0f, 140.0f }, { right + 94.0f, 140.0f }, {});
    expect(std::abs(stretchedDuration - 4.0) < 1.0e-9,
           "The stretch tool dispatches a pitch-preserving resize from the clip end.");
    timeline.setStretchToolEnabled(false);
    drag({ x, 96.0f }, { x, 80.0f }, {});
    const auto left = timeline.xForSeconds(1.0);
    drag({ left + 1.0f, 96.0f }, { left + 24.0f, 96.0f }, {});
    drag({ right - 1.0f, 96.0f }, { right - 24.0f, 96.0f }, {});
    expect(gains == 1 && fadeIns == 1 && fadeOuts == 1,
           "The smart tool independently dispatches center gain, start fade-in and end fade-out gestures.");
}

std::vector<studio::StudioAudioEngine::PluginRuntimeRequest> deviceRequests(const studio::Project& project)
{
    std::vector<studio::StudioAudioEngine::PluginRuntimeRequest> requests;
    for (const auto& track : project.tracks)
        for (const auto& insert : track.inserts)
        {
            if (insert.isContainer())
                continue;
            studio::StudioAudioEngine::PluginRuntimeRequest request;
            request.trackId = track.id;
            request.insertId = insert.id;
            request.name = insert.name;
            request.deviceIdentifier = insert.pluginIdentifier;
            request.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
            requests.push_back(std::move(request));
        }
    return requests;
}

void freezeAndRegionEffects()
{
    release_fixture::Directory directory;
    const auto media = directory.file.getChildFile("synthetic-effects.wav");
    expect(release_fixture::writeMedia(media).wasOk(), "Synthetic effects source can be written.");
    auto project = studio::Project::createDefault();
    project.metronomeEnabled = false;
    auto& track = project.tracks.front();
    const auto sourceId = track.id;
    studio::AudioClip clip;
    clip.sourceFile = media;
    clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = clip.durationSeconds = 0.125;
    clip.startSeconds = 0.25;
    track.clips = { clip };
    track.volumeDecibels = -6.0f;
    track.pan = 0.25f;
    track.polarityInverted = true;
    studio::PluginInsert gain;
    gain.name = "Synthetic gain";
    gain.pluginIdentifier = "studio.device.gain";
    gain.bundledDevice = true;
    gain.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    track.inserts = { gain };
    studio::PluginInsert rack;
    rack.name = "Synthetic serial rack";
    rack.kind = studio::PluginInsertKind::serialRack;
    track.inserts.insert(track.inserts.begin(), rack);
    track.inserts.back().containerId = rack.id;
    project.tracks[1].type = studio::TrackType::aux;
    studio::RoutingConnection send;
    send.name = "Synthetic pre-fader send";
    send.kind = studio::RouteKind::send;
    send.tap = studio::RouteTap::preFader;
    send.sourceTrackId = sourceId;
    send.destination.trackId = project.tracks[1].id;
    send.gainDecibels = -9.0f;
    project.routingConnections.push_back(send);
    studio::AutomationLane volume;
    volume.target.trackId = sourceId;
    volume.points = { { "volume-start", 0.0, 0.75 }, { "volume-end", 0.6, 0.9 } };
    studio::AutomationLane parameter;
    parameter.target.trackId = sourceId;
    parameter.target.insertId = gain.id;
    parameter.target.type = studio::AutomationTargetType::deviceParameter;
    parameter.target.parameterIndex = 0;
    parameter.target.parameterId = "gain";
    parameter.points = { { "gain-start", 0.0, 0.85 }, { "gain-end", 0.6, 1.0 } };
    project.automationLanes = { volume, parameter };
    studio::Track vca;
    vca.type = studio::TrackType::vca;
    vca.name = "Synthetic external VCA";
    vca.controlledTrackIds = { sourceId };
    vca.volumeDecibels = -3.0f;
    studio::AutomationLane vcaAutomation;
    vcaAutomation.target.trackId = vca.id;
    vcaAutomation.target.type = studio::AutomationTargetType::vcaVolume;
    vcaAutomation.points = { { "vca-start", 0.0, 0.7 }, { "vca-end", 0.6, 0.8 } };
    project.tracks.insert(project.tracks.end() - 1, vca);
    project.automationLanes.push_back(vcaAutomation);
    auto engine = std::make_unique<studio::StudioAudioEngine>();
    juce::AudioBuffer<float> reference, frozenAudio;
    auto rendered = engine->renderRangeToBuffer(project, reference, 48000.0,
                                                { 0.0, 1.0, 0.0 }, deviceRequests(project));
    juce::String error;
    auto plan = studio::ArrangementRender::prepareFreeze(project, sourceId, error);
    expect(rendered.wasOk() && plan.has_value(), error.toRawUTF8());
    if (rendered.failed() || !plan)
        return;
    plan->range.endSeconds = 1.0;
    plan->range.tailSeconds = 0.0;
    studio::AudioClip frozen;
    rendered = studio::ArrangementRender::render(*plan, directory.file.getChildFile("freeze.wav"),
                                                 48000.0, deviceRequests(plan->project), frozen);
    juce::AudioFormatManager frozenFormats;
    frozenFormats.registerBasicFormats();
    const auto frozenReader = std::unique_ptr<juce::AudioFormatReader>(frozenFormats.createReaderFor(frozen.sourceFile));
    juce::AudioBuffer<float> stem(2, 48000);
    stem.clear();
    if (frozenReader != nullptr)
        frozenReader->read(&stem, 0, 48000, 0, true, true);
    auto edit = studio::ArrangementRender::commit(project, *plan, frozen, error);
    studio::CommandStack history;
    expect(rendered.wasOk() && edit && history.perform(std::move(edit), project, error)
               && !project.isTrackActive(sourceId) && !project.hasActivePluginInserts()
               && project.findTrack(sourceId)->inserts.size() == 2
               && studio::Project::fromVar(project.toVar(), error).has_value(),
           ("Freeze retains the original processors and automation while disabling their runtime: " + error).toRawUTF8());
    expect(frozenReader != nullptr && frozenReader->usesFloatingPointData
               && stem.getMagnitude(0, stem.getNumSamples()) > 1.0f,
           "Frozen float stems preserve pre-fader headroom above full scale.");
    auto lockedProject = project;
    const auto* lockedClip = lockedProject.findClip(clip.id);
    auto changedClip = *lockedClip;
    changedClip.gainDecibels += 3.0f;
    studio::SetClipStateCommand lockedEdit(sourceId, *lockedClip, changedClip, "Edit frozen clip");
    expect(!lockedEdit.perform(lockedProject, error) && error.containsIgnoreCase("unfreeze")
               && lockedProject.findClip(clip.id)->gainDecibels == 0.0f,
           "Frozen original clips cannot be changed through command or shortcut paths before unfreezing.");
    rendered = engine->renderRangeToBuffer(project, frozenAudio, 48000.0,
                                            { 0.0, 1.0, 0.0 }, deviceRequests(project));
    auto maximumError = 0.0f;
    int errorSample = 0;
    if (rendered.wasOk() && reference.getNumSamples() == frozenAudio.getNumSamples())
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < reference.getNumSamples(); ++sample)
            {
                const auto difference = std::abs(reference.getSample(channel, sample) - frozenAudio.getSample(channel, sample));
                if (difference > maximumError)
                {
                    maximumError = difference;
                    errorSample = sample;
                }
            }
    else
        maximumError = 1.0f;
    const auto sampleText = [errorSample](const juce::AudioBuffer<float>& buffer, int channel)
    {
        return channel < buffer.getNumChannels() && errorSample < buffer.getNumSamples()
            ? juce::String(buffer.getSample(channel, errorSample), 9) : juce::String("unavailable");
    };
    expect(maximumError < 1.0e-6f,
           ("Frozen playback retains plugin automation, fader automation, pan, polarity and pre-fader sends; error "
            + juce::String(maximumError, 9) + " at sample " + juce::String(errorSample)
            + "; reference L/R " + sampleText(reference, 0) + "/"
            + sampleText(reference, 1) + "; frozen L/R "
            + sampleText(frozenAudio, 0) + "/"
            + sampleText(frozenAudio, 1) + "; stem L/R "
            + sampleText(stem, 0) + "/" + sampleText(stem, 1)
            + "; parameter lane count " + juce::String(static_cast<int>(plan->project.automationLanes.size()))).toRawUTF8());
    const auto package = directory.file.getChildFile("frozen.studioduo");
    expect(studio::ProjectFile::save(project, package).wasOk(), "A frozen rack project can be saved natively.");
    const auto exchange = studio::DawProjectIO::exportProject(project, package, directory.file.getChildFile("frozen.dawproject"));
    expect(exchange.succeeded() && std::any_of(exchange.report.issues.begin(), exchange.report.issues.end(),
               [](const auto& issue) { return issue.code == "unsupported.freeze-link"; }),
           ("Frozen racks export their audio with an explicit native-state loss report: "
            + exchange.result.getErrorMessage()).toRawUTF8());
    auto unfreeze = studio::ArrangementRender::unfreeze(project, plan->renderedTrack.id, error);
    expect(unfreeze && history.perform(std::move(unfreeze), project, error)
               && project.isTrackActive(sourceId) && project.findTrack(plan->renderedTrack.id) == nullptr
               && history.undo(project) && !project.isTrackActive(sourceId)
               && history.undo(project) && project.isTrackActive(sourceId),
           "Unfreeze restores the editable original, removes its proxy and supports undo of both transitions.");

    studio::ClipEffectsSettings settings;
    settings.startSeconds = 0.03125;
    settings.endSeconds = 0.09375;
    settings.tailSeconds = 0.0;
    settings.effects.inserts = { gain };
    studio::Project effectsProject;
    effectsProject.tracks = { settings.effects };
    settings.requests = deviceRequests(effectsProject);
    auto processor = studio::DeviceRegistry::create(gain.pluginIdentifier);
    processor->getParameters()[0]->setValueNotifyingHost(0.0f);
    processor->getStateInformation(settings.requests.front().state);
    studio::AudioClip processed;
    rendered = studio::ArrangementRender::applyClipEffects(project, clip, settings,
        directory.file.getChildFile("region.wav"), 48000.0, processed);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(processed.sourceFile));
    juce::AudioBuffer<float> region(2, 6000);
    region.clear();
    const auto read = reader != nullptr && reader->read(&region, 0, 6000, 0, true, true);
    expect(rendered.wasOk() && read && processed.id == clip.id
               && std::abs(processed.startSeconds - clip.startSeconds) < 1.0e-9
               && region.getMagnitude(0, 0, 1000) > 0.1f
               && region.getMagnitude(0, 2500, 1000) < 0.01f
               && region.getMagnitude(0, 5000, 1000) > 0.1f
               && project.findClip(clip.id)->sourceFile == media,
           ("AudioSuite changes only the chosen clip interval and leaves original media untouched: "
            + rendered.getErrorMessage() + "; levels " + juce::String(region.getMagnitude(0, 0, 1000), 6)
            + "/" + juce::String(region.getMagnitude(0, 2500, 1000), 6)
            + "/" + juce::String(region.getMagnitude(0, 5000, 1000), 6)).toRawUTF8());
    auto unrelated = project;
    studio::Track unavailable;
    unavailable.name = "Unrelated unavailable processor";
    studio::PluginInsert missing;
    missing.pluginIdentifier = "synthetic.unavailable";
    missing.name = "Synthetic unavailable plugin";
    missing.missing = true;
    unavailable.inserts = { missing };
    const auto unavailableId = unavailable.id;
    unrelated.tracks.insert(unrelated.tracks.end() - 1, unavailable);
    auto isolated = studio::ArrangementRender::prepareFreeze(unrelated, sourceId, error);
    expect(isolated && !isolated->project.isTrackActive(unavailableId)
               && isolated->project.findTrack(unavailableId)->inserts.empty()
               && unrelated.findTrack(unavailableId)->active && unrelated.findTrack(unavailableId)->inserts.size() == 1,
           "Freeze excludes unrelated unavailable processors without changing the source project.");
}

bool waitFor(const std::function<bool()>& ready)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (!ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    return ready();
}

void groupedTrackWorkflows()
{
    auto project = studio::Project::createDefault();
    const auto firstId = project.tracks[0].id, secondId = project.tracks[1].id;
    studio::CommandStack history;
    juce::String error;
    auto group = studio::ArrangementEditing::groupTracks(project, { firstId, secondId }, true, error);
    expect(group && history.perform(std::move(group), project, error), error.toRawUTF8());
    const auto busId = project.findTrack(firstId)->folderTrackId;
    const auto order = studio::ArrangementEditing::visibleTracks(project);
    expect(order.size() == 4 && order.front()->id == busId && order[1]->id == firstId
                && project.findTrack(firstId)->outputTrackId == busId
                && studio::ArrangementEditing::trackSubtreeIds(project, { busId }).size() == 3,
            "Folder creation groups selected tracks in visual order and wires their shared audio bus.");
    const auto before = studio::TrackLifecycleState::fromTrack(*project.findTrack(busId));
    auto collapsed = before;
    collapsed.folderCollapsed = true;
    expect(history.perform(std::make_unique<studio::SetTrackLifecycleCommand>(busId, before, collapsed, "Collapse"), project, error),
            error.toRawUTF8());
    studio::TimelineComponent timeline;
    timeline.setProject(&project);
    expect(timeline.preferredHeight(0) == 80 + 2 * 88 + 44, "Collapsed folder members do not occupy timeline rows.");
    auto deactivated = collapsed;
    deactivated.active = false;
    expect(history.perform(std::make_unique<studio::SetTrackLifecycleCommand>(busId, collapsed, deactivated, "Deactivate"), project, error)
                && !project.isTrackActive(firstId) && !project.isTrackActive(secondId)
                && history.undo(project) && project.isTrackActive(firstId)
                && history.undo(project) && history.undo(project) && project.tracks.size() == 3,
            "Folder deactivation and grouping restore their exact source tracks through undo.");

    studio::Project midi;
    midi.metronomeEnabled = false;
    std::vector<juce::String> trackIds, clipIds;
    for (int index = 0; index < 3; ++index)
    {
        studio::Track source;
        source.type = studio::TrackType::midi;
        source.name = "Synthetic MIDI " + juce::String(index + 1);
        studio::MidiClip clip;
        studio::MidiNote note;
        note.pitch = 36;
        note.velocity = 100;
        note.durationBeats = 0.25;
        clip.notes = { note };
        source.midiClips = { clip };
        trackIds.push_back(source.id);
        clipIds.push_back(clip.id);
        midi.tracks.push_back(std::move(source));
    }
    studio::Track master;
    master.type = studio::TrackType::master;
    midi.tracks.push_back(master);
    auto instrumentGroup = studio::ArrangementEditing::groupTracks(midi, trackIds, true, error);
    studio::CommandStack midiHistory;
    expect(instrumentGroup && midiHistory.perform(std::move(instrumentGroup), midi, error), error.toRawUTF8());
    const auto instrumentId = midi.findTrack(trackIds.front())->folderTrackId;
    studio::PluginInsert drum;
    drum.pluginIdentifier = "studio.device.drum-composer";
    drum.name = "Synthetic group instrument";
    drum.bundledDevice = true;
    drum.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    expect(midi.findTrack(instrumentId)->type == studio::TrackType::instrument
                && midiHistory.perform(std::make_unique<studio::AddPluginInsertCommand>(instrumentId, drum), midi, error),
            "MIDI grouping creates a ready-wired instrument bus that accepts a synth.");
    release_fixture::Directory directory;
    auto bounce = studio::ArrangementRender::prepareBounce(midi, clipIds, false, error);
    expect(bounce.has_value(), error.toRawUTF8());
    if (bounce)
    {
        bounce->range.endSeconds = 0.02;
        bounce->range.tailSeconds = 0.0;
        studio::AudioClip printed;
        const auto result = studio::ArrangementRender::render(*bounce, directory.file.getChildFile("midi-bounce.wav"),
                                                               48000.0, deviceRequests(bounce->project), printed);
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(printed.sourceFile));
        juce::AudioBuffer<float> audio(2, 960);
        audio.clear();
        expect(result.wasOk() && reader != nullptr && reader->read(&audio, 0, 960, 0, true, true)
                    && audio.getMagnitude(0, 960) > 0.001f,
                ("Bouncing MIDI children renders their downstream instrument, not silence: " + result.getErrorMessage()).toRawUTF8());
    }
    auto inactiveInstrument = midi;
    for (const auto& id : trackIds)
        inactiveInstrument.findTrack(id)->folderTrackId.clear();
    inactiveInstrument.findTrack(instrumentId)->active = false;
    expect(!studio::ArrangementRender::prepareBounce(inactiveInstrument, clipIds, false, error)
               && error.contains("active instrument"),
           "Active MIDI clips cannot be replaced by silence from an unavailable downstream instrument.");
    auto freeze = studio::ArrangementRender::prepareFreeze(midi, instrumentId, error);
    expect(freeze.has_value(), error.toRawUTF8());
    if (freeze)
    {
        freeze->range.endSeconds = 0.02;
        freeze->range.tailSeconds = 0.0;
        studio::AudioClip printed;
        const auto result = studio::ArrangementRender::render(*freeze, directory.file.getChildFile("instrument-freeze.wav"),
                                                               48000.0, deviceRequests(freeze->project), printed);
        auto edit = studio::ArrangementRender::commit(midi, *freeze, printed, error);
        expect(result.wasOk() && edit && midiHistory.perform(std::move(edit), midi, error)
                    && !midi.isTrackActive(instrumentId) && !midi.isTrackActive(trackIds.front())
                    && midi.frozenSourceTrackId(trackIds.front()) == instrumentId,
                "Freezing a synth folder suspends its MIDI members and keeps their original notes.");
        auto restore = studio::ArrangementRender::unfreeze(midi, trackIds.front(), error);
        expect(restore && midiHistory.perform(std::move(restore), midi, error)
                    && midi.isTrackActive(instrumentId) && midi.isTrackActive(trackIds.front())
                    && midi.findTrack(trackIds.front())->midiClips.front().notes.size() == 1,
                "Unfreeze from a frozen folder member restores its instrument and notes.");
    }
    auto unsupported = midi;
    studio::Track internalVca;
    internalVca.type = studio::TrackType::vca;
    internalVca.folderTrackId = instrumentId;
    internalVca.controlledTrackIds = { instrumentId };
    unsupported.tracks.insert(unsupported.tracks.end() - 1, internalVca);
    expect(!studio::ArrangementRender::prepareFreeze(unsupported, instrumentId, error)
                && error.contains("VCA") && unsupported.findTrack(instrumentId)->frozenTrackId.isEmpty(),
           "Unrepresentable internal VCA control is rejected before changing or approximating the frozen mix.");
    auto live = std::make_unique<studio::StudioAudioEngine>();
    midi.findTrack(trackIds.front())->active = false;
    expect(live->configureAudioForTesting(48000.0, 512).wasOk()
                && live->updateProject(midi, deviceRequests(midi)).wasOk()
                && waitFor([&] { return !live->pluginRuntimeTransitionPending(); }),
            "The live synthetic instrument bus is ready without opening audio or MIDI hardware.");
    live->setMidiAuditionEnabled(true);
    const auto scoped = live->capturePluginStates(100, { instrumentId });
    expect(scoped.size() == 1 && scoped.front().trackId == instrumentId && scoped.front().result.wasOk()
               && live->capturePluginStates(100, { trackIds.front() }).empty(),
           "Scoped processor capture touches only the requested track, not unrelated chains.");
    expect(live->enqueueMidiInput(trackIds.front(), juce::MidiMessage::noteOn(1, 36, juce::uint8(100))).failed()
                && live->enqueueMidiInput(trackIds.front(), juce::MidiMessage::noteOff(1, 36)).wasOk(),
            "Deactivated MIDI sources reject new notes while still accepting release/cleanup messages.");
    midi.findTrack(instrumentId)->active = false;
    expect(live->updateProject(midi, deviceRequests(midi)).wasOk()
                && waitFor([&] { return !live->pluginRuntimeTransitionPending(); })
                && live->pluginRuntimeStatuses().empty() && midi.findTrack(instrumentId)->inserts.size() == 1,
            "Deactivation removes the active processor runtime without deleting its saved chain.");
}

void clipEffectsInspectorControls()
{
    release_fixture::Directory directory;
    studio::PluginCatalog catalog(directory.file.getChildFile("catalog"), false);
    auto context = studio::Project::createDefault();
    studio::AudioClip clip;
    clip.durationSeconds = 0.125;
    int requestBuilds = 0;
    std::vector<studio::PluginInsert> requestedChain;
    studio::ClipEffectsComponent inspector(context, clip, catalog, [&](const auto& project)
    {
        ++requestBuilds;
        requestedChain = project.tracks.front().inserts;
        return deviceRequests(project);
    });
    auto* start = dynamic_cast<juce::TextEditor*>(inspector.findChildWithID("Clip-FX-range-start"));
    auto* end = dynamic_cast<juce::TextEditor*>(inspector.findChildWithID("Clip-FX-range-end"));
    auto* tail = dynamic_cast<juce::TextEditor*>(inspector.findChildWithID("Clip-FX-effects-tail"));
    studio::PluginInsertPanel* rack = nullptr;
    juce::Label* status = nullptr;
    for (auto* child : inspector.getChildren())
    {
        if (auto* panel = dynamic_cast<studio::PluginInsertPanel*>(child))
             rack = panel;
        if (child->getTitle() == "Clip FX status")
            status = dynamic_cast<juce::Label*>(child);
    }
    expect(start != nullptr && end != nullptr && tail != nullptr && rack != nullptr && status != nullptr,
            "The clip inspector exposes real named range, tail and rack controls.");
    if (start == nullptr || end == nullptr || tail == nullptr || rack == nullptr || status == nullptr)
        return;
    int applies = 0, closes = 0;
    inspector.onApply = [&](studio::ClipEffectsSettings settings)
    {
        ++applies;
        expect(settings.effects.inserts.size() == 1 && settings.requests.size() == 1
                    && std::abs(settings.startSeconds - 0.03125) < 1.0e-9
                    && std::abs(settings.endSeconds - 0.09375) < 1.0e-9,
                "The inspector dispatches only its clip range and configured plugin state.");
        return juce::Result::ok();
    };
    inspector.onClose = [&] { ++closes; };
    start->setText("nan", false);
    expect(inspector.applyChanges().failed() && applies == 0 && closes == 0,
            "Invalid clip effect ranges stay open without mutating audio.");
    start->setText("0.03125", false);
    end->setText("0.09375", false);
    tail->setText("0", false);
    studio::PluginInsert gain;
    gain.pluginIdentifier = "studio.device.gain";
    gain.name = "Synthetic clip gain";
    gain.bundledDevice = true;
    gain.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    rack->onSetChain({}, { gain }, "Add synthetic effect");
    expect(waitFor([&] { return inspector.effectsReady(); })
                && inspector.applyChanges().wasOk() && applies == 1 && closes == 1,
            "The native clip FX inspector loads, captures and applies its own effect without a track insert.");
    auto missing = gain;
    missing.id = juce::Uuid().toString();
    missing.pluginIdentifier = "studio.device.synthetic-missing";
    missing.name = "Synthetic unavailable effect";
    rack->onSetChain({}, { missing }, "Replace synthetic effect");
    expect(waitFor([&] { return status->getText().contains(missing.name); }) && !inspector.effectsReady(),
           "Unavailable clip effects stay visibly failed rather than enabling a successful dry render.");
    const auto beforeReload = requestBuilds;
    rack->onReload({}, missing.id);
    expect(requestBuilds == beforeReload + 1,
           "An unavailable effect can reload its known setup without requiring state from its failed processor.");
    expect(waitFor([&] { return status->getText().contains(missing.name); }), "Reload failure remains visible.");
    rack->onRemove({}, missing.id);
    expect(requestedChain.empty() && !inspector.effectsReady(),
           "A failed effect can be removed without retaining unavailable state.");
    rack->onSetChain({}, { gain }, "Restore synthetic effect");
    expect(waitFor([&] { return inspector.effectsReady(); }) && inspector.applyChanges().wasOk()
               && applies == 2 && closes == 2,
           "A recovered clip inspector can apply a healthy replacement without stale failed state.");
}

void nestedFreezeLocks()
{
    auto project = studio::Project::createDefault();
    const auto innerId = project.tracks[0].id;
    const auto outerId = project.tracks[1].id;
    project.tracks[1].type = studio::TrackType::bus;
    project.tracks[0].folderTrackId = outerId;
    project.tracks[0].outputTrackId = outerId;
    studio::Track innerProxy, outerProxy;
    innerProxy.folderTrackId = innerProxy.outputTrackId = outerId;
    innerProxy.freezeSourceTrackId = innerId;
    outerProxy.freezeSourceTrackId = outerId;
    project.tracks[0].frozenTrackId = innerProxy.id;
    project.tracks[1].frozenTrackId = outerProxy.id;
    project.tracks.insert(project.tracks.end() - 1, { innerProxy, outerProxy });
    juce::String error;
    expect(studio::Project::fromVar(project.toVar(), error).has_value()
               && project.frozenSourceTrackId(innerProxy.id) == outerId,
           ("Nested freeze references preserve the outermost frozen owner: " + error).toRawUTF8());
    const auto before = studio::TrackMixState::fromTrack(innerProxy);
    auto after = before;
    after.volumeDecibels = -6.0f;
    studio::SetTrackMixCommand locked(innerProxy.id, before, after);
    expect(!locked.perform(project, error)
               && std::abs(project.findTrack(innerProxy.id)->volumeDecibels - before.volumeDecibels) < 1.0e-6f,
           "A nested frozen render cannot change inaudibly while its containing bus is frozen.");
    studio::SetTrackMixCommand live(outerProxy.id, before, after);
    expect(live.perform(project, error) && std::abs(project.findTrack(outerProxy.id)->volumeDecibels + 6.0f) < 1.0e-6f,
           "The outermost audible frozen render retains its ordinary fader controls.");
    auto armed = after;
    armed.armed = armed.inputMonitoring = true;
    studio::SetTrackMixCommand recording(outerProxy.id, after, armed);
    expect(!recording.perform(project, error) && !project.findTrack(outerProxy.id)->armed,
           "Frozen render controls cannot enable new recording or live input through non-UI command paths.");
    studio::AutomationLane lane;
    lane.target.trackId = outerProxy.id;
    studio::AddAutomationLaneCommand automation(lane);
    expect(!automation.perform(project, error) && project.automationLanes.empty(),
           "Frozen render automation cannot accept edits that would be discarded when unfreezing.");
    auto restore = studio::ArrangementRender::unfreeze(project, innerProxy.id, error);
    expect(restore && restore->perform(project, error) && project.findTrack(outerProxy.id) == nullptr
               && project.findTrack(innerId)->frozenTrackId == innerProxy.id
               && project.frozenSourceTrackId(innerProxy.id) == innerId && project.isTrackActive(innerProxy.id),
           "Unfreezing a nested render thaws its outer bus first and preserves the earlier inner freeze.");
    studio::SetTrackMixCommand resumed(innerProxy.id, before, after);
    expect(resumed.perform(project, error) && std::abs(project.findTrack(innerProxy.id)->volumeDecibels + 6.0f) < 1.0e-6f,
           "The inner render becomes mixer-editable once its containing bus is unfrozen.");
}

void inactiveToneReferencesAndPreparation()
{
    release_fixture::Directory directory;
    const auto media = directory.file.getChildFile("synthetic-di.wav");
    expect(release_fixture::writeMedia(media).wasOk(), "Synthetic DI material can be written.");
    for (const auto useTake : { false, true })
    {
        auto project = studio::Project::createDefault();
        project.metronomeEnabled = false;
        const auto sourceId = project.tracks[0].id;
        studio::AudioClip clip;
        clip.sourceFile = media;
        clip.durationSeconds = clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 0.125;
        project.tracks[0].clips = { clip };
        project.tracks[0].muted = true;
        if (useTake)
        {
            studio::Track take;
            take.parentTrackId = sourceId;
            take.versionNumber = 1;
            take.clips = { clip };
            project.tracks[0].clips.clear();
            project.tracks[0].activeTakeTrackId = take.id;
            project.tracks.insert(project.tracks.end() - 1, take);
        }
        studio::ReampRoute route;
        route.type = studio::TonePathType::plugin;
        route.sourceTrackId = sourceId;
        route.returnTrackId = project.tracks[1].id;
        route.ownsReturnTrack = true;
        project.reampRoutes = { route };
        auto engine = std::make_unique<studio::StudioAudioEngine>();
        juce::AudioBuffer<float> reference, inactive;
        const auto baseline = engine->renderRangeToBuffer(project, reference, 48000.0, { 0.0, 0.02, 0.0 });
        project.findTrack(sourceId)->active = false;
        studio::PluginInsert unavailable;
        unavailable.name = "Synthetic inactive DI effect";
        unavailable.pluginIdentifier = "studio.device.synthetic-missing";
        project.findTrack(sourceId)->inserts = { unavailable };
        const auto result = engine->renderRangeToBuffer(project, inactive, 48000.0, { 0.0, 0.02, 0.0 },
                                                        deviceRequests(project));
        auto error = 0.0f;
        if (reference.getNumSamples() != inactive.getNumSamples())
            error = 1.0f;
        else
            for (int channel = 0; channel < reference.getNumChannels(); ++channel)
                for (int sample = 0; sample < reference.getNumSamples(); ++sample)
                    error = std::max(error, std::abs(reference.getSample(channel, sample) - inactive.getSample(channel, sample)));
        expect(baseline.wasOk() && result.wasOk() && reference.getNumSamples() > 0
                   && reference.getMagnitude(0, reference.getNumSamples()) > 0.001f && error < 1.0e-6f,
               "An active tone return retains raw DI clip/take audio without loading a deactivated DI's own processors.");
    }

    auto project = studio::Project::createDefault();
    project.metronomeEnabled = false;
    studio::AudioClip clip;
    clip.sourceFile = media;
    clip.durationSeconds = clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 0.125;
    project.tracks[0].clips = { clip };
    auto engine = std::make_unique<studio::StudioAudioEngine>();
    expect(engine->configureAudioForTesting(48000.0, 512).wasOk() && engine->updateProject(project).wasOk(),
           "The live preparation fixture publishes its valid initial graph.");
    const auto capture = [&]
    {
        engine->seekSeconds(0.0);
        engine->play();
        auto audio = engine->renderActiveBlockForTesting(512);
        engine->pause();
        return audio;
    };
    const auto before = capture();
    const auto invalidMedia = directory.file.getChildFile("invalid.wav");
    expect(invalidMedia.replaceWithText("Synthetic invalid audio"), "Invalid render-placement media can be prepared.");
    auto candidate = project;
    candidate.tracks[0].clips.front().sourceFile = invalidMedia;
    expect(engine->validateProjectPreparation(candidate).failed(),
           "Render-placement preparation reports invalid media before any model command is committed.");
    const auto after = capture();
    auto maximumError = 0.0f;
    for (int channel = 0; channel < before.getNumChannels(); ++channel)
        for (int sample = 0; sample < before.getNumSamples(); ++sample)
            maximumError = std::max(maximumError, std::abs(before.getSample(channel, sample) - after.getSample(channel, sample)));
    expect(before.getMagnitude(0, before.getNumSamples()) > 0.001f && maximumError < 1.0e-6f,
           "Rejected render-placement preflight leaves the previously published audio graph unchanged.");
}
}

void arrangementWorkflowTests()
{
    nativeTimelineGestures();
    freezeAndRegionEffects();
    groupedTrackWorkflows();
    clipEffectsInspectorControls();
    nestedFreezeLocks();
    inactiveToneReferencesAndPreparation();
    auto project = studio::Project::createDefault();
    while (project.tracks.size() < 1000)
        project.tracks.insert(project.tracks.end() - 1, studio::Track {});
    studio::AddTrackCommand add(studio::Track {});
    juce::String error;
    expect(!add.perform(project, error) && project.tracks.size() == 1000
               && error.contains("1000"),
           "Adding track 1001 fails explicitly without changing the project.");
    std::size_t memory = 0;
    const auto maximumSamples = studio::maximumAudioBufferBytes / (2 * sizeof(float));
    expect(studio::boundedAudioSampleCount(static_cast<double>(maximumSamples), 2, error).has_value()
               && !studio::boundedAudioSampleCount(static_cast<double>(maximumSamples + 1), 2, error)
               && studio::reservePreparedAudio(studio::maximumAudioBufferBytes, memory, error)
               && !studio::reservePreparedAudio(1, memory, error) && memory == studio::maximumAudioBufferBytes,
           "Audio preparation and render buffers enforce exact memory limits before allocating oversized projects.");

    auto encoded = studio::Track {}.toVar();
    encoded.getDynamicObject()->setProperty("active", false);
    encoded.getDynamicObject()->setProperty("hidden", true);
    encoded.getDynamicObject()->setProperty("folderCollapsed", true);
    encoded.getDynamicObject()->setProperty("frozenTrackId", "rendered-track");
    encoded.getDynamicObject()->setProperty("freezeSourceTrackId", "original-track");
    const auto decoded = studio::Track::fromVar(encoded, error);
    const auto restored = decoded ? decoded->toVar() : juce::var();
    expect(decoded && restored["active"].isBool() && !static_cast<bool>(restored["active"])
               && static_cast<bool>(restored["hidden"]) && static_cast<bool>(restored["folderCollapsed"])
               && restored["frozenTrackId"].toString() == "rendered-track"
               && restored["freezeSourceTrackId"].toString() == "original-track",
           "Track deactivation, hiding, folder collapse and reversible freeze links survive serialization.");

    auto inactiveProject = studio::Project::createDefault();
    inactiveProject.metronomeEnabled = false;
    auto& inactive = inactiveProject.tracks.front();
    inactive.active = false;
    inactive.hidden = true;
    studio::PluginInsert generator;
    generator.pluginIdentifier = "studio.device.generator";
    generator.name = "Synthetic generator";
    generator.bundledDevice = true;
    generator.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    inactive.inserts.push_back(generator);
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = inactive.id;
    request.insertId = generator.id;
    request.name = generator.name;
    request.deviceIdentifier = generator.pluginIdentifier;
    request.bridgeMode = generator.bridgeMode;
    auto engine = std::make_unique<studio::StudioAudioEngine>();
    juce::AudioBuffer<float> audio;
    const auto rendered = engine->renderRangeToBuffer(inactiveProject, audio, 48000.0,
                                                       { 0.0, 0.02, 0.0 }, { request });
    studio::TimelineComponent timeline;
    timeline.setProject(&inactiveProject);
    expect(rendered.wasOk() && audio.getMagnitude(0, audio.getNumSamples()) == 0.0f
               && !inactiveProject.hasActivePluginInserts()
               && inactiveProject.armedAudioParentTrackIds().empty()
               && timeline.preferredHeight(0) == 80 + 2 * 88 + 44,
           "Inactive hidden tracks neither process plugins nor arm recording and do not occupy timeline rows.");

    auto grouped = studio::Project::createDefault();
    const auto originalTracks = grouped.tracks.size();
    studio::Track bus;
    bus.type = studio::TrackType::bus;
    bus.name = "Synthetic bus folder";
    const auto busId = bus.id;
    std::vector<std::unique_ptr<studio::ProjectCommand>> edits;
    edits.push_back(std::make_unique<studio::AddTrackCommand>(bus));
    for (std::size_t index = 0; index < 2; ++index)
    {
        const auto& member = grouped.tracks[index];
        const auto before = studio::TrackRoutingState::fromTrack(member);
        auto after = before;
        after.folderTrackId = busId;
        edits.push_back(std::make_unique<studio::SetTrackRoutingStateCommand>(member.id, before, after));
        edits.push_back(std::make_unique<studio::SetTrackOutputCommand>(member.id, busId));
    }
    studio::CommandStack history;
    const auto groupCreated = history.perform(
        std::make_unique<studio::BatchProjectCommand>("Group tracks", std::move(edits)), grouped, error);
    expect(groupCreated && grouped.tracks.front().folderTrackId == busId
               && grouped.tracks.front().outputTrackId == busId,
           ("A bus folder groups and routes its selected tracks: " + error).toRawUTF8());
    expect(history.undo(grouped) && grouped.tracks.size() == originalTracks
               && grouped.tracks.front().folderTrackId.isEmpty()
               && grouped.tracks.front().outputTrackId.isEmpty()
               && grouped.validateRoutingGraph(error)
               && history.redo(grouped, error) && grouped.validateRoutingGraph(error),
           ("Bus folder undo and redo restore valid membership and output routing: " + error).toRawUTF8());

    studio::AudioClip slipped;
    slipped.startSeconds = 10.0;
    slipped.sourceOffsetSeconds = 2.0;
    slipped.sourceLengthSeconds = slipped.sourceRangeEndSeconds = 12.0;
    slipped.durationSeconds = 3.0;
    slipped.playbackRate = 2.0;
    slipped.warpMarkers = { { 1.0, 4.0 } };
    const auto slip = studio::ArrangementEditing::slipClip(slipped, 1.0, error);
    expect(slip && std::abs(slip->startSeconds - slipped.startSeconds) < 1.0e-9 && slip->durationSeconds == 3.0
               && slip->sourceOffsetSeconds == 3.0 && slip->warpMarkers.front().sourceSeconds == 5.0
               && slip->playbackRate == 2.0,
           "Slipping changes source audio and warp anchors without moving or resizing the clip.");
    const auto stretch = studio::ArrangementEditing::stretchClip(slipped, 6.0, error);
    expect(stretch && stretch->durationSeconds == 6.0 && stretch->playbackRate == 1.0
               && stretch->warpMarkers.front().timelineOffsetSeconds == 2.0
               && std::abs(stretch->sourceSecondsAt(6.0) - slipped.sourceSecondsAt(3.0)) < 1.0e-9,
           "Stretching changes duration while retaining the source range and scaled warp timing.");

    studio::AutomationLane lane;
    lane.target.trackId = inactiveProject.tracks[1].id;
    lane.name = "Synthetic plugin parameter";
    lane.points = { { "start", 0.0, 0.25 }, { "end", 1.0, 0.75 } };
    inactiveProject.automationLanes = { lane };
    timeline.setProject(&inactiveProject);
    const auto normalHeight = timeline.preferredHeight(0);
    expect(timeline.keyPressed(juce::KeyPress('A')) && timeline.preferredHeight(0) > normalHeight
               && timeline.keyPressed(juce::KeyPress('A'))
               && timeline.preferredHeight(0) == normalHeight,
           "A toggles named automation lanes on every visible timeline track without editing the project.");
    studio::AudioClip overlayClip;
    overlayClip.durationSeconds = 1.0;
    inactiveProject.tracks[1].clips = { overlayClip };
    timeline.setProject(&inactiveProject);
    timeline.setSize(1000, 700);
    timeline.setAutomationVisible(true);
    juce::Image image(juce::Image::ARGB, 1000, 700, true, juce::SoftwareImageType());
    juce::Graphics graphics(image);
    timeline.paintEntireComponent(graphics, true);
    const auto painted = timeline.paintStatisticsForTesting();
    expect(painted.automationLanesPainted == 1 && painted.clipAutomationOverlaysPainted == 1,
           "Automation view paints both a named timeline lane and the corresponding clip envelope.");
    timeline.setAutomationVisible(false);
    timeline.paintEntireComponent(graphics, true);
    expect(timeline.paintStatisticsForTesting().automationLanesPainted == 0
               && timeline.paintStatisticsForTesting().clipAutomationOverlaysPainted == 0,
           "Hidden automation paints neither extra lanes nor clip overlays.");
    int opened = 0, created = 0;
    inactiveProject.tracks[1].type = studio::TrackType::instrument;
    timeline.setProject(&inactiveProject);
    timeline.setAutomationVisible(true);
    timeline.onAutomationEditorRequested = [&] { ++opened; };
    timeline.onCreateMidiClip = [&](const auto&, double) { ++created; };
    timeline.mouseDoubleClick(eventFor(timeline, { 400.0f, 180.0f }, { 400.0f, 180.0f }, {}));
    expect(opened == 1 && created == 0,
           "Double-clicking an instrument automation row opens automation rather than creating an accidental MIDI clip.");

    project.metronomeEnabled = false;
    auto largeEngine = std::make_unique<studio::StudioAudioEngine>();
    juce::AudioBuffer<float> largeAudio;
    const auto largeRender = largeEngine->renderRangeToBuffer(project, largeAudio, 48000.0,
                                                               { 0.0, 0.005, 0.0 });
    expect(largeRender.wasOk() && largeAudio.getNumSamples() == 240
               && largeEngine->trackMeterSnapshots().size() == 999,
           "A 1000-track project renders safely and retains meters for every non-master track.");

    release_fixture::Directory directory;
    const auto source = directory.file.getChildFile("synthetic.wav");
    expect(release_fixture::writeMedia(source).wasOk(), "Synthetic bounce source can be written.");
    auto bounceProject = studio::Project::createDefault();
    bounceProject.metronomeEnabled = false;
    studio::AudioClip first;
    first.sourceFile = source;
    first.sourceLengthSeconds = first.sourceRangeEndSeconds = first.durationSeconds = 0.125;
    first.startSeconds = 0.25;
    auto second = first;
    second.id = juce::Uuid().toString();
    second.startSeconds = 0.5;
    bounceProject.tracks.front().clips = { first, second };
    auto plan = studio::ArrangementRender::prepareBounce(bounceProject, { first.id, second.id }, true, error);
    expect(plan.has_value(), error.toRawUTF8());
    if (plan)
    {
        plan->range.tailSeconds = 0.0;
        studio::AudioClip printed;
        const auto result = studio::ArrangementRender::render(*plan,
            directory.file.getChildFile("bounce.wav"), 48000.0, {}, printed);
        auto edit = studio::ArrangementRender::commit(bounceProject, *plan, printed, error);
        studio::CommandStack bounceHistory;
        expect(result.wasOk() && printed.startSeconds == 0.25
                   && std::abs(printed.durationSeconds - 0.375) < 1.0e-9
                   && edit && bounceHistory.perform(std::move(edit), bounceProject, error)
                   && bounceProject.tracks.front().clips.size() == 1
                   && bounceHistory.undo(bounceProject) && bounceProject.tracks.front().clips.size() == 2,
               "In-place bounce preserves the span and silence between selected clips and is fully undoable.");
    }
}

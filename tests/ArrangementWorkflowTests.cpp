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
    timeline.onClipSlipped = [&](const auto&, double delta) { slipDelta = delta; };
    timeline.onClipStretched = [&](const auto&, double duration) { stretchedDuration = duration; };
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
    parameter.points = { { "gain-start", 0.0, 0.7 }, { "gain-end", 0.6, 0.9 } };
    project.automationLanes = { volume, parameter };
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
               && project.findTrack(sourceId)->inserts.size() == 1
               && studio::Project::fromVar(project.toVar(), error).has_value(),
           ("Freeze retains the original processors and automation while disabling their runtime: " + error).toRawUTF8());
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
    expect(maximumError < 1.0e-6f,
           ("Frozen playback retains plugin automation, fader automation, pan, polarity and pre-fader sends; error "
            + juce::String(maximumError, 9) + " at sample " + juce::String(errorSample)
            + "; reference L/R " + juce::String(reference.getSample(0, errorSample), 9) + "/"
            + juce::String(reference.getSample(1, errorSample), 9) + "; frozen L/R "
            + juce::String(frozenAudio.getSample(0, errorSample), 9) + "/"
            + juce::String(frozenAudio.getSample(1, errorSample), 9) + "; stem L/R "
            + juce::String(stem.getSample(0, errorSample), 9) + "/" + juce::String(stem.getSample(1, errorSample), 9)
            + "; parameter lane count " + juce::String(static_cast<int>(plan->project.automationLanes.size()))).toRawUTF8());
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
}
}

void arrangementWorkflowTests()
{
    nativeTimelineGestures();
    freezeAndRegionEffects();
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

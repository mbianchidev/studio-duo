#include "ArrangementRender.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace studio
{
namespace
{
bool audioTrack(const Track& track)
{
    return track.type == TrackType::audio || track.type == TrackType::instrument
        || track.type == TrackType::bus || track.type == TrackType::aux;
}

bool contains(const std::vector<juce::String>& ids, const juce::String& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

Project isolatedProject(const Project& context, Track track)
{
    Project isolated;
    isolated.id = context.id;
    isolated.tempo = context.tempo;
    isolated.timeSignatureNumerator = context.timeSignatureNumerator;
    isolated.timeSignatureDenominator = context.timeSignatureDenominator;
    isolated.tempoChanges = context.tempoChanges;
    isolated.meterChanges = context.meterChanges;
    isolated.sections = context.sections;
    isolated.drumMaps = context.drumMaps;
    isolated.metronomeEnabled = false;
    track.parentTrackId.clear();
    track.folderTrackId.clear();
    track.outputTrackId.clear();
    track.frozenTrackId.clear();
    track.freezeSourceTrackId.clear();
    track.compRegions.clear();
    track.activeTakeTrackId.clear();
    track.active = true;
    track.muted = track.solo = track.armed = track.inputMonitoring = false;
    Track master;
    master.type = TrackType::master;
    master.name = "Render master";
    isolated.tracks = { std::move(track), std::move(master) };
    return isolated;
}

bool prepareStemRouting(Project& project, const std::vector<juce::String>& targets,
                        bool preFader, juce::String& error)
{
    if (!project.canAddTracks(1, error))
        return false;
    std::set<juce::String> processing(targets.begin(), targets.end());
    for (auto changed = true; changed;)
    {
        changed = false;
        for (const auto& track : project.tracks)
        {
            if (track.parentTrackId.isNotEmpty() && processing.contains(track.parentTrackId))
                changed = processing.insert(track.id).second || changed;
            if (track.parentTrackId.isEmpty() && audioTrack(track) && !track.muted
                && processing.contains(project.resolvedOutputTrackId(track)))
                changed = processing.insert(track.id).second || changed;
        }
        for (const auto& route : project.routingConnections)
        {
            const auto* source = project.findTrack(route.sourceTrackId);
            if (route.enabled && source != nullptr && !source->muted
                && processing.contains(route.destination.trackId))
                changed = processing.insert(route.sourceTrackId).second || changed;
        }
    }
    std::set<juce::String> rawSources;
    for (const auto& route : project.reampRoutes)
        if (route.enabled && route.type == TonePathType::plugin && processing.contains(route.returnTrackId))
        {
            rawSources.insert(route.sourceTrackId);
            for (const auto& track : project.tracks)
                if (track.parentTrackId == route.sourceTrackId)
                    rawSources.insert(track.id);
        }
    auto metadata = processing;
    for (const auto& track : project.tracks)
        if (track.type == TrackType::vca && std::any_of(track.controlledTrackIds.begin(), track.controlledTrackIds.end(),
            [&](const auto& id) { return processing.contains(id) && !(preFader && contains(targets, id)); }))
            metadata.insert(track.id);
    for (const auto& id : std::vector<juce::String>(metadata.begin(), metadata.end()))
    {
        const auto* track = project.findTrack(id);
        for (std::size_t depth = 0; track != nullptr && depth <= project.tracks.size(); ++depth)
        {
            const auto parentId = track->parentTrackId.isNotEmpty() ? track->parentTrackId : track->folderTrackId;
            if (parentId.isEmpty())
                break;
            metadata.insert(parentId);
            track = project.findTrack(parentId);
        }
    }
    const auto masterId = project.masterTrackId();
    Track sink;
    sink.type = TrackType::bus;
    sink.name = "Offline dependency sink";
    sink.muted = true;
    const auto sinkId = sink.id;
    project.tracks.push_back(std::move(sink));
    std::set<juce::String> omittedInserts;
    for (auto& track : project.tracks)
    {
        track.solo = false;
        if (track.type == TrackType::master)
        {
            track.inserts.clear();
            track.volumeDecibels = track.pan = 0.0f;
            track.muted = track.polarityInverted = false;
        }
        else if (contains(targets, track.id))
        {
            track.outputTrackId = masterId;
            track.muted = false;
            track.inputMonitoring = false;
            if (preFader)
            {
                track.volumeDecibels = track.pan = 0.0f;
                track.polarityInverted = false;
            }
        }
        else if (audioTrack(track) && track.id != sinkId
                 && project.resolvedOutputTrackId(track) == masterId)
            track.outputTrackId = sinkId;
        if (track.id != masterId && track.id != sinkId && !processing.contains(track.id))
        {
            if (!metadata.contains(track.id))
                track.active = false;
            for (const auto& insert : track.inserts)
                omittedInserts.insert(insert.id);
            track.inserts.clear();
            if (metadata.contains(track.id) && !rawSources.contains(track.id))
            {
                track.clips.clear();
                track.midiClips.clear();
            }
        }
        if ((processing.contains(track.id) && project.isTrackActive(track.id)) || rawSources.contains(track.id))
            for (const auto& clip : track.clips)
                if (!clip.muted && !clip.sourceFile.existsAsFile())
                {
                    error = "Relink missing media before rendering: " + clip.sourceFile.getFileName();
                    return false;
                }
        if (preFader && track.type == TrackType::vca)
            std::erase_if(track.controlledTrackIds, [&](const auto& id) { return contains(targets, id); });
    }
    for (const auto& id : targets)
    {
        auto* track = project.findTrack(id);
        for (std::size_t depth = 0; track != nullptr && depth <= project.tracks.size(); ++depth)
        {
            track = project.findTrack(track->folderTrackId);
            if (track != nullptr)
                track->muted = false;
        }
    }
    std::set<juce::String> omittedRoutes;
    std::erase_if(project.routingConnections, [&](const auto& route)
    {
        const auto omit = route.kind == RouteKind::hardwareOutput || route.kind == RouteKind::controlRoom
            || (contains(targets, route.sourceTrackId) && route.signalType == SignalType::audio)
            || omittedInserts.contains(route.sourceInsertId) || omittedInserts.contains(route.destination.insertId);
        if (omit)
            omittedRoutes.insert(route.id);
        return omit;
    });
    for (auto& route : project.routingConnections)
        if (route.signalType == SignalType::audio && route.destination.trackId == masterId)
            route.destination.trackId = sinkId;
    std::erase_if(project.automationLanes, [&](const auto& lane)
    {
        if (omittedInserts.contains(lane.target.insertId) || omittedRoutes.contains(lane.target.routeId))
            return true;
        if (lane.target.trackId == masterId)
            return true;
        if (!contains(targets, lane.target.trackId))
            return false;
        return lane.target.type == AutomationTargetType::sendGain
            || lane.target.type == AutomationTargetType::sendPan
            || lane.target.type == AutomationTargetType::sendMute
            || (preFader && (lane.target.type == AutomationTargetType::trackVolume
                || lane.target.type == AutomationTargetType::trackPan
                || lane.target.type == AutomationTargetType::trackMute
                || lane.target.type == AutomationTargetType::trackPolarity));
    });
    project.metronomeEnabled = project.loopEnabled = false;
    return project.validateRoutingGraph(error);
}

juce::Result writeRender(const juce::AudioBuffer<float>& audio, const juce::File& destination,
                         double rate, double start, const juce::String& name, AudioClip& result)
{
    AudioExportSettings settings;
    settings.sampleRate = rate;
    settings.bitDepth = 32;
    if (const auto written = AudioExport::write(audio, destination, settings); written.failed())
        return written;
    result = AudioClip {};
    result.name = name;
    result.sourceFile = destination;
    result.sourceHash = juce::SHA256(destination).toHexString();
    result.startSeconds = start;
    result.durationSeconds = result.sourceLengthSeconds = result.sourceRangeEndSeconds
        = static_cast<double>(audio.getNumSamples()) / rate;
    return juce::Result::ok();
}
}

std::optional<ArrangementRenderPlan> ArrangementRender::prepareBounce(
    const Project& source, const std::vector<juce::String>& ids, bool inPlace, juce::String& error)
{
    error.clear();
    if (ids.empty() || std::set<juce::String>(ids.begin(), ids.end()).size() != ids.size())
    {
        error = "Select one or more distinct audio or MIDI clips to bounce.";
        return std::nullopt;
    }
    ArrangementRenderPlan plan;
    plan.project = source;
    plan.clipIds = ids;
    plan.replaceSources = inPlace;
    plan.range.startSeconds = std::numeric_limits<double>::infinity();
    std::vector<juce::String> owners;
    juce::String output;
    for (const auto& id : ids)
    {
        const auto* clip = source.findClip(id);
        const auto* midi = source.findMidiClip(id);
        const auto* track = clip != nullptr ? source.findTrackContainingClip(id) : source.findTrackContainingMidiClip(id);
        if (track == nullptr || (clip == nullptr && midi == nullptr) || !source.isTrackActive(track->id)
            || track->freezeSourceTrackId.isNotEmpty())
        {
            error = "Bounce available clips on active, unfrozen tracks.";
            return std::nullopt;
        }
        if (!contains(owners, track->id))
            owners.push_back(track->id);
        const auto start = clip != nullptr ? clip->startSeconds : source.secondsAtBeat(midi->startBeats);
        const auto end = clip != nullptr ? clip->endSeconds() : source.secondsAtBeat(midi->endBeats());
        plan.range.startSeconds = std::min(plan.range.startSeconds, start);
        plan.range.endSeconds = std::max(plan.range.endSeconds, end);
        const auto destination = source.resolvedOutputTrackId(*track);
        if (output.isNotEmpty() && output != destination)
        {
            error = "Bounce tracks sharing a main output, or bounce differently routed tracks separately.";
            return std::nullopt;
        }
        output = destination;
    }
    const auto* first = source.findTrack(owners.front());
    plan.renderedTrack.name = "Bounce";
    plan.renderedTrack.colour = first->colour;
    plan.renderedTrack.outputTrackId = output;
    if (std::all_of(owners.begin(), owners.end(), [&](const auto& id)
        { return source.findTrack(id)->folderTrackId == first->folderTrackId; }))
        plan.renderedTrack.folderTrackId = first->folderTrackId;
    if (inPlace && owners.size() == 1 && first->type == TrackType::audio)
    {
        auto track = *first;
        std::erase_if(track.clips, [&](const auto& clip) { return !contains(ids, clip.id); });
        track.inserts.clear();
        track.volumeDecibels = track.pan = 0.0f;
        track.polarityInverted = false;
        plan.project = isolatedProject(source, std::move(track));
        plan.destinationTrackId = first->id;
        plan.range.tailSeconds = 0.0;
        return plan;
    }
    if (!source.canAddTracks(1, error))
        return std::nullopt;
    if (inPlace && std::any_of(source.routingConnections.begin(), source.routingConnections.end(),
        [&](const auto& route)
        {
            return route.enabled && contains(owners, route.sourceTrackId)
                && route.kind != RouteKind::mainOutput;
        }))
    {
        error = "Bounce routed sends separately, or freeze the track to preserve its sends.";
        return std::nullopt;
    }
    for (auto& track : plan.project.tracks)
        if (contains(owners, track.id))
        {
            std::erase_if(track.clips, [&](const auto& clip) { return !contains(ids, clip.id); });
            std::erase_if(track.midiClips, [&](const auto& clip) { return !contains(ids, clip.id); });
        }
    if (!prepareStemRouting(plan.project, owners, false, error))
        return std::nullopt;
    return plan;
}

std::optional<ArrangementRenderPlan> ArrangementRender::prepareFreeze(
    const Project& source, const juce::String& id, juce::String& error)
{
    error.clear();
    const auto* track = source.findTrack(id);
    if (track == nullptr || !audioTrack(*track) || track->parentTrackId.isNotEmpty()
        || !source.isTrackActive(id) || track->freezeSourceTrackId.isNotEmpty())
    {
        error = "Freeze an active root audio, instrument, aux or bus track; unfreeze existing renders first.";
        return std::nullopt;
    }
    if (std::any_of(source.routingConnections.begin(), source.routingConnections.end(), [&](const auto& route)
        {
            return route.enabled && route.sourceTrackId == id
                && (route.signalType == SignalType::midi || route.sourceInsertId.isNotEmpty());
        }))
    {
        error = "Print multi-output or MIDI-sending instruments to separate audio tracks before freezing them.";
        return std::nullopt;
    }
    ArrangementRenderPlan plan;
    plan.project = source;
    plan.freezeSourceTrackId = id;
    plan.range.endSeconds = source.lengthSeconds();
    plan.renderedTrack = *track;
    plan.renderedTrack.id = juce::Uuid().toString();
    plan.renderedTrack.name += " Freeze";
    plan.renderedTrack.type = TrackType::audio;
    plan.renderedTrack.inserts.clear();
    plan.renderedTrack.clips.clear();
    plan.renderedTrack.midiClips.clear();
    plan.renderedTrack.compRegions.clear();
    plan.renderedTrack.activeTakeTrackId.clear();
    plan.renderedTrack.controlledTrackIds.clear();
    plan.renderedTrack.armed = plan.renderedTrack.inputMonitoring = false;
    plan.renderedTrack.automationArmed = false;
    plan.renderedTrack.automationMode = AutomationMode::read;
    plan.renderedTrack.frozenTrackId.clear();
    plan.renderedTrack.freezeSourceTrackId = id;
    if (!prepareStemRouting(plan.project, { id }, true, error))
        return std::nullopt;
    return plan;
}

juce::Result ArrangementRender::render(
    const ArrangementRenderPlan& plan, const juce::File& destination, double rate,
    std::vector<StudioAudioEngine::PluginRuntimeRequest> requests, AudioClip& result)
{
    auto engine = std::make_unique<StudioAudioEngine>();
    juce::AudioBuffer<float> audio;
    if (const auto rendered = engine->renderRangeToBuffer(plan.project, audio, rate, plan.range, std::move(requests));
        rendered.failed())
        return rendered;
    const auto written = writeRender(audio, destination, rate, plan.range.startSeconds, plan.renderedTrack.name, result);
    result.colour = plan.renderedTrack.colour;
    return written;
}

std::unique_ptr<ProjectCommand> ArrangementRender::commit(
    const Project& project, const ArrangementRenderPlan& plan, const AudioClip& clip, juce::String& error)
{
    error.clear();
    if (project.id != plan.project.id || !clip.sourceFile.existsAsFile() || clip.durationSeconds <= 0.0)
    {
        error = "The rendered media or its source project is no longer available.";
        return {};
    }
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    if (plan.freezeSourceTrackId.isNotEmpty())
    {
        const auto* source = project.findTrack(plan.freezeSourceTrackId);
        if (source == nullptr || !project.isTrackActive(source->id))
        {
            error = "The freeze source is no longer available or is already frozen.";
            return {};
        }
        auto rendered = plan.renderedTrack;
        rendered.clips = { clip };
        std::vector<RoutingConnection> routes;
        std::map<juce::String, juce::String> routeIds;
        std::optional<RoutingConnection> main;
        for (const auto& route : project.routingConnections)
            if (route.sourceTrackId == source->id && route.signalType == SignalType::audio)
            {
                auto duplicate = route;
                duplicate.id = juce::Uuid().toString();
                duplicate.sourceTrackId = rendered.id;
                routeIds.emplace(route.id, duplicate.id);
                if (route.kind == RouteKind::mainOutput)
                    main = duplicate;
                else
                    routes.push_back(std::move(duplicate));
            }
        commands.push_back(std::make_unique<AddTrackCommand>(rendered, source->id, main));
        for (auto& route : routes)
            commands.push_back(std::make_unique<AddRoutingConnectionCommand>(std::move(route)));
        for (const auto& lane : project.automationLanes)
        {
            if (lane.target.trackId != source->id || lane.target.type == AutomationTargetType::pluginParameter
                || lane.target.type == AutomationTargetType::deviceParameter
                || lane.target.type == AutomationTargetType::midiChannelPressure)
                continue;
            auto duplicate = lane;
            duplicate.id = juce::Uuid().toString();
            duplicate.target.trackId = rendered.id;
            if (const auto mapped = routeIds.find(lane.target.routeId); mapped != routeIds.end())
                duplicate.target.routeId = mapped->second;
            for (auto& point : duplicate.points)
                point.id = juce::Uuid().toString();
            commands.push_back(std::make_unique<AddAutomationLaneCommand>(std::move(duplicate)));
        }
        for (const auto& vca : project.tracks)
            if (vca.type == TrackType::vca && contains(vca.controlledTrackIds, source->id))
            {
                const auto before = TrackRoutingState::fromTrack(vca);
                auto after = before;
                after.controlledTrackIds.push_back(rendered.id);
                commands.push_back(std::make_unique<SetTrackRoutingStateCommand>(vca.id, before, after));
            }
        const auto before = TrackLifecycleState::fromTrack(*source);
        auto after = before;
        after.frozenTrackId = rendered.id;
        commands.push_back(std::make_unique<SetTrackLifecycleCommand>(source->id, before, after, "Freeze source"));
    }
    else
    {
        for (const auto& id : plan.clipIds)
        {
            const auto* audio = project.findClip(id);
            const auto* midi = project.findMidiClip(id);
            const auto* owner = audio != nullptr ? project.findTrackContainingClip(id) : project.findTrackContainingMidiClip(id);
            if (owner == nullptr || (audio == nullptr && midi == nullptr) || !project.isTrackActive(owner->id))
            {
                error = "A bounced source clip is no longer available.";
                return {};
            }
            if (plan.replaceSources)
            {
                if (audio != nullptr)
                    commands.push_back(std::make_unique<DeleteClipCommand>(id));
                else
                    commands.push_back(std::make_unique<DeleteMidiClipCommand>(id));
            }
            else if (audio != nullptr)
            {
                auto muted = *audio;
                muted.muted = true;
                commands.push_back(std::make_unique<SetClipStateCommand>(owner->id, *audio, muted, "Mute bounced source"));
            }
            else
            {
                auto muted = *midi;
                muted.muted = true;
                commands.push_back(std::make_unique<SetMidiClipStateCommand>(owner->id, *midi, muted, "Mute bounced source"));
            }
        }
        if (plan.destinationTrackId.isNotEmpty())
            commands.push_back(std::make_unique<AddClipCommand>(plan.destinationTrackId, clip));
        else
        {
            auto rendered = plan.renderedTrack;
            rendered.clips = { clip };
            commands.push_back(std::make_unique<AddTrackCommand>(std::move(rendered)));
        }
    }
    return std::make_unique<BatchProjectCommand>(
        plan.freezeSourceTrackId.isNotEmpty() ? "Freeze track" : plan.replaceSources ? "Bounce in place" : "Bounce to track",
        std::move(commands));
}

std::unique_ptr<ProjectCommand> ArrangementRender::unfreeze(
    const Project& project, const juce::String& id, juce::String& error)
{
    const auto* source = project.findTrack(project.frozenSourceTrackId(id));
    const auto* proxy = source != nullptr ? project.findTrack(source->frozenTrackId) : nullptr;
    if (source == nullptr || proxy == nullptr || proxy->freezeSourceTrackId != source->id)
    {
        error = "Select a frozen source or its rendered track to unfreeze.";
        return {};
    }
    const auto before = TrackLifecycleState::fromTrack(*source);
    auto after = before;
    after.frozenTrackId.clear();
    std::vector<std::unique_ptr<ProjectCommand>> commands;
    commands.push_back(std::make_unique<SetTrackLifecycleCommand>(source->id, before, after, "Restore frozen source"));
    const auto oldMix = TrackMixState::fromTrack(*source);
    auto restoredMix = oldMix;
    restoredMix.volumeDecibels = proxy->volumeDecibels;
    restoredMix.pan = proxy->pan;
    restoredMix.muted = proxy->muted;
    restoredMix.solo = proxy->solo;
    commands.push_back(std::make_unique<SetTrackMixCommand>(source->id, oldMix, restoredMix));
    commands.push_back(std::make_unique<RemoveTrackCommand>(proxy->id));
    return std::make_unique<BatchProjectCommand>("Unfreeze track", std::move(commands));
}

juce::Result ArrangementRender::applyClipEffects(
    const Project& context, const AudioClip& clip, const ClipEffectsSettings& settings,
    const juce::File& destination, double rate, AudioClip& result)
{
    if (!std::isfinite(settings.startSeconds) || !std::isfinite(settings.endSeconds)
        || !std::isfinite(settings.tailSeconds) || settings.startSeconds < 0.0
        || settings.endSeconds <= settings.startSeconds || settings.endSeconds > clip.durationSeconds
        || settings.tailSeconds < 0.0 || settings.tailSeconds > 30.0
        || !std::isfinite(rate) || rate <= 0.0 || settings.effects.inserts.empty())
        return juce::Result::fail("Choose an effect and a valid range inside the clip, with a tail between 0 and 30 seconds.");
    auto engine = std::make_unique<StudioAudioEngine>();
    Track dryTrack;
    dryTrack.clips = { clip };
    dryTrack.clips.front().muted = false;
    auto dryProject = isolatedProject(context, dryTrack);
    juce::AudioBuffer<float> dry;
    if (const auto rendered = engine->renderRangeToBuffer(dryProject, dry, rate,
            { clip.startSeconds, clip.endSeconds(), 0.0 }); rendered.failed())
        return rendered;
    juce::TemporaryFile dryFile(".wav");
    AudioClip dryClip;
    if (const auto written = writeRender(dry, dryFile.getFile(), rate, clip.startSeconds, clip.name, dryClip); written.failed())
        return written;
    const auto firstSample = static_cast<int>(std::llround(settings.startSeconds * rate));
    const auto endSample = static_cast<int>(std::llround(settings.endSeconds * rate));
    if (firstSample >= endSample || endSample > dry.getNumSamples())
        return juce::Result::fail("The selected clip range contains no complete samples at this sample rate.");
    auto effectsTrack = settings.effects;
    dryClip.startSeconds += static_cast<double>(firstSample) / rate;
    dryClip.sourceOffsetSeconds = static_cast<double>(firstSample) / rate;
    dryClip.durationSeconds = static_cast<double>(endSample - firstSample) / rate;
    effectsTrack.clips = { dryClip };
    auto wetProject = isolatedProject(context, effectsTrack);
    juce::AudioBuffer<float> wet;
    if (const auto rendered = engine->renderRangeToBuffer(wetProject, wet, rate,
            { dryClip.startSeconds, dryClip.endSeconds(), settings.tailSeconds }, settings.requests); rendered.failed())
        return rendered;
    juce::AudioBuffer<float> combined(2, std::max(dry.getNumSamples(), firstSample + wet.getNumSamples()));
    combined.clear();
    const auto selectedSamples = endSample - firstSample;
    const auto transition = std::min(32, selectedSamples / 2);
    for (int channel = 0; channel < 2; ++channel)
    {
        combined.copyFrom(channel, 0, dry, channel, 0, dry.getNumSamples());
        for (int sample = 0; sample < wet.getNumSamples(); ++sample)
        {
            const auto position = firstSample + sample;
            if (sample >= selectedSamples)
                combined.addSample(channel, position, wet.getSample(channel, sample));
            else
            {
                const auto blend = transition > 0
                    ? std::min({ 1.0f, static_cast<float>(sample) / static_cast<float>(transition),
                                 static_cast<float>(selectedSamples - 1 - sample) / static_cast<float>(transition) })
                    : 1.0f;
                const auto original = combined.getSample(channel, position);
                combined.setSample(channel, position, original + blend * (wet.getSample(channel, sample) - original));
            }
        }
    }
    const auto written = writeRender(combined, destination, rate, clip.startSeconds, clip.name + " FX", result);
    result.id = clip.id;
    result.colour = clip.colour;
    result.muted = clip.muted;
    return written;
}
}

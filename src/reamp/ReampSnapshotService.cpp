#include "ReampSnapshotService.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
juce::String hash(const juce::var& value)
{
    const auto json = juce::JSON::toString(value, false);
    return juce::SHA256(
        json.toRawUTF8(),
        static_cast<std::size_t>(json.getNumBytesAsUTF8()))
        .toHexString();
}

bool routeTouches(const RoutingConnection& route,
                  const juce::String& sourceTrackId,
                  const juce::String& returnTrackId)
{
    return route.sourceTrackId == sourceTrackId
        || route.sourceTrackId == returnTrackId
        || route.destination.trackId == sourceTrackId
        || route.destination.trackId == returnTrackId;
}

juce::Array<juce::var> clipState(const Project& project,
                                 const juce::String& trackId)
{
    juce::Array<juce::var> clips;
    for (const auto& candidate : project.tracks)
    {
        if (candidate.id != trackId
            && candidate.parentTrackId != trackId)
            continue;
        for (const auto& clip : candidate.clips)
        {
            auto clipValue = clip.toVar();
            if (auto* clipObject = clipValue.getDynamicObject())
            {
                clipObject->setProperty(
                    "mediaHash",
                    clip.sourceFile.existsAsFile()
                        ? juce::SHA256(clip.sourceFile).toHexString()
                        : juce::String("missing"));
            }
            clips.add(std::move(clipValue));
        }
    }
    return clips;
}

void addTempoStateIfNeeded(juce::DynamicObject& object,
                           const Project& project,
                           bool hasBeatAutomation)
{
    if (!hasBeatAutomation)
        return;

    object.setProperty("tempo", project.tempo);
    juce::Array<juce::var> tempoChanges;
    for (const auto& change : project.tempoChanges)
        tempoChanges.add(change.toVar());
    object.setProperty("tempoChanges", juce::var(tempoChanges));
}
}

std::optional<ToneSnapshot> ReampSnapshotService::capture(
    const Project& project,
    const juce::String& routeId,
    juce::String name,
    juce::String& error)
{
    const auto route = std::find_if(
        project.reampRoutes.cbegin(),
        project.reampRoutes.cend(),
        [&routeId](const auto& candidate)
        {
            return candidate.id == routeId;
        });
    if (route == project.reampRoutes.cend())
    {
        error = "The reamp route no longer exists.";
        return std::nullopt;
    }
    const auto* source = project.findTrack(route->sourceTrackId);
    const auto* returnTrack = project.findTrack(route->returnTrackId);
    if (source == nullptr || returnTrack == nullptr)
    {
        error = "The reamp source or return track is unavailable.";
        return std::nullopt;
    }

    ToneSnapshot snapshot;
    snapshot.name = std::move(name).trim();
    snapshot.reampRouteId = route->id;
    snapshot.sourceTrackId = source->id;
    snapshot.returnTrackId = returnTrack->id;
    snapshot.returnVolumeDecibels = returnTrack->volumeDecibels;
    snapshot.returnPan = returnTrack->pan;
    snapshot.returnPolarityInverted = returnTrack->polarityInverted;
    snapshot.inserts = returnTrack->inserts;
    for (const auto& connection : project.routingConnections)
        if (routeTouches(connection, source->id, returnTrack->id))
            snapshot.routes.push_back(connection);
    for (const auto& lane : project.automationLanes)
        if (lane.target.trackId == source->id
            || lane.target.trackId == returnTrack->id)
            snapshot.automation.push_back(lane);
    snapshot.sourceFingerprint = sourceFingerprint(project, source->id);
    snapshot.chainFingerprint = chainFingerprint(project, route->id);
    if (snapshot.name.isEmpty()
        || snapshot.sourceFingerprint.isEmpty()
        || snapshot.chainFingerprint.isEmpty())
    {
        error = "The tone snapshot could not be fingerprinted.";
        return std::nullopt;
    }
    return snapshot;
}

juce::String ReampSnapshotService::staleReason(
    const Project& project,
    const ToneSnapshot& snapshot)
{
    if (sourceFingerprint(project, snapshot.sourceTrackId)
        != snapshot.sourceFingerprint)
        return "DI source changed";

    const auto route = std::find_if(
        project.reampRoutes.cbegin(),
        project.reampRoutes.cend(),
        [&snapshot](const auto& candidate)
        {
            return candidate.id == snapshot.reampRouteId;
        });
    const auto* returnTrack = project.findTrack(
        snapshot.returnTrackId);
    const auto snapshotVolumeLane = std::find_if(
        snapshot.automation.cbegin(),
        snapshot.automation.cend(),
        [&snapshot](const auto& lane)
        {
            return lane.enabled
                && lane.target.trackId == snapshot.returnTrackId
                && lane.target.type
                    == AutomationTargetType::trackVolume;
        });
    const auto automatedReturnVolume =
        snapshotVolumeLane != snapshot.automation.cend();
    const auto matchedLevel = juce::jlimit(
        -60.0f,
        12.0f,
        snapshot.returnVolumeDecibels
            + snapshot.comparisonGainDecibels);
    if (route != project.reampRoutes.cend()
        && route->activeSnapshotId == snapshot.id
        && returnTrack != nullptr)
    {
        if (automatedReturnVolume
            && std::abs(
                   returnTrack->volumeDecibels
                   - snapshot.returnVolumeDecibels)
                < 0.0001f)
        {
            const auto activeLane = std::find_if(
                project.automationLanes.cbegin(),
                project.automationLanes.cend(),
                [snapshotVolumeLane](const auto& lane)
                {
                    return lane.id == snapshotVolumeLane->id;
                });
            const auto matchedTrim =
                snapshotVolumeLane->trimOffset
                + snapshot.comparisonGainDecibels / 72.0;
            if (activeLane != project.automationLanes.cend()
                && std::abs(activeLane->trimOffset - matchedTrim)
                    < 0.0001)
            {
                auto comparisonProject = project;
                const auto normalizedLane = std::find_if(
                    comparisonProject.automationLanes.begin(),
                    comparisonProject.automationLanes.end(),
                    [snapshotVolumeLane](const auto& lane)
                    {
                        return lane.id == snapshotVolumeLane->id;
                    });
                normalizedLane->trimOffset =
                    snapshotVolumeLane->trimOffset;
                if (chainFingerprint(
                        comparisonProject,
                        snapshot.reampRouteId)
                    != snapshot.chainFingerprint)
                    return "Tone chain changed";
                return {};
            }
        }
        else if (!automatedReturnVolume
                 && std::abs(
                        returnTrack->volumeDecibels
                        - matchedLevel)
                     < 0.0001f)
        {
            auto comparisonProject = project;
            comparisonProject.findTrack(snapshot.returnTrackId)
                ->volumeDecibels =
                snapshot.returnVolumeDecibels;
            if (chainFingerprint(
                    comparisonProject,
                    snapshot.reampRouteId)
                != snapshot.chainFingerprint)
                return "Tone chain changed";
            return {};
        }
    }
    if (chainFingerprint(project, snapshot.reampRouteId)
        != snapshot.chainFingerprint)
        return "Tone chain changed";
    return {};
}

juce::String ReampSnapshotService::sourceFingerprint(
    const Project& project,
    const juce::String& trackId)
{
    const auto* track = project.findTrack(trackId);
    if (track == nullptr)
        return {};
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("trackId", track->id);
    object->setProperty("activeTakeTrackId", track->activeTakeTrackId);
    object->setProperty("versionsCollapsed", track->versionsCollapsed);
    object->setProperty("volumeDecibels", track->volumeDecibels);
    object->setProperty("pan", track->pan);
    object->setProperty("polarity", track->polarityInverted);
    object->setProperty("muted", track->muted);
    object->setProperty(
        "channelLayout",
        channelLayoutToString(track->channelLayout));
    juce::Array<juce::var> inserts;
    for (const auto& insert : track->inserts)
        inserts.add(insert.toVar());
    object->setProperty("inserts", juce::var(inserts));
    object->setProperty("clips", juce::var(clipState(project, trackId)));
    juce::Array<juce::var> comps;
    for (const auto& region : track->compRegions)
        comps.add(region.toVar());
    object->setProperty("compRegions", juce::var(comps));
    juce::Array<juce::var> automation;
    auto hasBeatAutomation = false;
    for (const auto& lane : project.automationLanes)
    {
        if (lane.target.trackId != trackId
            || lane.target.routeId.isNotEmpty())
            continue;
        automation.add(lane.toVar());
        hasBeatAutomation =
            hasBeatAutomation
            || lane.timebase == AutomationTimebase::beats;
    }
    object->setProperty("automation", juce::var(automation));
    addTempoStateIfNeeded(*object, project, hasBeatAutomation);
    return hash(juce::var(object.release()));
}

juce::String ReampSnapshotService::chainFingerprint(
    const Project& project,
    const juce::String& routeId)
{
    const auto route = std::find_if(
        project.reampRoutes.cbegin(),
        project.reampRoutes.cend(),
        [&routeId](const auto& candidate)
        {
            return candidate.id == routeId;
        });
    if (route == project.reampRoutes.cend())
        return {};
    const auto* returnTrack = project.findTrack(route->returnTrackId);
    if (returnTrack == nullptr)
        return {};
    auto object = std::make_unique<juce::DynamicObject>();
    auto routeValue = route->toVar();
    if (auto* routeObject = routeValue.getDynamicObject())
        routeObject->removeProperty("activeSnapshotId");
    object->setProperty("route", std::move(routeValue));
    object->setProperty("outputTrackId", returnTrack->outputTrackId);
    object->setProperty(
        "activeTakeTrackId",
        returnTrack->activeTakeTrackId);
    object->setProperty(
        "versionsCollapsed",
        returnTrack->versionsCollapsed);
    object->setProperty("volumeDecibels", returnTrack->volumeDecibels);
    object->setProperty("pan", returnTrack->pan);
    object->setProperty("polarity", returnTrack->polarityInverted);
    object->setProperty("muted", returnTrack->muted);
    object->setProperty(
        "channelLayout",
        channelLayoutToString(returnTrack->channelLayout));
    juce::Array<juce::var> inserts;
    for (const auto& insert : returnTrack->inserts)
        inserts.add(insert.toVar());
    object->setProperty("inserts", juce::var(inserts));
    object->setProperty(
        "clips",
        juce::var(clipState(project, returnTrack->id)));
    juce::Array<juce::var> comps;
    for (const auto& region : returnTrack->compRegions)
        comps.add(region.toVar());
    object->setProperty("compRegions", juce::var(comps));
    juce::Array<juce::var> routes;
    juce::StringArray routeIds;
    for (const auto& connection : project.routingConnections)
        if (routeTouches(
                connection,
                route->sourceTrackId,
                route->returnTrackId))
        {
            routes.add(connection.toVar());
            routeIds.add(connection.id);
        }
    object->setProperty("routes", juce::var(routes));
    juce::Array<juce::var> automation;
    auto hasBeatAutomation = false;
    for (const auto& lane : project.automationLanes)
    {
        if (lane.target.trackId == route->returnTrackId
            || (lane.target.routeId.isNotEmpty()
                && routeIds.contains(lane.target.routeId)))
        {
            automation.add(lane.toVar());
            hasBeatAutomation =
                hasBeatAutomation
                || lane.timebase == AutomationTimebase::beats;
        }
    }
    object->setProperty("automation", juce::var(automation));
    addTempoStateIfNeeded(*object, project, hasBeatAutomation);
    return hash(juce::var(object.release()));
}

std::optional<MixerSnapshot> MixerSnapshotService::capture(
    const Project& project,
    const std::vector<juce::String>& trackIds,
    juce::String name,
    juce::String& error)
{
    MixerSnapshot snapshot;
    snapshot.name = std::move(name).trim();
    for (const auto& trackId : trackIds)
    {
        const auto* track = project.findTrack(trackId);
        if (track == nullptr)
        {
            error = "A mixer snapshot track is unavailable.";
            return std::nullopt;
        }
        snapshot.tracks.push_back({
            track->id,
            track->volumeDecibels,
            track->pan,
            track->muted,
            track->solo,
            track->soloSafe,
            track->polarityInverted,
            track->channelLayout,
            track->folderTrackId,
            track->controlledTrackIds,
            track->inserts
        });
    }
    for (const auto& route : project.routingConnections)
        if (std::find(trackIds.cbegin(),
                      trackIds.cend(),
                      route.sourceTrackId) != trackIds.cend()
            || std::find(trackIds.cbegin(),
                         trackIds.cend(),
                         route.destination.trackId) != trackIds.cend())
            snapshot.routes.push_back(route);
    for (const auto& lane : project.automationLanes)
        if (std::find(trackIds.cbegin(),
                      trackIds.cend(),
                      lane.target.trackId) != trackIds.cend())
            snapshot.automation.push_back(lane);
    if (snapshot.name.isEmpty() || snapshot.tracks.empty())
    {
        error = "Mixer snapshots require a name and at least one track.";
        return std::nullopt;
    }
    return snapshot;
}
}

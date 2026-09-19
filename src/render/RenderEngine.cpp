#include "RenderEngine.h"

#include "model/ProjectCommands.h"
#include "reamp/ReampSnapshotService.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace studio
{
namespace
{
double gatedRms(const juce::AudioBuffer<float>& audio)
{
    auto sum = 0.0;
    auto count = std::int64_t { 0 };
    constexpr auto gate = 0.001f;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto value = audio.getSample(channel, sample);
            if (std::abs(value) < gate)
                continue;
            sum += static_cast<double>(value) * value;
            ++count;
        }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

juce::String safeName(const ToneSnapshot& snapshot)
{
    auto value = snapshot.name.retainCharacters(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ ");
    value = value.trim().replaceCharacter(' ', '-');
    return value.isNotEmpty() ? value : snapshot.id;
}

void writeReport(const RenderReport& report, const juce::File& destination)
{
    destination.replaceWithText(
        juce::JSON::toString(report.toVar(), true),
        false,
        false,
        "\n");
}
}

std::optional<StudioAudioEngine::RenderRange> RenderEngine::resolveRange(
    const Project& project,
    const MixExportSettings& settings,
    juce::String& error)
{
    error.clear();
    if (const auto validation = AudioExport::validate(settings.audio); validation.failed())
    {
        error = validation.getErrorMessage();
        return std::nullopt;
    }
    StudioAudioEngine::RenderRange range;
    range.tailSeconds = settings.tailSeconds;
    switch (settings.range)
    {
        case MixExportRange::entireProject:
            range.endSeconds = project.lengthSeconds();
            break;
        case MixExportRange::loop:
            range.startSeconds = project.loopStartSeconds;
            range.endSeconds = project.loopEndSeconds;
            break;
        case MixExportRange::markers:
        {
            const auto findMarker = [&project](const juce::String& id)
            {
                return std::find_if(
                    project.sections.cbegin(), project.sections.cend(),
                    [&id](const auto& marker) { return marker.id == id; });
            };
            const auto start = findMarker(settings.startMarkerId);
            const auto end = findMarker(settings.endMarkerId);
            if (start == project.sections.cend() || end == project.sections.cend())
            {
                error = "Choose existing start and end markers. A selected marker is missing.";
                return std::nullopt;
            }
            range.startSeconds = start->timeSeconds;
            range.endSeconds = end->timeSeconds;
            break;
        }
        case MixExportRange::custom:
            range.startSeconds = settings.startSeconds;
            range.endSeconds = settings.endSeconds;
            break;
        default:
            error = "Choose a supported export range.";
            return std::nullopt;
    }
    if (!std::isfinite(range.startSeconds)
        || !std::isfinite(range.endSeconds)
        || range.startSeconds < 0.0
        || range.endSeconds <= range.startSeconds)
    {
        error = "The export end must be after its start, with finite, non-negative positions.";
        return std::nullopt;
    }
    if (range.tailSeconds.has_value()
        && (!std::isfinite(*range.tailSeconds)
            || *range.tailSeconds < 0.0 || *range.tailSeconds > 30.0))
    {
        error = "Custom effects tails must be between 0 and 30 seconds.";
        return std::nullopt;
    }
    if (!std::isfinite(settings.fadeInSeconds)
        || !std::isfinite(settings.fadeOutSeconds)
        || settings.fadeInSeconds < 0.0 || settings.fadeInSeconds > 30.0
        || settings.fadeOutSeconds < 0.0 || settings.fadeOutSeconds > 30.0)
    {
        error = "Fades must be between 0 and 30 seconds.";
        return std::nullopt;
    }
    const auto endSamples = (range.endSeconds + range.tailSeconds.value_or(0.0))
        * settings.audio.sampleRate;
    if (endSamples > static_cast<double>(std::numeric_limits<int>::max()))
    {
        error = "The selected export range is too long for a memory buffer.";
        return std::nullopt;
    }
    const auto selectedSamples = std::llround(range.endSeconds * settings.audio.sampleRate)
        - std::llround(range.startSeconds * settings.audio.sampleRate);
    if (selectedSamples <= 0)
    {
        error = "The selected export range contains no samples at this sample rate.";
        return std::nullopt;
    }
    const auto outputSamples = selectedSamples
        + std::llround(range.tailSeconds.value_or(0.0) * settings.audio.sampleRate);
    if (std::llround(settings.fadeInSeconds * settings.audio.sampleRate) > outputSamples
        || std::llround(settings.fadeOutSeconds * settings.audio.sampleRate) > outputSamples)
    {
        error = "Fades must fit within the sample-rounded export range.";
        return std::nullopt;
    }
    return range;
}

juce::Result RenderEngine::exportMix(
    StudioAudioEngine& engine,
    const Project& project,
    const juce::File& destination,
    const MixExportSettings& settings,
    std::vector<StudioAudioEngine::PluginRuntimeRequest> pluginRequests)
{
    juce::String error;
    const auto range = resolveRange(project, settings, error);
    if (!range.has_value())
        return juce::Result::fail(error);
    juce::AudioBuffer<float> audio;
    if (const auto result = engine.renderRangeToBuffer(
            project, audio, settings.audio.sampleRate, *range, std::move(pluginRequests));
        result.failed())
        return result;

    const auto fadeInSamples = static_cast<int>(
        std::llround(settings.fadeInSeconds * settings.audio.sampleRate));
    const auto fadeOutSamples = static_cast<int>(
        std::llround(settings.fadeOutSeconds * settings.audio.sampleRate));
    if (fadeInSamples > 0 || fadeOutSamples > 0)
    {
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto fadeIn = fadeInSamples > 0
                ? std::min(1.0f, static_cast<float>(sample) / static_cast<float>(std::max(1, fadeInSamples - 1)))
                : 1.0f;
            const auto fadeOut = fadeOutSamples > 0
                ? std::min(1.0f, static_cast<float>(audio.getNumSamples() - 1 - sample)
                                    / static_cast<float>(std::max(1, fadeOutSamples - 1)))
                : 1.0f;
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                audio.setSample(channel, sample, audio.getSample(channel, sample) * fadeIn * fadeOut);
        }
    }
    juce::StringPairArray metadata;
    metadata.set("title", project.name);
    metadata.set("artist", project.metadata.artist);
    metadata.set("album", project.metadata.album);
    metadata.set("genre", project.metadata.genre);
    metadata.set("date", project.metadata.year);
    metadata.set("copyright", project.metadata.copyright);
    metadata.set("comment", project.metadata.comment);
    return AudioExport::write(audio, destination, settings.audio, metadata);
}

double RenderEngine::levelMatchGainDecibels(
    const juce::AudioBuffer<float>& reference,
    const juce::AudioBuffer<float>& candidate)
{
    const auto referenceRms = gatedRms(reference);
    const auto candidateRms = gatedRms(candidate);
    if (referenceRms <= 0.0 || candidateRms <= 0.0)
        return 0.0;
    return juce::jlimit(
        -24.0,
        24.0,
        20.0 * std::log10(referenceRms / candidateRms));
}

std::optional<double> RenderEngine::levelMatchGainDecibels(
    const juce::File& reference,
    const juce::File& candidate,
    juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto referenceReader = std::unique_ptr<juce::AudioFormatReader>(
        formats.createReaderFor(reference));
    auto candidateReader = std::unique_ptr<juce::AudioFormatReader>(
        formats.createReaderFor(candidate));
    if (referenceReader == nullptr || candidateReader == nullptr)
    {
        error = "Could not read the tone renders for level matching.";
        return std::nullopt;
    }
    const auto length = std::min(
        referenceReader->lengthInSamples,
        candidateReader->lengthInSamples);
    if (length <= 0
        || length > static_cast<std::int64_t>(
            std::numeric_limits<int>::max()))
    {
        error = "Tone renders have no comparable audio.";
        return std::nullopt;
    }
    const auto samples = static_cast<int>(length);
    juce::AudioBuffer<float> referenceAudio(2, samples);
    juce::AudioBuffer<float> candidateAudio(2, samples);
    if (!referenceReader->read(
            &referenceAudio,
            0,
            samples,
            0,
            true,
            true)
        || !candidateReader->read(
            &candidateAudio,
            0,
            samples,
            0,
            true,
            true))
    {
        error = "Could not decode the tone renders for level matching.";
        return std::nullopt;
    }
    return levelMatchGainDecibels(referenceAudio, candidateAudio);
}

std::vector<RenderReport> RenderEngine::batchToneSnapshots(
    StudioAudioEngine& engine,
    const Project& project,
    const std::vector<ToneSnapshot>& snapshots,
    const juce::File& outputDirectory,
    const RequestBuilder& requestBuilder)
{
    std::vector<RenderReport> reports;
    reports.reserve(snapshots.size());
    outputDirectory.createDirectory();
    for (const auto& snapshot : snapshots)
    {
        RenderReport report;
        report.scope = "reamp:" + snapshot.id;
        report.sourceHash = snapshot.sourceFingerprint;
        report.chainHash = snapshot.chainFingerprint;
        report.createdAt = juce::Time::getCurrentTime().toISO8601(true);
        report.durationSeconds = project.lengthSeconds();
        report.warning = ReampSnapshotService::staleReason(
            project,
            snapshot);

        auto renderProject = project;
        RecallToneSnapshotCommand recall(snapshot);
        juce::String error;
        if (!recall.perform(renderProject, error))
        {
            report.status = "failed";
            report.error = error;
            reports.push_back(std::move(report));
            continue;
        }
        auto* returnTrack = renderProject.findTrack(snapshot.returnTrackId);
        if (returnTrack == nullptr)
        {
            report.status = "failed";
            report.error = "The snapshot return track is unavailable.";
            reports.push_back(std::move(report));
            continue;
        }
        for (auto& track : renderProject.tracks)
            if (track.parentTrackId.isEmpty()
                && track.type != TrackType::master)
                track.solo = track.id == returnTrack->id;

        const auto output = outputDirectory.getChildFile(
            safeName(snapshot) + "-" + snapshot.id.substring(0, 8) + ".wav");
        const auto requests = requestBuilder(renderProject);
        report.mode = std::any_of(
            requests.cbegin(),
            requests.cend(),
            [](const auto& request)
            {
                return !request.bypassed
                    && !request.missing
                    && request.deviceIdentifier.isEmpty()
                    && request.bridgeMode == PluginBridgeMode::sandboxed;
            })
            ? "real-time"
            : "offline";
        const auto result = engine.renderToWav(
            renderProject,
            output,
            48000.0,
            requests);
        report.outputFile = output.getFullPathName();
        if (result.failed())
        {
            report.status = "failed";
            report.error = result.getErrorMessage();
        }
        else
        {
            report.status = "success";
            report.outputHash = juce::SHA256(output).toHexString();
        }
        writeReport(
            report,
            output.getSiblingFile(
                output.getFileNameWithoutExtension() + ".report.json"));
        reports.push_back(std::move(report));
    }
    return reports;
}
}

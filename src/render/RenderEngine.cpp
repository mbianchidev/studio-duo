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
    auto referenceReader = formats.createReaderFor(reference);
    auto candidateReader = formats.createReaderFor(candidate);
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

#include "TestSuites.h"

#include "mastering/MasteringEngine.h"
#include "mastering/MasteringReleaseService.h"
#include "platform/Architecture.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>

std::optional<int> runMasteringMeasurement(const juce::StringArray& arguments)
{
    if (!std::any_of(arguments.begin(), arguments.end(), [](const auto& argument)
        { return argument.startsWith("--measure-mastering") || argument.startsWith("--measurement-output"); }))
        return std::nullopt;

    const auto fail = [](const juce::String& message) -> std::optional<int>
    {
        std::cerr << "Mastering measurement failed: " << message << '\n';
        return 1;
    };
    juce::String sourceName, outputName;
    for (const auto& argument : arguments)
    {
        if (argument.startsWith("--measure-mastering=") && sourceName.isEmpty())
            sourceName = argument.fromFirstOccurrenceOf("=", false, false);
        else if (argument.startsWith("--measurement-output=") && outputName.isEmpty())
            outputName = argument.fromFirstOccurrenceOf("=", false, false);
        else
            return fail("Unknown or duplicate measurement argument.");
    }
    if (!juce::File::isAbsolutePath(sourceName) || !juce::File::isAbsolutePath(outputName))
        return fail("An absolute source file and new output directory are required.");
    const juce::File source(sourceName), output(outputName);
    if (!source.existsAsFile() || output.exists())
        return fail("Use an existing source and a new output directory; nothing is overwritten.");

    const auto sourceHash = juce::SHA256(source).toHexString();
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if (reader == nullptr || !std::isfinite(reader->sampleRate)
        || reader->sampleRate < 8000.0 || reader->sampleRate > 192000.0
        || reader->numChannels < 1 || reader->numChannels > 2
        || reader->lengthInSamples <= 0 || reader->lengthInSamples > 60 * 192000
        || reader->lengthInSamples > std::numeric_limits<int>::max())
        return fail("The source must be bounded, readable mono/stereo audio at 8-192 kHz.");

    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels),
                                  static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&audio, 0, audio.getNumSamples(), 0, true, true))
        return fail("The complete source could not be decoded.");
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            if (!std::isfinite(audio.getSample(channel, sample)))
                return fail("Nonfinite source samples cannot be measured.");
    if (juce::SHA256(source).toHexString() != sourceHash)
        return fail("The source changed during measurement.");
    const auto measurement = studio::MasteringEngine::analyse(audio, reader->sampleRate);
    if ((measurement.integratedLoudnessLufs && !std::isfinite(*measurement.integratedLoudnessLufs))
        || (measurement.loudnessRangeLu && !std::isfinite(*measurement.loudnessRangeLu))
        || !std::isfinite(measurement.truePeakDbtp))
        return fail("Analysis produced nonfinite measurements.");
    const auto silent = audio.getMagnitude(0, audio.getNumSamples()) == 0.0f;

    auto values = std::make_unique<juce::DynamicObject>();
    values->setProperty("lufs", measurement.integratedLoudnessLufs
        ? juce::var(*measurement.integratedLoudnessLufs) : juce::var());
    values->setProperty("lra", measurement.loudnessRangeLu && !silent
        ? juce::var(*measurement.loudnessRangeLu) : juce::var());
    values->setProperty("dbtp", silent ? juce::var() : juce::var(measurement.truePeakDbtp));
    auto report = std::make_unique<juce::DynamicObject>();
    report->setProperty("schemaVersion", 1);
    report->setProperty("candidateSha", STUDIO_DUO_BUILD_COMMIT);
    report->setProperty("buildRevision", STUDIO_DUO_BUILD_REVISION);
    report->setProperty("applicationVersion", STUDIO_DUO_VERSION);
    report->setProperty("os", juce::SystemStats::getOperatingSystemName());
    report->setProperty("architecture", studio::currentArchitecture());
    report->setProperty("sourceSha256", sourceHash);
    report->setProperty("sampleRateHz", reader->sampleRate);
    report->setProperty("channels", audio.getNumChannels());
    report->setProperty("frames", audio.getNumSamples());
    report->setProperty("measurements", juce::var(values.release()));
    const auto created = output.createDirectory();
    if (created.failed())
        return fail("Could not create measurement output: " + created.getErrorMessage());
    if (!output.getChildFile("report.json").replaceWithText(
            juce::JSON::toString(juce::var(report.release()), true), false, false, "\n"))
        return fail("Could not write the measurement report.");
    std::cout << "Measured complete mono/stereo source without opening audio or MIDI devices.\n";
    return 0;
}

std::optional<int> runMasteringDeliveryFixture(const juce::StringArray& arguments)
{
    if (!std::any_of(arguments.begin(), arguments.end(), [](const auto& argument)
        { return argument.startsWith("--export-mastering") || argument.startsWith("--delivery-output"); }))
        return std::nullopt;
    const auto fail = [](const juce::String& message) -> std::optional<int>
    {
        std::cerr << "Mastering delivery fixture failed: " << message << '\n';
        return 1;
    };
    juce::String fixtureName, outputName;
    for (const auto& argument : arguments)
    {
        if (argument.startsWith("--export-mastering=") && fixtureName.isEmpty())
            fixtureName = argument.fromFirstOccurrenceOf("=", false, false);
        else if (argument.startsWith("--delivery-output=") && outputName.isEmpty())
            outputName = argument.fromFirstOccurrenceOf("=", false, false);
        else
            return fail("Unknown or duplicate delivery argument.");
    }
    if (!juce::File::isAbsolutePath(fixtureName) || !juce::File::isAbsolutePath(outputName))
        return fail("An absolute fixture file and new output directory are required.");
    const juce::File fixture(fixtureName), output(outputName);
    if (!fixture.existsAsFile() || fixture.getSize() > 128 * 1024 || output.exists())
        return fail("Use a bounded existing fixture and a new output directory; nothing is overwritten.");
    const auto fixtureHash = juce::SHA256(fixture).toHexString();
    const auto request = juce::JSON::parse(fixture.loadFileAsString());
    if (!request.isObject() || !request["schemaVersion"].isInt() || static_cast<int>(request["schemaVersion"]) != 1)
        return fail("The delivery fixture requires schema version 1.");
    auto albumValue = request["album"];
    auto* tracks = albumValue["tracks"].getArray();
    auto* references = albumValue["references"].getArray();
    const auto* exports = request["exports"].getArray();
    if (tracks == nullptr || tracks->isEmpty() || tracks->size() > 4
        || references == nullptr || references->size() > 2
        || exports == nullptr || exports->isEmpty() || exports->size() > 32)
        return fail("The fixture requires bounded album sources and export cases.");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto resolveSource = [&](juce::var& value)
    {
        const auto name = value["file"].toString();
        if (!value.isObject() || name.isEmpty() || name.containsAnyOf("/\\:")
            || name == "." || name == "..")
            return false;
        const auto file = fixture.getParentDirectory().getChildFile(name);
        const auto hash = value["sourceHash"].toString();
        if (!file.existsAsFile() || file.getSize() > 32 * 1024 * 1024
            || hash.length() != 64 || !hash.containsOnly("0123456789abcdef")
            || juce::SHA256(file).toHexString() != hash)
            return false;
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr || !std::isfinite(reader->sampleRate)
            || reader->sampleRate < 8000 || reader->sampleRate > 192000
            || reader->numChannels < 1 || reader->numChannels > 2
            || reader->lengthInSamples <= 0
            || reader->lengthInSamples > static_cast<juce::int64>(reader->sampleRate * 8))
            return false;
        value.getDynamicObject()->setProperty("file", file.getFullPathName());
        return true;
    };
    for (auto& track : *tracks)
    {
        auto* sources = track["sources"].getArray();
        if (sources == nullptr || sources->isEmpty() || sources->size() > 2)
            return fail("Each fixture song requires one or two sources.");
        for (auto& source : *sources)
            if (!resolveSource(source))
                return fail("A fixture source is unsafe, changed, unreadable or outside its bounds.");
    }
    for (auto& reference : *references)
        if (!resolveSource(reference))
            return fail("A fixture reference is unsafe, changed, unreadable or outside its bounds.");
    juce::String error;
    const auto album = studio::MasteringAlbum::fromVar(albumValue, error);
    if (!album || album->durationSeconds() > 60)
        return fail("Invalid or oversized fixture album: " + error);
    if (output.createDirectory().failed())
        return fail("Could not create the delivery output directory.");

    struct PrivateFiles
    {
        juce::File signing;
        std::vector<juce::File> reports;
        bool remove() const
        {
            auto ok = true;
            for (const auto& file : reports)
                if (file.exists() && !file.deleteFile())
                {
                    std::cerr << "Could not remove a private delivery signing sidecar.\n";
                    ok = false;
                }
            if (signing.exists() && !signing.deleteRecursively())
            {
                std::cerr << "Could not remove the disposable delivery signing key.\n";
                ok = false;
            }
            return ok;
        }
        ~PrivateFiles() { remove(); }
    } privateFiles { output.getChildFile("private-signing"), {} };
    juce::Array<juce::var> results;
    juce::StringArray ids;
    for (const auto& item : *exports)
    {
        const auto id = item["id"].toString();
        if (!item.isObject() || id.isEmpty() || id.length() > 100
            || !id.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789-") || ids.contains(id))
            return fail("Delivery case IDs must be unique safe leaf names.");
        ids.add(id);
        const auto formatName = item["format"].toString();
        const std::array<std::pair<const char*, studio::AudioExportFormat>, 5> formatsByName {{
            { "wav", studio::AudioExportFormat::wav }, { "aiff", studio::AudioExportFormat::aiff },
            { "flac", studio::AudioExportFormat::flac }, { "ogg", studio::AudioExportFormat::oggVorbis },
            { "mp3", studio::AudioExportFormat::mp3 }
        }};
        const auto format = std::find_if(formatsByName.begin(), formatsByName.end(),
            [&](const auto& entry) { return formatName == entry.first; });
        const auto dither = item["dither"].toString(), mode = item["mp3BitrateMode"].toString();
        if (format == formatsByName.end() || !item["sampleRate"].isInt()
            || !item["bitDepth"].isInt() || !item["channels"].isInt()
            || (dither != "none" && dither != "tpdf") || (mode != "constant" && mode != "variable"))
            return fail("Invalid delivery case settings: " + id);
        studio::MasteringExportSettings settings;
        settings.format = format->second;
        settings.sampleRate = static_cast<double>(item["sampleRate"]);
        settings.bitDepth = static_cast<int>(item["bitDepth"]);
        settings.channels = static_cast<int>(item["channels"]);
        settings.dither = dither == "tpdf" ? studio::AudioExportDither::tpdf : studio::AudioExportDither::none;
        settings.mp3BitrateMode = mode == "variable" ? studio::Mp3BitrateMode::variable
                                                    : studio::Mp3BitrateMode::constant;
        settings.distributionPresetId = "streaming-balanced";
        if (const auto validation = studio::AudioExport::validate(settings); validation.failed())
            return fail(id + ": " + validation.getErrorMessage());
        auto result = std::make_unique<juce::DynamicObject>();
        result->setProperty("id", id);
        result->setProperty("settings", settings.toVar());
        const auto caseDirectory = output.getChildFile(id);
        for (const auto* pass : { "master", "repeat" })
        {
            const auto destination = caseDirectory.getChildFile(juce::String(pass) + "." + formatName);
            const auto sidecar = caseDirectory.getChildFile(juce::String(pass) + ".report.json");
            privateFiles.reports.push_back(sidecar);
            if (!studio::MasteringReleaseService::exportMaster(*album, destination, settings, privateFiles.signing, error))
                return fail(id + ": " + error);
            const auto signedReport = studio::RenderReport::fromVar(juce::JSON::parse(sidecar.loadFileAsString()), error);
            if (!signedReport || !studio::MasteringReleaseService::verifyReport(*signedReport, privateFiles.signing, error)
                || signedReport->outputHash != juce::SHA256(destination).toHexString())
                return fail(id + ": signed final-file report verification failed: " + error);
            auto reader = studio::AudioExport::createVerificationReader(destination, settings.format, error);
            if (reader == nullptr || reader->numChannels != static_cast<unsigned int>(settings.channels)
                || reader->lengthInSamples <= 0 || reader->lengthInSamples > 60 * 192000)
                return fail(id + ": completed-file decode is invalid: " + error);
            juce::AudioBuffer<float> decoded(settings.channels, static_cast<int>(reader->lengthInSamples));
            if (!reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, settings.channels == 2))
                return fail(id + ": could not decode the complete verification programme.");
            std::vector<juce::uint32> interleaved;
            interleaved.reserve(static_cast<std::size_t>(decoded.getNumSamples()) * static_cast<std::size_t>(settings.channels));
            for (int sample = 0; sample < decoded.getNumSamples(); ++sample)
                for (int channel = 0; channel < settings.channels; ++channel)
                    interleaved.push_back(juce::ByteOrder::swapIfBigEndian(
                        std::bit_cast<juce::uint32>(decoded.getSample(channel, sample))));
            const auto decodedFile = caseDirectory.getChildFile(juce::String(pass) + ".native.f32le");
            if (!decodedFile.replaceWithData(interleaved.data(), interleaved.size() * sizeof(juce::uint32)))
                return fail(id + ": could not retain native verification PCM.");
            auto snapshot = signedReport->toVar();
            for (const auto* field : { "id", "outputFile", "createdAt", "signingPublicKey", "signature" })
                snapshot.getDynamicObject()->removeProperty(field);
            auto measurement = std::make_unique<juce::DynamicObject>();
            measurement->setProperty("file", destination.getFileName());
            measurement->setProperty("nativeDecodedFile", decodedFile.getFileName());
            measurement->setProperty("nativeDecodedFrames", decoded.getNumSamples());
            measurement->setProperty("nativeDecodedChannels", settings.channels);
            measurement->setProperty("report", snapshot);
            measurement->setProperty("signedReportSha256", juce::SHA256(sidecar).toHexString());
            measurement->setProperty("signatureVerifiedBeforeRedaction", true);
            result->setProperty(pass, juce::var(measurement.release()));
        }
        results.add(juce::var(result.release()));
        std::cout << id << ": exported twice and verified signed reports.\n";
    }
    if (juce::SHA256(fixture).toHexString() != fixtureHash || !privateFiles.remove())
        return fail("The fixture changed or private signing material could not be removed.");
    auto report = std::make_unique<juce::DynamicObject>();
    report->setProperty("schemaVersion", 1);
    report->setProperty("candidateSha", STUDIO_DUO_BUILD_COMMIT);
    report->setProperty("buildRevision", STUDIO_DUO_BUILD_REVISION);
    report->setProperty("applicationVersion", STUDIO_DUO_VERSION);
    report->setProperty("os", juce::SystemStats::getOperatingSystemName());
    report->setProperty("architecture", studio::currentArchitecture());
    report->setProperty("fixtureSha256", fixtureHash);
    report->setProperty("reportRedaction", "Product signatures verified before removing local paths, IDs, times and keys; snapshots are not signed originals.");
    report->setProperty("exports", juce::var(results));
    if (!output.getChildFile("report.json").replaceWithText(
            juce::JSON::toString(juce::var(report.release()), true), false, false, "\n"))
        return fail("Could not retain the native delivery measurements.");
    return 0;
}

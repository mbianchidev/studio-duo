#include "TestSuites.h"

#include "mastering/MasteringEngine.h"
#include "platform/Architecture.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
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

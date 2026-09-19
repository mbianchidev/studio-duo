#include "MasteringEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>
#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace studio
{
namespace
{
struct Biquad
{
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;

    double process(double input) noexcept
    {
        const auto output = b0 * input
            + b1 * x1
            + b2 * x2
            - a1 * y1
            - a2 * y2;
        x2 = x1;
        x1 = input;
        y2 = y1;
        y1 = output;
        return output;
    }
};

Biquad highShelf(double sampleRate)
{
    constexpr auto frequency = 1681.974450955533;
    constexpr auto quality = 0.7071752369554196;
    constexpr auto gainDecibels = 3.999843853973347;
    constexpr auto shelfExponent = 0.4996667741545416;
    const auto k = std::tan(
        juce::MathConstants<double>::pi * frequency / sampleRate);
    const auto highGain = std::pow(10.0, gainDecibels / 20.0);
    const auto bandGain = std::pow(highGain, shelfExponent);
    const auto a0 = 1.0 + k / quality + k * k;

    Biquad filter;
    filter.b0 = (highGain + bandGain * k / quality + k * k) / a0;
    filter.b1 = 2.0 * (k * k - highGain) / a0;
    filter.b2 = (highGain - bandGain * k / quality + k * k) / a0;
    filter.a1 = 2.0 * (k * k - 1.0) / a0;
    filter.a2 = (1.0 - k / quality + k * k) / a0;
    return filter;
}

Biquad highPass(double sampleRate)
{
    constexpr auto frequency = 38.13547087602444;
    constexpr auto quality = 0.5003270373238773;
    const auto k = std::tan(
        juce::MathConstants<double>::pi * frequency / sampleRate);
    const auto a0 = 1.0 + k / quality + k * k;

    Biquad filter;
    filter.b0 = 1.0;
    filter.b1 = -2.0;
    filter.b2 = 1.0;
    filter.a1 = 2.0 * (k * k - 1.0) / a0;
    filter.a2 = (1.0 - k / quality + k * k) / a0;
    return filter;
}

double loudnessForEnergy(double energy)
{
    return energy > 0.0
        ? -0.691 + 10.0 * std::log10(energy)
        : -std::numeric_limits<double>::infinity();
}

double truePeak(const juce::AudioBuffer<float>& audio,
                double sampleRate)
{
    const auto stages = sampleRate <= 48000.0
        ? std::size_t { 3 }
        : sampleRate <= 96000.0
            ? std::size_t { 2 }
            : std::size_t { 1 };
    constexpr auto blockSize = 4096;
    juce::dsp::Oversampling<float> oversampling(
        static_cast<std::size_t>(audio.getNumChannels()),
        stages,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
        true);
    oversampling.initProcessing(blockSize);
    oversampling.reset();

    juce::dsp::AudioBlock<const float> input(audio);
    auto peak = 0.0f;
    for (int start = 0;
         start < audio.getNumSamples();
         start += blockSize)
    {
        const auto count = std::min(
            blockSize,
            audio.getNumSamples() - start);
        const auto upsampled = oversampling.processSamplesUp(
            input.getSubBlock(
                static_cast<std::size_t>(start),
                static_cast<std::size_t>(count)));
        for (std::size_t channel = 0;
             channel < upsampled.getNumChannels();
             ++channel)
        {
            const auto* samples = upsampled.getChannelPointer(channel);
            for (std::size_t sample = 0;
                 sample < upsampled.getNumSamples();
                 ++sample)
                peak = std::max(peak, std::abs(samples[sample]));
        }
    }
    return static_cast<double>(peak);
}

double percentile(const std::vector<double>& sortedValues,
                  double proportion)
{
    if (sortedValues.empty())
        return 0.0;
    const auto index = static_cast<std::size_t>(
        std::floor(
            static_cast<double>(sortedValues.size() - 1)
                * proportion
            + 0.5));
    return sortedValues[std::min(index, sortedValues.size() - 1)];
}

std::optional<juce::AudioBuffer<float>> readSource(
    const MasteringSourceMix& source,
    double targetSampleRate,
    juce::String& error)
{
    if (!source.file.existsAsFile())
    {
        error = "Mastering source is missing: "
            + source.file.getFullPathName();
        return std::nullopt;
    }
    if (source.sourceHash.isNotEmpty()
        && juce::SHA256(source.file).toHexString()
            != source.sourceHash)
    {
        error = "Mastering source hash does not match: "
            + source.file.getFileName();
        return std::nullopt;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(
        formats.createReaderFor(source.file));
    if (reader == nullptr
        || reader->lengthInSamples <= 0
        || reader->lengthInSamples
            > std::numeric_limits<int>::max()
        || reader->sampleRate <= 0.0)
    {
        error = "Could not decode mastering source: "
            + source.file.getFullPathName();
        return std::nullopt;
    }

    const auto actualDuration =
        static_cast<double>(reader->lengthInSamples)
        / reader->sampleRate;
    if (std::abs(actualDuration - source.durationSeconds) > 0.01)
    {
        error = "Mastering source duration changed since it was added: "
            + source.file.getFileName();
        return std::nullopt;
    }
    const auto inputSamples =
        static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> input(2, inputSamples);
    if (!reader->read(
            &input,
            0,
            inputSamples,
            0,
            true,
            reader->numChannels > 1))
    {
        error = "Could not read mastering source: "
            + source.file.getFullPathName();
        return std::nullopt;
    }
    if (reader->numChannels == 1)
        input.copyFrom(1, 0, input, 0, 0, inputSamples);

    const auto outputSamples64 = static_cast<std::int64_t>(
        std::llround(actualDuration * targetSampleRate));
    if (outputSamples64 <= 0
        || outputSamples64 > std::numeric_limits<int>::max())
    {
        error = "Mastering source is too long to render.";
        return std::nullopt;
    }
    const auto outputSamples = static_cast<int>(outputSamples64);
    if (std::abs(reader->sampleRate - targetSampleRate) < 0.001
        && outputSamples == inputSamples)
        return input;

    std::vector<float> interleavedInput(
        static_cast<std::size_t>(inputSamples) * 2);
    for (int sample = 0; sample < inputSamples; ++sample)
    {
        interleavedInput[static_cast<std::size_t>(sample) * 2] =
            input.getSample(0, sample);
        interleavedInput[static_cast<std::size_t>(sample) * 2 + 1] =
            input.getSample(1, sample);
    }
    std::vector<float> interleavedOutput(
        static_cast<std::size_t>(outputSamples) * 2);
    SRC_DATA conversion {};
    conversion.data_in = interleavedInput.data();
    conversion.data_out = interleavedOutput.data();
    conversion.input_frames = inputSamples;
    conversion.output_frames = outputSamples;
    conversion.src_ratio = targetSampleRate / reader->sampleRate;
    conversion.end_of_input = 1;
    if (const auto result = src_simple(
            &conversion,
            SRC_SINC_BEST_QUALITY,
            2);
        result != 0
        || conversion.output_frames_gen != outputSamples)
    {
        error = "Could not sample-rate convert mastering source "
            + source.file.getFileName()
            + ": "
            + juce::String(src_strerror(result));
        return std::nullopt;
    }
    juce::AudioBuffer<float> output(2, outputSamples);
    for (int sample = 0; sample < outputSamples; ++sample)
    {
        output.setSample(
            0,
            sample,
            interleavedOutput[static_cast<std::size_t>(sample) * 2]);
        output.setSample(
            1,
            sample,
            interleavedOutput[
                static_cast<std::size_t>(sample) * 2 + 1]);
    }
    return output;
}
}

MasteringMeasurement MasteringEngine::analyse(
    const juce::AudioBuffer<float>& audio,
    double sampleRate)
{
    MasteringMeasurement measurement;
    if (sampleRate <= 0.0
        || audio.getNumChannels() <= 0
        || audio.getNumSamples() <= 0)
        return measurement;
    measurement.durationSeconds =
        static_cast<double>(audio.getNumSamples()) / sampleRate;

    auto samplePeak = 0.0f;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        samplePeak = std::max(
            samplePeak,
            audio.getMagnitude(
                channel,
                0,
                audio.getNumSamples()));
    measurement.samplePeakDbfs =
        juce::Decibels::gainToDecibels(
            static_cast<double>(samplePeak),
            -300.0);
    measurement.truePeakDbtp =
        juce::Decibels::gainToDecibels(
            truePeak(audio, sampleRate),
            -300.0);
    if (audio.getNumChannels() >= 2)
    {
        auto product = 0.0;
        auto leftEnergy = 0.0;
        auto rightEnergy = 0.0;
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto left = static_cast<double>(
                audio.getSample(0, sample));
            const auto right = static_cast<double>(
                audio.getSample(1, sample));
            product += left * right;
            leftEnergy += left * left;
            rightEnergy += right * right;
        }
        const auto denominator = std::sqrt(leftEnergy * rightEnergy);
        measurement.correlation = denominator > 0.0
            ? juce::jlimit(-1.0, 1.0, product / denominator)
            : 1.0;
    }

    const auto analysisTailSamples = static_cast<int>(
        std::llround(sampleRate * 1.5));
    const auto weightedSamples =
        audio.getNumSamples() + analysisTailSamples;
    const auto blockSamples = std::max(
        1,
        static_cast<int>(std::llround(sampleRate * 0.4)));
    const auto shortTermSamples = std::max(
        1,
        static_cast<int>(std::llround(sampleRate * 3.0)));
    const auto hopSamples = std::max(
        1,
        static_cast<int>(std::llround(sampleRate * 0.1)));
    std::vector<Biquad> shelves;
    std::vector<Biquad> highPasses;
    shelves.reserve(static_cast<std::size_t>(audio.getNumChannels()));
    highPasses.reserve(static_cast<std::size_t>(audio.getNumChannels()));
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        shelves.push_back(highShelf(sampleRate));
        highPasses.push_back(highPass(sampleRate));
    }
    std::vector<double> blockRing(
        static_cast<std::size_t>(blockSamples));
    std::vector<double> shortTermRing(
        static_cast<std::size_t>(shortTermSamples));
    auto blockSum = 0.0;
    auto shortTermSum = 0.0;
    std::vector<double> absoluteGated;
    std::vector<double> shortTermEnergies;
    for (int sample = 0; sample < weightedSamples; ++sample)
    {
        auto energy = 0.0;
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            const auto filtered =
                highPasses[static_cast<std::size_t>(channel)].process(
                    shelves[static_cast<std::size_t>(channel)].process(
                        sample < audio.getNumSamples()
                            ? audio.getSample(channel, sample)
                            : 0.0f));
            const auto channelWeight =
                channel == 3 || channel == 4 ? 1.41 : 1.0;
            energy += channelWeight * filtered * filtered;
        }
        const auto blockIndex = static_cast<std::size_t>(
            sample % blockSamples);
        blockSum += energy - blockRing[blockIndex];
        blockRing[blockIndex] = energy;
        const auto shortTermIndex = static_cast<std::size_t>(
            sample % shortTermSamples);
        shortTermSum += energy - shortTermRing[shortTermIndex];
        shortTermRing[shortTermIndex] = energy;

        if (sample + 1 >= blockSamples
            && sample < audio.getNumSamples()
            && (sample + 1 - blockSamples) % hopSamples == 0)
        {
            const auto blockEnergy =
                blockSum / static_cast<double>(blockSamples);
            if (loudnessForEnergy(blockEnergy) > -70.0)
                absoluteGated.push_back(blockEnergy);
        }
        if (sample + 1 >= shortTermSamples
            && (sample + 1 - shortTermSamples) % hopSamples == 0)
        {
            const auto shortTermEnergy =
                shortTermSum
                / static_cast<double>(shortTermSamples);
            if (loudnessForEnergy(shortTermEnergy) > -70.0)
                shortTermEnergies.push_back(shortTermEnergy);
        }
    }
    if (!absoluteGated.empty())
    {
        const auto absoluteMean = std::accumulate(
            absoluteGated.cbegin(),
            absoluteGated.cend(),
            0.0)
            / static_cast<double>(absoluteGated.size());
        const auto relativeGate =
            loudnessForEnergy(absoluteMean) - 10.0;
        auto finalEnergy = 0.0;
        auto finalCount = std::size_t { 0 };
        for (const auto energy : absoluteGated)
        {
            if (loudnessForEnergy(energy) > relativeGate)
            {
                finalEnergy += energy;
                ++finalCount;
            }
        }
        if (finalCount > 0)
            measurement.integratedLoudnessLufs =
                loudnessForEnergy(
                    finalEnergy / static_cast<double>(finalCount));
    }
    if (!shortTermEnergies.empty())
    {
        const auto mean = std::accumulate(
            shortTermEnergies.cbegin(),
            shortTermEnergies.cend(),
            0.0)
            / static_cast<double>(shortTermEnergies.size());
        const auto gate = loudnessForEnergy(mean) - 20.0;
        std::vector<double> gatedLoudness;
        for (const auto energy : shortTermEnergies)
            if (loudnessForEnergy(energy) > gate)
                gatedLoudness.push_back(loudnessForEnergy(energy));
        if (!gatedLoudness.empty())
        {
            std::sort(gatedLoudness.begin(), gatedLoudness.end());
            measurement.loudnessRangeLu =
                percentile(gatedLoudness, 0.95)
                - percentile(gatedLoudness, 0.10);
        }
    }
    return measurement;
}

std::optional<MasteringRender> MasteringEngine::renderAlbum(
    const MasteringAlbum& album,
    double sampleRate,
    juce::String& error)
{
    if (sampleRate < 8000.0 || sampleRate > 384000.0)
    {
        error = "Mastering renders require a sample rate from 8 to 384 kHz.";
        return std::nullopt;
    }
    juce::String albumError;
    const auto validated = MasteringAlbum::fromVar(
        album.toVar(),
        albumError);
    if (!validated.has_value())
    {
        error = "Invalid mastering album: " + albumError;
        return std::nullopt;
    }
    if (album.tracks.empty())
    {
        error = "Add at least one song before rendering the mastering album.";
        return std::nullopt;
    }

    MasteringRender render;
    render.sampleRate = sampleRate;
    render.placements = album.placements();
    const auto totalSamples64 = static_cast<std::int64_t>(
        std::llround(album.durationSeconds() * sampleRate));
    if (totalSamples64 <= 0
        || totalSamples64 > std::numeric_limits<int>::max())
    {
        error = "The mastering album is too long to render in one pass.";
        return std::nullopt;
    }
    render.audio.setSize(2, static_cast<int>(totalSamples64));
    render.audio.clear();
    const auto albumGain = juce::Decibels::decibelsToGain(
        static_cast<float>(album.outputGainDecibels));

    for (std::size_t index = 0;
         index < render.placements.size();
         ++index)
    {
        const auto& placement = render.placements[index];
        const auto& track = album.tracks[index];
        const auto* source = track.selectedSource();
        if (source == nullptr)
        {
            error = "Mastering track has no selected source: "
                + track.title;
            return std::nullopt;
        }
        auto sourceAudio = readSource(*source, sampleRate, error);
        if (!sourceAudio.has_value())
            return std::nullopt;

        const auto destinationStart = static_cast<int>(
            std::llround(placement.startSeconds * sampleRate));
        const auto fadeInSamples = static_cast<int>(
            std::llround(track.fadeInSeconds * sampleRate));
        const auto fadeOutSamples = static_cast<int>(
            std::llround(track.fadeOutSeconds * sampleRate));
        const auto trackGain = juce::Decibels::decibelsToGain(
            static_cast<float>(track.gainDecibels))
            * albumGain;
        for (int sample = 0;
             sample < sourceAudio->getNumSamples();
             ++sample)
        {
            const auto destinationSample =
                destinationStart + sample;
            if (destinationSample < 0
                || destinationSample >= render.audio.getNumSamples())
                continue;
            auto envelope = 1.0f;
            if (fadeInSamples > 0 && sample < fadeInSamples)
                envelope *= static_cast<float>(sample)
                    / static_cast<float>(fadeInSamples);
            const auto remaining =
                sourceAudio->getNumSamples() - 1 - sample;
            if (fadeOutSamples > 0 && remaining < fadeOutSamples)
                envelope *= static_cast<float>(remaining)
                    / static_cast<float>(fadeOutSamples);
            const auto gain = trackGain * envelope;
            for (int channel = 0; channel < 2; ++channel)
                render.audio.addSample(
                    channel,
                    destinationSample,
                    sourceAudio->getSample(channel, sample) * gain);
        }
    }
    return render;
}
}

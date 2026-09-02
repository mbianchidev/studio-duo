#include "UtilityDeviceProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace studio
{
namespace
{
float coefficient(double milliseconds, double sampleRate)
{
    return static_cast<float>(
        std::exp(-1.0
                 / (std::max(0.01, milliseconds)
                    * 0.001
                    * sampleRate)));
}
}

juce::AudioProcessor::BusesProperties UtilityDeviceProcessor::busesFor(
    UtilityDeviceType type)
{
    auto buses = BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true);
    if (type == UtilityDeviceType::compressor
        || type == UtilityDeviceType::gate)
        buses = buses.withInput(
            "Sidechain",
            juce::AudioChannelSet::stereo(),
            false);
    return buses.withOutput(
        "Output",
        juce::AudioChannelSet::stereo(),
        true);
}

UtilityDeviceProcessor::UtilityDeviceProcessor(UtilityDeviceType deviceType)
    : juce::AudioProcessor(busesFor(deviceType)),
      type(deviceType)
{
    switch (type)
    {
        case UtilityDeviceType::equalizer:
            addFloat(ParameterSlot::frequency, "frequency", "Frequency",
                     { 20.0f, 20000.0f, 0.0f, 0.3f }, 1000.0f);
            addFloat(ParameterSlot::midGain, "midGain", "Gain", { -18.0f, 18.0f }, 0.0f);
            addFloat(ParameterSlot::q, "q", "Q", { 0.2f, 10.0f, 0.0f, 0.4f }, 1.0f);
            break;
        case UtilityDeviceType::compressor:
            addFloat(ParameterSlot::threshold, "threshold", "Threshold", { -60.0f, 0.0f }, -18.0f);
            addFloat(ParameterSlot::ratio, "ratio", "Ratio", { 1.0f, 20.0f }, 4.0f);
            addFloat(ParameterSlot::attack, "attack", "Attack", { 0.1f, 100.0f }, 10.0f);
            addFloat(ParameterSlot::release, "release", "Release", { 5.0f, 1000.0f }, 100.0f);
            addFloat(ParameterSlot::makeup, "makeup", "Makeup", { -12.0f, 24.0f }, 0.0f);
            addFloat(ParameterSlot::mix, "mix", "Mix", { 0.0f, 1.0f }, 1.0f);
            break;
        case UtilityDeviceType::limiter:
            addFloat(ParameterSlot::ceiling, "ceiling", "Ceiling", { -12.0f, 0.0f }, -0.5f);
            addFloat(ParameterSlot::release, "release", "Release", { 5.0f, 1000.0f }, 100.0f);
            addFloat(ParameterSlot::truePeak, "truePeak", "True peak", { 0.0f, 1.0f, 1.0f }, 1.0f);
            break;
        case UtilityDeviceType::reverb:
            addFloat(ParameterSlot::room, "room", "Room size", { 0.0f, 1.0f }, 0.5f);
            addFloat(ParameterSlot::damping, "damping", "Damping", { 0.0f, 1.0f }, 0.5f);
            addFloat(ParameterSlot::width, "width", "Width", { 0.0f, 1.0f }, 1.0f);
            addFloat(ParameterSlot::mix, "mix", "Mix", { 0.0f, 1.0f }, 0.35f);
            break;
        case UtilityDeviceType::gate:
            addFloat(ParameterSlot::threshold, "threshold", "Threshold", { -80.0f, 0.0f }, -40.0f);
            addFloat(ParameterSlot::attack, "attack", "Attack", { 0.1f, 100.0f }, 2.0f);
            addFloat(ParameterSlot::release, "release", "Release", { 5.0f, 1000.0f }, 100.0f);
            addFloat(ParameterSlot::range, "range", "Range", { -96.0f, 0.0f }, -96.0f);
            break;
        case UtilityDeviceType::gain:
            addFloat(ParameterSlot::gain, "gain", "Gain", { -24.0f, 24.0f }, 0.0f);
            break;
        case UtilityDeviceType::polarity:
            addFloat(ParameterSlot::invert, "invert", "Invert", { 0.0f, 1.0f, 1.0f }, 0.0f);
            break;
        case UtilityDeviceType::delay:
            addFloat(ParameterSlot::samples, "samples", "Samples", { 0.0f, 96000.0f, 1.0f }, 0.0f);
            break;
        case UtilityDeviceType::tuner:
            break;
        case UtilityDeviceType::generator:
            addFloat(ParameterSlot::waveform, "waveform", "Waveform", { 0.0f, 2.0f, 1.0f }, 0.0f);
            addFloat(ParameterSlot::frequency, "frequency", "Frequency",
                     { 20.0f, 20000.0f, 0.0f, 0.3f }, 440.0f);
            addFloat(ParameterSlot::level, "level", "Level", { -60.0f, 0.0f }, -18.0f);
            break;
    }
}

juce::AudioParameterFloat* UtilityDeviceProcessor::addFloat(
    ParameterSlot slot,
    const juce::String& id,
    const juce::String& name,
    juce::NormalisableRange<float> range,
    float defaultValue)
{
    auto value = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(id, 1),
        name,
        range,
        defaultValue);
    auto* pointer = value.get();
    addParameter(pointer);
    value.release();
    parameterLookup.emplace_back(id, pointer);
    realtimeParameters[static_cast<std::size_t>(slot)] = pointer;
    return pointer;
}

const juce::String UtilityDeviceProcessor::getName() const
{
    switch (type)
    {
        case UtilityDeviceType::equalizer: return "Parametric EQ";
        case UtilityDeviceType::compressor: return "Compressor";
        case UtilityDeviceType::limiter: return "True-Peak Limiter";
        case UtilityDeviceType::reverb: return "Algorithmic Reverb";
        case UtilityDeviceType::gate: return "Noise Gate";
        case UtilityDeviceType::gain: return "Gain";
        case UtilityDeviceType::polarity: return "Polarity";
        case UtilityDeviceType::delay: return "Delay";
        case UtilityDeviceType::tuner: return "Tuner";
        case UtilityDeviceType::generator: return "Signal Generator";
    }
    return "Utility";
}

void UtilityDeviceProcessor::prepareToPlay(double sampleRate,
                                           int maximumBlockSize)
{
    currentSampleRate = sampleRate;
    maximumBlock = maximumBlockSize;
    delayBuffer.setSize(
        2,
        static_cast<int>(std::ceil(sampleRate * 2.0))
            + maximumBlockSize
            + 1,
        false,
        true,
        false);
    if (type == UtilityDeviceType::limiter)
    {
        limiterOversampling =
            std::make_unique<juce::dsp::Oversampling<float>>(
                2,
                2,
                juce::dsp::Oversampling<float>::
                    filterHalfBandPolyphaseIIR,
                true);
        limiterOversampling->initProcessing(
            static_cast<std::size_t>(
                std::max(1, maximumBlockSize)));
    }
    reset();
    reverb.setSampleRate(sampleRate);
}

void UtilityDeviceProcessor::releaseResources()
{
}

void UtilityDeviceProcessor::reset()
{
    for (auto& filter : equalizerFilters)
        filter.reset();
    compressorEnvelope.fill(1.0f);
    gateEnvelope.fill(0.0f);
    limiterGain.fill(1.0f);
    if (limiterOversampling != nullptr)
        limiterOversampling->reset();
    delayBuffer.clear();
    delayWritePosition = 0;
    reverb.reset();
    generatorPhase = 0.0;
    randomState = 0x6d2b79f5u;
    pinkState.fill(0.0f);
    tunerHistory.fill(0.0f);
    tunerAnalysis.fill(0.0f);
    tunerCorrelation.fill(0.0f);
    tunerWritePosition = 0;
    tunerSamplesAvailable = 0;
    tunerSamplesSinceAnalysis = 0;
    tunerDecimationCount = 0;
    tunerDecimationSum = 0.0f;
    detectedFrequency.store(0.0, std::memory_order_relaxed);
}

void UtilityDeviceProcessor::processBlock(juce::AudioBuffer<float>& audio,
                                          juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    switch (type)
    {
        case UtilityDeviceType::equalizer:
            processEqualizer(audio);
            break;
        case UtilityDeviceType::compressor:
            processCompressor(audio);
            break;
        case UtilityDeviceType::limiter:
            processLimiter(audio);
            break;
        case UtilityDeviceType::reverb:
        {
            juce::Reverb::Parameters settings;
            settings.roomSize = parameter(ParameterSlot::room);
            settings.damping = parameter(ParameterSlot::damping);
            settings.width = parameter(ParameterSlot::width);
            settings.wetLevel = parameter(ParameterSlot::mix);
            settings.dryLevel = 1.0f - settings.wetLevel;
            reverb.setParameters(settings);
            reverb.processStereo(
                audio.getWritePointer(0),
                audio.getWritePointer(std::min(1, audio.getNumChannels() - 1)),
                audio.getNumSamples());
            break;
        }
        case UtilityDeviceType::gate:
            processGate(audio);
            break;
        case UtilityDeviceType::gain:
            audio.applyGain(
                juce::Decibels::decibelsToGain(parameter(ParameterSlot::gain)));
            break;
        case UtilityDeviceType::polarity:
            if (parameter(ParameterSlot::invert) >= 0.5f)
                audio.applyGain(-1.0f);
            break;
        case UtilityDeviceType::delay:
            processDelay(audio);
            break;
        case UtilityDeviceType::tuner:
            processTuner(audio);
            break;
        case UtilityDeviceType::generator:
            processGenerator(audio);
            break;
    }
}

float UtilityDeviceProcessor::Biquad::process(float input) noexcept
{
    const auto output = input * b0 + z1;
    z1 = input * b1 - output * a1 + z2;
    z2 = input * b2 - output * a2;
    return output;
}

void UtilityDeviceProcessor::Biquad::reset() noexcept
{
    z1 = 0.0f;
    z2 = 0.0f;
}

void UtilityDeviceProcessor::processEqualizer(
    juce::AudioBuffer<float>& audio) noexcept
{
    const auto frequency = juce::jlimit(
        20.0,
        currentSampleRate * 0.45,
        static_cast<double>(parameter(ParameterSlot::frequency)));
    const auto gain = static_cast<double>(parameter(ParameterSlot::midGain));
    const auto q = std::max(0.2, static_cast<double>(parameter(ParameterSlot::q)));
    const auto a = std::pow(10.0, gain / 40.0);
    const auto omega = juce::MathConstants<double>::twoPi
        * frequency
        / currentSampleRate;
    const auto alpha = std::sin(omega) / (2.0 * q);
    const auto cosOmega = std::cos(omega);
    const auto a0 = 1.0 + alpha / a;
    const auto b0 = (1.0 + alpha * a) / a0;
    const auto b1 = (-2.0 * cosOmega) / a0;
    const auto b2 = (1.0 - alpha * a) / a0;
    const auto a1 = (-2.0 * cosOmega) / a0;
    const auto a2 = (1.0 - alpha / a) / a0;
    for (auto& filter : equalizerFilters)
    {
        filter.b0 = static_cast<float>(b0);
        filter.b1 = static_cast<float>(b1);
        filter.b2 = static_cast<float>(b2);
        filter.a1 = static_cast<float>(a1);
        filter.a2 = static_cast<float>(a2);
    }
    for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
    {
        auto* samples = audio.getWritePointer(channel);
        auto& filter = equalizerFilters[static_cast<std::size_t>(channel)];
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            samples[sample] = filter.process(samples[sample]);
    }
}

void UtilityDeviceProcessor::processCompressor(
    juce::AudioBuffer<float>& audio) noexcept
{
    auto sidechain = getBusCount(true) > 1
        ? getBusBuffer(audio, true, 1)
        : juce::AudioBuffer<float>();
    const auto* detectorBuffer = sidechain.getNumChannels() > 0
        ? &sidechain
        : &audio;
    const auto threshold = parameter(ParameterSlot::threshold);
    const auto ratio = parameter(ParameterSlot::ratio);
    const auto attack = coefficient(parameter(ParameterSlot::attack), currentSampleRate);
    const auto release = coefficient(parameter(ParameterSlot::release), currentSampleRate);
    const auto makeup =
        juce::Decibels::decibelsToGain(parameter(ParameterSlot::makeup));
    const auto mix = parameter(ParameterSlot::mix);
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        auto detector = 0.0f;
        for (int channel = 0;
             channel < std::min(2, detectorBuffer->getNumChannels());
             ++channel)
            detector = std::max(
                detector,
                std::abs(detectorBuffer->getSample(channel, sample)));
        const auto level = juce::Decibels::gainToDecibels(
            detector,
            -120.0f);
        const auto reduction = level > threshold
            ? (threshold + (level - threshold) / ratio) - level
            : 0.0f;
        const auto target = juce::Decibels::decibelsToGain(reduction);
        for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
        {
            auto& envelope =
                compressorEnvelope[static_cast<std::size_t>(channel)];
            const auto smoothing = target < envelope ? attack : release;
            envelope = target + smoothing * (envelope - target);
            const auto dry = audio.getSample(channel, sample);
            const auto wet = dry * envelope * makeup;
            audio.setSample(channel,
                            sample,
                            dry + (wet - dry) * mix);
        }
    }
}

void UtilityDeviceProcessor::processLimiter(
    juce::AudioBuffer<float>& audio) noexcept
{
    const auto ceiling =
        juce::Decibels::decibelsToGain(parameter(ParameterSlot::ceiling));
    const auto release = coefficient(parameter(ParameterSlot::release), currentSampleRate);
    const auto truePeak = parameter(ParameterSlot::truePeak) >= 0.5f;
    juce::dsp::AudioBlock<float> oversampled;
    auto oversamplingFactor = std::size_t { 1 };
    if (truePeak && limiterOversampling != nullptr)
    {
        auto inputBlock = juce::dsp::AudioBlock<float>(audio)
                              .getSubsetChannelBlock(
                                  0,
                                  static_cast<std::size_t>(
                                      std::min(2, audio.getNumChannels())));
        oversampled =
            limiterOversampling->processSamplesUp(inputBlock);
        oversamplingFactor =
            limiterOversampling->getOversamplingFactor();
    }
    else if (limiterOversampling != nullptr)
    {
        limiterOversampling->reset();
    }
    for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
    {
        auto gain = limiterGain[static_cast<std::size_t>(channel)];
        auto* samples = audio.getWritePointer(channel);
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto current = samples[sample];
            auto peak = std::abs(current);
            if (truePeak && oversampled.getNumChannels() > 0)
            {
                const auto first = static_cast<std::size_t>(sample)
                    * oversamplingFactor;
                const auto end = std::min(
                    first + oversamplingFactor,
                    oversampled.getNumSamples());
                for (auto index = first; index < end; ++index)
                {
                    peak = std::max(
                        peak,
                        std::abs(oversampled.getSample(
                            channel,
                            static_cast<int>(index))));
                }
            }
            const auto target = peak > ceiling && peak > 0.0f
                ? ceiling / peak
                : 1.0f;
            gain = target < gain
                ? target
                : 1.0f + release * (gain - 1.0f);
            samples[sample] = current * gain;
        }
        limiterGain[static_cast<std::size_t>(channel)] = gain;
    }
}

void UtilityDeviceProcessor::processGate(
    juce::AudioBuffer<float>& audio) noexcept
{
    auto sidechain = getBusCount(true) > 1
        ? getBusBuffer(audio, true, 1)
        : juce::AudioBuffer<float>();
    const auto* detectorBuffer = sidechain.getNumChannels() > 0
        ? &sidechain
        : &audio;
    const auto threshold = juce::Decibels::decibelsToGain(
        parameter(ParameterSlot::threshold));
    const auto floorGain = juce::Decibels::decibelsToGain(
        parameter(ParameterSlot::range));
    const auto attack = coefficient(parameter(ParameterSlot::attack), currentSampleRate);
    const auto release = coefficient(parameter(ParameterSlot::release), currentSampleRate);
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        auto detector = 0.0f;
        for (int channel = 0;
             channel < std::min(2, detectorBuffer->getNumChannels());
             ++channel)
            detector = std::max(
                detector,
                std::abs(detectorBuffer->getSample(channel, sample)));
        const auto target = detector >= threshold ? 1.0f : floorGain;
        for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
        {
            auto& envelope = gateEnvelope[static_cast<std::size_t>(channel)];
            const auto smoothing = target > envelope ? attack : release;
            envelope = target + smoothing * (envelope - target);
            audio.applyGain(channel, sample, 1, envelope);
        }
    }
}

void UtilityDeviceProcessor::processDelay(
    juce::AudioBuffer<float>& audio) noexcept
{
    if (delayBuffer.getNumSamples() <= 1)
        return;
    const auto delaySamples = juce::jlimit(
        0,
        delayBuffer.getNumSamples() - maximumBlock - 1,
        static_cast<int>(std::round(parameter(ParameterSlot::samples))));
    const auto capacity = delayBuffer.getNumSamples();
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        auto read = delayWritePosition - delaySamples;
        if (read < 0)
            read += capacity;
        for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
        {
            const auto input = audio.getSample(channel, sample);
            const auto output = delaySamples == 0
                ? input
                : delayBuffer.getSample(channel, read);
            delayBuffer.setSample(channel, delayWritePosition, input);
            audio.setSample(channel, sample, output);
        }
        delayWritePosition = (delayWritePosition + 1) % capacity;
    }
}

void UtilityDeviceProcessor::processTuner(
    const juce::AudioBuffer<float>& audio) noexcept
{
    if (audio.getNumChannels() == 0)
        return;

    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        auto mono = 0.0f;
        for (int channel = 0;
             channel < std::min(2, audio.getNumChannels());
             ++channel)
        {
            mono += audio.getSample(channel, sample);
        }
        mono /= static_cast<float>(
            std::min(2, audio.getNumChannels()));
        tunerDecimationSum += mono;
        ++tunerDecimationCount;
        if (tunerDecimationCount < tunerDownsampleFactor)
            continue;

        tunerHistory[static_cast<std::size_t>(
            tunerWritePosition)] =
            tunerDecimationSum
            / static_cast<float>(tunerDownsampleFactor);
        tunerWritePosition =
            (tunerWritePosition + 1) % tunerHistorySize;
        tunerSamplesAvailable = std::min(
            tunerHistorySize,
            tunerSamplesAvailable + 1);
        ++tunerSamplesSinceAnalysis;
        tunerDecimationCount = 0;
        tunerDecimationSum = 0.0f;
    }

    const auto analysisRate =
        currentSampleRate
        / static_cast<double>(tunerDownsampleFactor);
    const auto maximumLag = std::min({
        tunerSamplesAvailable / 2,
        static_cast<int>(analysisRate / 24.0),
        static_cast<int>(tunerCorrelation.size()) - 1
    });
    const auto minimumLag = std::max(
        2,
        static_cast<int>(analysisRate / 2000.0));
    if (maximumLag <= minimumLag
        || tunerSamplesAvailable < maximumLag * 2
        || (tunerSamplesSinceAnalysis < 256
            && detectedFrequency.load(std::memory_order_relaxed)
                > 0.0))
    {
        return;
    }
    tunerSamplesSinceAnalysis = 0;

    const auto oldest = tunerSamplesAvailable == tunerHistorySize
        ? tunerWritePosition
        : 0;
    auto mean = 0.0;
    for (int index = 0; index < tunerSamplesAvailable; ++index)
    {
        const auto value = tunerHistory[static_cast<std::size_t>(
            (oldest + index) % tunerHistorySize)];
        tunerAnalysis[static_cast<std::size_t>(index)] = value;
        mean += value;
    }
    mean /= static_cast<double>(tunerSamplesAvailable);
    auto energy = 0.0;
    for (int index = 0; index < tunerSamplesAvailable; ++index)
    {
        auto& value = tunerAnalysis[static_cast<std::size_t>(index)];
        value -= static_cast<float>(mean);
        energy += static_cast<double>(value) * value;
    }
    if (energy < 0.000001)
    {
        detectedFrequency.store(0.0, std::memory_order_relaxed);
        return;
    }

    for (int lag = minimumLag; lag <= maximumLag; ++lag)
    {
        auto correlation = 0.0;
        auto firstEnergy = 0.0;
        auto secondEnergy = 0.0;
        const auto count = tunerSamplesAvailable - lag;
        for (int index = 0; index < count; ++index)
        {
            const auto first =
                tunerAnalysis[static_cast<std::size_t>(index)];
            const auto second = tunerAnalysis[static_cast<std::size_t>(
                index + lag)];
            correlation += static_cast<double>(first) * second;
            firstEnergy += static_cast<double>(first) * first;
            secondEnergy += static_cast<double>(second) * second;
        }
        const auto denominator = firstEnergy + secondEnergy;
        tunerCorrelation[static_cast<std::size_t>(lag)] =
            denominator > 0.0
            ? static_cast<float>(2.0 * correlation / denominator)
            : 0.0f;
    }

    auto crossedNegative = false;
    auto bestLag = 0;
    for (int lag = minimumLag + 1; lag < maximumLag; ++lag)
    {
        const auto score =
            tunerCorrelation[static_cast<std::size_t>(lag)];
        crossedNegative = crossedNegative || score < 0.0f;
        if (!crossedNegative || score < 0.6f)
            continue;
        if (score
                >= tunerCorrelation[static_cast<std::size_t>(lag - 1)]
            && score
                > tunerCorrelation[static_cast<std::size_t>(lag + 1)])
        {
            bestLag = lag;
            break;
        }
    }
    if (bestLag == 0)
    {
        bestLag = minimumLag;
        for (int lag = minimumLag + 1; lag <= maximumLag; ++lag)
        {
            if (tunerCorrelation[static_cast<std::size_t>(lag)]
                > tunerCorrelation[static_cast<std::size_t>(bestLag)])
            {
                bestLag = lag;
            }
        }
    }

    auto refinedLag = static_cast<double>(bestLag);
    if (bestLag > minimumLag && bestLag < maximumLag)
    {
        const auto left = tunerCorrelation[
            static_cast<std::size_t>(bestLag - 1)];
        const auto centre = tunerCorrelation[
            static_cast<std::size_t>(bestLag)];
        const auto right = tunerCorrelation[
            static_cast<std::size_t>(bestLag + 1)];
        const auto curvature = left - 2.0f * centre + right;
        if (std::abs(curvature) > 0.000001f)
        {
            refinedLag += juce::jlimit(
                -0.5,
                0.5,
                0.5 * static_cast<double>(left - right)
                    / static_cast<double>(curvature));
        }
    }
    detectedFrequency.store(
        refinedLag > 0.0 ? analysisRate / refinedLag : 0.0,
        std::memory_order_relaxed);
}

void UtilityDeviceProcessor::processGenerator(
    juce::AudioBuffer<float>& audio) noexcept
{
    const auto waveform = static_cast<int>(
        std::round(parameter(ParameterSlot::waveform)));
    const auto frequency = parameter(ParameterSlot::frequency);
    const auto level =
        juce::Decibels::decibelsToGain(parameter(ParameterSlot::level));
    const auto phaseStep = juce::MathConstants<double>::twoPi
        * frequency
        / currentSampleRate;
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        float generated = 0.0f;
        if (waveform == 0)
        {
            generated = static_cast<float>(std::sin(generatorPhase));
        }
        else
        {
            randomState ^= randomState << 13;
            randomState ^= randomState >> 17;
            randomState ^= randomState << 5;
            const auto white =
                static_cast<float>(randomState)
                    / static_cast<float>(
                        std::numeric_limits<std::uint32_t>::max())
                    * 2.0f
                - 1.0f;
            if (waveform == 1)
                generated = white;
            else
            {
                pinkState[0] = 0.99765f * pinkState[0] + white * 0.099046f;
                pinkState[1] = 0.96300f * pinkState[1] + white * 0.2965164f;
                generated = (pinkState[0] + pinkState[1] + white * 0.1848f)
                    * 0.5f;
            }
        }
        generatorPhase += phaseStep;
        if (generatorPhase >= juce::MathConstants<double>::twoPi)
            generatorPhase -= juce::MathConstants<double>::twoPi;
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            audio.setSample(channel, sample, generated * level);
    }
}

float UtilityDeviceProcessor::parameter(ParameterSlot slot) const noexcept
{
    const auto* value =
        realtimeParameters[static_cast<std::size_t>(slot)];
    return value != nullptr ? value->get() : 0.0f;
}

double UtilityDeviceProcessor::getTailLengthSeconds() const
{
    return type == UtilityDeviceType::reverb ? 4.0 : 0.0;
}

bool UtilityDeviceProcessor::acceptsMidi() const
{
    return false;
}

bool UtilityDeviceProcessor::producesMidi() const
{
    return false;
}

juce::AudioProcessorEditor* UtilityDeviceProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

bool UtilityDeviceProcessor::hasEditor() const
{
    return true;
}

int UtilityDeviceProcessor::getNumPrograms()
{
    return 1;
}

int UtilityDeviceProcessor::getCurrentProgram()
{
    return 0;
}

void UtilityDeviceProcessor::setCurrentProgram(int)
{
}

const juce::String UtilityDeviceProcessor::getProgramName(int)
{
    return "Default";
}

void UtilityDeviceProcessor::changeProgramName(int, const juce::String&)
{
}

void UtilityDeviceProcessor::getStateInformation(
    juce::MemoryBlock& destination)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("device", getName());
    for (const auto& [id, value] : parameterLookup)
        object->setProperty(
            id,
            static_cast<juce::AudioProcessorParameter*>(value)->getValue());
    const auto json = juce::JSON::toString(
        juce::var(object.release()),
        false);
    destination.replaceAll(json.toRawUTF8(),
                           static_cast<std::size_t>(
                               json.getNumBytesAsUTF8()));
}

void UtilityDeviceProcessor::setStateInformation(const void* data, int size)
{
    if (data == nullptr || size <= 0)
        return;
    const auto value = juce::JSON::parse(
        juce::String::fromUTF8(
            static_cast<const char*>(data),
            size));
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        return;
    for (const auto& [id, parameterValue] : parameterLookup)
    {
        const auto saved = object->getProperty(id);
        if (saved.isInt() || saved.isInt64() || saved.isDouble())
            static_cast<juce::AudioProcessorParameter*>(parameterValue)
                ->setValue(
                juce::jlimit(
                    0.0f,
                    1.0f,
                    static_cast<float>(static_cast<double>(saved))));
    }
}

bool UtilityDeviceProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet()
        != juce::AudioChannelSet::stereo())
        return false;
    const auto input = layouts.getMainInputChannelSet();
    return input == juce::AudioChannelSet::stereo()
        || (type == UtilityDeviceType::generator && input.isDisabled());
}

UtilityDeviceType UtilityDeviceProcessor::deviceType() const noexcept
{
    return type;
}

double UtilityDeviceProcessor::meterValue(const juce::String& meter) const
{
    return type == UtilityDeviceType::tuner && meter == "frequency"
        ? detectedFrequency.load(std::memory_order_relaxed)
        : 0.0;
}
}

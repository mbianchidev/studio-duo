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
            addFloat("frequency", "Frequency",
                     { 20.0f, 20000.0f, 0.0f, 0.3f }, 1000.0f);
            addFloat("midGain", "Gain", { -18.0f, 18.0f }, 0.0f);
            addFloat("q", "Q", { 0.2f, 10.0f, 0.0f, 0.4f }, 1.0f);
            break;
        case UtilityDeviceType::compressor:
            addFloat("threshold", "Threshold", { -60.0f, 0.0f }, -18.0f);
            addFloat("ratio", "Ratio", { 1.0f, 20.0f }, 4.0f);
            addFloat("attack", "Attack", { 0.1f, 100.0f }, 10.0f);
            addFloat("release", "Release", { 5.0f, 1000.0f }, 100.0f);
            addFloat("makeup", "Makeup", { -12.0f, 24.0f }, 0.0f);
            addFloat("mix", "Mix", { 0.0f, 1.0f }, 1.0f);
            break;
        case UtilityDeviceType::limiter:
            addFloat("ceiling", "Ceiling", { -12.0f, 0.0f }, -0.5f);
            addFloat("release", "Release", { 5.0f, 1000.0f }, 100.0f);
            addFloat("truePeak", "True peak", { 0.0f, 1.0f, 1.0f }, 1.0f);
            break;
        case UtilityDeviceType::reverb:
            addFloat("room", "Room size", { 0.0f, 1.0f }, 0.5f);
            addFloat("damping", "Damping", { 0.0f, 1.0f }, 0.5f);
            addFloat("width", "Width", { 0.0f, 1.0f }, 1.0f);
            addFloat("mix", "Mix", { 0.0f, 1.0f }, 0.35f);
            break;
        case UtilityDeviceType::gate:
            addFloat("threshold", "Threshold", { -80.0f, 0.0f }, -40.0f);
            addFloat("attack", "Attack", { 0.1f, 100.0f }, 2.0f);
            addFloat("release", "Release", { 5.0f, 1000.0f }, 100.0f);
            addFloat("range", "Range", { -96.0f, 0.0f }, -96.0f);
            break;
        case UtilityDeviceType::gain:
            addFloat("gain", "Gain", { -24.0f, 24.0f }, 0.0f);
            break;
        case UtilityDeviceType::polarity:
            addFloat("invert", "Invert", { 0.0f, 1.0f, 1.0f }, 0.0f);
            break;
        case UtilityDeviceType::delay:
            addFloat("samples", "Samples", { 0.0f, 96000.0f, 1.0f }, 0.0f);
            break;
        case UtilityDeviceType::tuner:
            break;
        case UtilityDeviceType::generator:
            addFloat("waveform", "Waveform", { 0.0f, 2.0f, 1.0f }, 0.0f);
            addFloat("frequency", "Frequency",
                     { 20.0f, 20000.0f, 0.0f, 0.3f }, 440.0f);
            addFloat("level", "Level", { -60.0f, 0.0f }, -18.0f);
            break;
    }
}

juce::AudioParameterFloat* UtilityDeviceProcessor::addFloat(
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
    compressorEnvelope.fill(0.0f);
    gateEnvelope.fill(0.0f);
    limiterPrevious.fill(0.0f);
    delayBuffer.clear();
    delayWritePosition = 0;
    reverb.reset();
    generatorPhase = 0.0;
    randomState = 0x6d2b79f5u;
    pinkState.fill(0.0f);
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
            settings.roomSize = parameter("room");
            settings.damping = parameter("damping");
            settings.width = parameter("width");
            settings.wetLevel = parameter("mix");
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
                juce::Decibels::decibelsToGain(parameter("gain")));
            break;
        case UtilityDeviceType::polarity:
            if (parameter("invert") >= 0.5f)
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
        static_cast<double>(parameter("frequency")));
    const auto gain = static_cast<double>(parameter("midGain"));
    const auto q = std::max(0.2, static_cast<double>(parameter("q")));
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
    const auto threshold = parameter("threshold");
    const auto ratio = parameter("ratio");
    const auto attack = coefficient(parameter("attack"), currentSampleRate);
    const auto release = coefficient(parameter("release"), currentSampleRate);
    const auto makeup =
        juce::Decibels::decibelsToGain(parameter("makeup"));
    const auto mix = parameter("mix");
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
        juce::Decibels::decibelsToGain(parameter("ceiling"));
    const auto release = coefficient(parameter("release"), currentSampleRate);
    const auto truePeak = parameter("truePeak") >= 0.5f;
    for (int channel = 0; channel < std::min(2, audio.getNumChannels()); ++channel)
    {
        auto gain = 1.0f;
        auto* samples = audio.getWritePointer(channel);
        auto previous = limiterPrevious[static_cast<std::size_t>(channel)];
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto current = samples[sample];
            auto peak = std::abs(current);
            if (truePeak)
            {
                for (int step = 1; step < 4; ++step)
                {
                    const auto interpolated = previous
                        + (current - previous)
                            * static_cast<float>(step)
                            / 4.0f;
                    peak = std::max(peak, std::abs(interpolated));
                }
            }
            const auto target = peak > ceiling && peak > 0.0f
                ? ceiling / peak
                : 1.0f;
            gain = target < gain
                ? target
                : 1.0f + release * (gain - 1.0f);
            samples[sample] = current * gain;
            previous = current;
        }
        limiterPrevious[static_cast<std::size_t>(channel)] = previous;
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
        parameter("threshold"));
    const auto floorGain = juce::Decibels::decibelsToGain(
        parameter("range"));
    const auto attack = coefficient(parameter("attack"), currentSampleRate);
    const auto release = coefficient(parameter("release"), currentSampleRate);
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
        static_cast<int>(std::round(parameter("samples"))));
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
    if (audio.getNumChannels() == 0 || audio.getNumSamples() < 64)
        return;
    const auto* samples = audio.getReadPointer(0);
    const auto minimumLag = static_cast<int>(currentSampleRate / 2000.0);
    const auto maximumLag = std::min(
        audio.getNumSamples() / 2,
        static_cast<int>(currentSampleRate / 50.0));
    auto bestLag = 0;
    auto bestCorrelation = 0.0;
    for (int lag = minimumLag; lag <= maximumLag; ++lag)
    {
        auto correlation = 0.0;
        for (int sample = 0;
             sample + lag < audio.getNumSamples();
             ++sample)
            correlation += samples[sample] * samples[sample + lag];
        if (correlation > bestCorrelation)
        {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }
    detectedFrequency.store(
        bestLag > 0 ? currentSampleRate / bestLag : 0.0,
        std::memory_order_relaxed);
}

void UtilityDeviceProcessor::processGenerator(
    juce::AudioBuffer<float>& audio) noexcept
{
    const auto waveform = static_cast<int>(std::round(parameter("waveform")));
    const auto frequency = parameter("frequency");
    const auto level =
        juce::Decibels::decibelsToGain(parameter("level"));
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

float UtilityDeviceProcessor::parameter(const juce::String& id) const
{
    const auto value = std::find_if(
        parameterLookup.cbegin(),
        parameterLookup.cend(),
        [&id](const auto& entry)
        {
            return entry.first == id;
        });
    return value != parameterLookup.cend() ? value->second->get() : 0.0f;
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

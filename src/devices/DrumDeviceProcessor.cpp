#include "DrumDeviceProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace studio
{
namespace
{
constexpr auto stateSchema = 1;

float decayFor(float seconds, double sampleRate)
{
    return static_cast<float>(
        std::exp(
            -1.0
            / (std::max(0.001f, seconds)
               * static_cast<float>(sampleRate))));
}

float wrapPhase(float phase) noexcept
{
    if (phase >= juce::MathConstants<float>::twoPi)
        phase -= juce::MathConstants<float>::twoPi;
    return phase;
}
}

juce::AudioProcessor::BusesProperties DrumDeviceProcessor::buses()
{
    return BusesProperties()
        .withOutput(outputBusName(mainOutput),
                    juce::AudioChannelSet::stereo(),
                    true)
        .withOutput(outputBusName(kickOutput),
                    juce::AudioChannelSet::stereo(),
                    true)
        .withOutput(outputBusName(snareOutput),
                    juce::AudioChannelSet::stereo(),
                    true)
        .withOutput(outputBusName(tomsOutput),
                    juce::AudioChannelSet::stereo(),
                    true)
        .withOutput(outputBusName(cymbalsOutput),
                    juce::AudioChannelSet::stereo(),
                    true);
}

DrumDeviceProcessor::DrumDeviceProcessor()
    : juce::AudioProcessor(buses())
{
    addFloat(ParameterSlot::mainLevel,
             "main",
             "Main level",
             { -60.0f, 12.0f },
             0.0f);
    addFloat(ParameterSlot::kickLevel,
             "kick",
             "Kick level",
             { -24.0f, 12.0f },
             0.0f);
    addFloat(ParameterSlot::snareLevel,
             "snare",
             "Snare level",
             { -24.0f, 12.0f },
             0.0f);
    addFloat(ParameterSlot::tomsLevel,
             "toms",
             "Toms level",
             { -24.0f, 12.0f },
             0.0f);
    addFloat(ParameterSlot::cymbalsLevel,
             "cymbals",
             "Cymbals level",
             { -24.0f, 12.0f },
             -2.0f);
    addFloat(ParameterSlot::room,
             "room",
             "Room",
             { 0.0f, 1.0f },
             0.18f);
    addFloat(ParameterSlot::tuning,
             "tuning",
             "Kit tuning",
             { -12.0f, 12.0f },
             0.0f);
    addFloat(ParameterSlot::velocityCurve,
             "velocityCurve",
             "Velocity curve",
             { 0.5f, 2.0f },
             1.0f);
}

juce::AudioParameterFloat* DrumDeviceProcessor::addFloat(
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

const juce::String DrumDeviceProcessor::getName() const
{
    return "Metal Drum Composer";
}

void DrumDeviceProcessor::prepareToPlay(double sampleRate,
                                        int maximumBlockSize)
{
    juce::ignoreUnused(maximumBlockSize);
    currentSampleRate = std::max(1.0, sampleRate);
    roomDelay.setSize(
        2,
        static_cast<int>(std::ceil(currentSampleRate * 0.079))
            + 1,
        false,
        true,
        false);
    reset();
}

void DrumDeviceProcessor::releaseResources()
{
}

void DrumDeviceProcessor::reset()
{
    for (auto& voice : voices)
        voice = {};
    roundRobinCounters.fill(0);
    roomDelay.clear();
    roomWritePosition = 0;
    hiHatFootControl = 1.0f;
    voiceOrdinal = 0;
}

void DrumDeviceProcessor::processBlock(
    juce::AudioBuffer<float>& audio,
    juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer(audio, false, mainOutput);
    auto kick = getBusBuffer(audio, false, kickOutput);
    auto snare = getBusBuffer(audio, false, snareOutput);
    auto toms = getBusBuffer(audio, false, tomsOutput);
    auto cymbals = getBusBuffer(audio, false, cymbalsOutput);
    main.clear();
    kick.clear();
    snare.clear();
    toms.clear();
    cymbals.clear();

    auto cursor = 0;
    for (const auto metadata : midi)
    {
        const auto position = juce::jlimit(
            cursor,
            audio.getNumSamples(),
            metadata.samplePosition);
        renderSamples(main,
                      kick,
                      snare,
                      toms,
                      cymbals,
                      cursor,
                      position - cursor);
        handleMidiMessage(metadata.getMessage());
        cursor = position;
    }
    renderSamples(main,
                  kick,
                  snare,
                  toms,
                  cymbals,
                  cursor,
                  audio.getNumSamples() - cursor);
}

void DrumDeviceProcessor::handleMidiMessage(
    const juce::MidiMessage& message) noexcept
{
    if (message.isNoteOn())
    {
        startVoice(message.getNoteNumber(), message.getFloatVelocity());
    }
    else if (message.isController() && message.getControllerNumber() == 4)
    {
        hiHatFootControl = juce::jlimit(
            0.0f,
            1.0f,
            static_cast<float>(message.getControllerValue()) / 127.0f);
        if (hiHatFootControl > 0.85f)
            chokeVoices(1, 96);
    }
}

DrumDeviceProcessor::HitDescription
DrumDeviceProcessor::describeHit(int note) const noexcept
{
    HitDescription hit;
    hit.valid = true;
    switch (note)
    {
        case 35:
            hit.kind = VoiceKind::kick;
            hit.outputBus = kickOutput;
            hit.roundRobinFamily = 0;
            hit.explicitVariant = 1;
            hit.frequency = 49.0f;
            hit.durationSeconds = 0.62f;
            hit.brightness = 0.48f;
            break;
        case 36:
        case 37:
            hit.kind = VoiceKind::kick;
            hit.outputBus = kickOutput;
            hit.roundRobinFamily = 0;
            hit.explicitVariant = note == 37 ? 1 : -1;
            hit.frequency = note == 37 ? 52.0f : 50.0f;
            hit.durationSeconds = 0.64f;
            hit.brightness = 0.5f;
            break;
        case 38:
        case 40:
            hit.kind = VoiceKind::snare;
            hit.outputBus = snareOutput;
            hit.roundRobinFamily = 1;
            hit.explicitVariant = note == 40 ? 1 : -1;
            hit.frequency = 185.0f;
            hit.durationSeconds = 0.72f;
            hit.brightness = 0.78f;
            break;
        case 39:
            hit.kind = VoiceKind::snare;
            hit.outputBus = snareOutput;
            hit.frequency = 245.0f;
            hit.durationSeconds = 0.32f;
            hit.brightness = 0.92f;
            break;
        case 41:
        case 43:
            hit.kind = VoiceKind::tom;
            hit.outputBus = tomsOutput;
            hit.roundRobinFamily = 2;
            hit.explicitVariant = note == 43 ? 1 : -1;
            hit.frequency = 82.0f;
            hit.durationSeconds = 1.05f;
            hit.brightness = 0.36f;
            break;
        case 45:
        case 47:
            hit.kind = VoiceKind::tom;
            hit.outputBus = tomsOutput;
            hit.roundRobinFamily = 3;
            hit.explicitVariant = note == 47 ? 1 : -1;
            hit.frequency = 116.0f;
            hit.durationSeconds = 0.88f;
            hit.brightness = 0.42f;
            break;
        case 48:
        case 50:
            hit.kind = VoiceKind::tom;
            hit.outputBus = tomsOutput;
            hit.roundRobinFamily = 4;
            hit.explicitVariant = note == 50 ? 1 : -1;
            hit.frequency = 155.0f;
            hit.durationSeconds = 0.72f;
            hit.brightness = 0.48f;
            break;
        case 42:
        case 44:
        case 46:
            hit.kind = VoiceKind::hat;
            hit.outputBus = cymbalsOutput;
            hit.chokeGroup = 1;
            hit.frequency = note == 44 ? 6900.0f : 7600.0f;
            hit.durationSeconds = note == 46
                ? 1.5f
                : note == 44 ? 0.22f : 0.12f;
            hit.brightness = note == 46 ? 0.82f : 0.68f;
            break;
        case 49:
        case 55:
            hit.kind = VoiceKind::cymbal;
            hit.outputBus = cymbalsOutput;
            hit.chokeGroup = 2;
            hit.roundRobinFamily = 5;
            hit.explicitVariant = note == 55 ? 1 : -1;
            hit.frequency = 510.0f;
            hit.durationSeconds = 3.2f;
            hit.brightness = 0.78f;
            break;
        case 57:
            hit.chokeOnly = true;
            hit.chokeGroup = 2;
            break;
        case 52:
        case 58:
            hit.kind = VoiceKind::cymbal;
            hit.outputBus = cymbalsOutput;
            hit.chokeGroup = 3;
            hit.roundRobinFamily = 6;
            hit.explicitVariant = note == 58 ? 1 : -1;
            hit.frequency = 365.0f;
            hit.durationSeconds = 2.8f;
            hit.brightness = 0.67f;
            break;
        case 59:
            hit.chokeOnly = true;
            hit.chokeGroup = 3;
            break;
        case 51:
        case 53:
            hit.kind = VoiceKind::cymbal;
            hit.outputBus = cymbalsOutput;
            hit.chokeGroup = 4;
            hit.roundRobinFamily = 7;
            hit.explicitVariant = note == 53 ? 1 : -1;
            hit.frequency = 430.0f;
            hit.durationSeconds = 3.8f;
            hit.brightness = 0.58f;
            break;
        case 56:
            hit.kind = VoiceKind::cymbal;
            hit.outputBus = cymbalsOutput;
            hit.chokeGroup = 4;
            hit.frequency = 850.0f;
            hit.durationSeconds = 2.2f;
            hit.brightness = 0.9f;
            break;
        case 60:
            hit.chokeOnly = true;
            hit.chokeGroup = 4;
            break;
        default:
            hit.valid = false;
            break;
    }
    return hit;
}

void DrumDeviceProcessor::startVoice(int note, float velocity) noexcept
{
    const auto hit = describeHit(note);
    if (!hit.valid)
        return;
    if (hit.chokeGroup > 0)
        chokeVoices(hit.chokeGroup, hit.chokeOnly ? 32 : 160);
    if (hit.chokeOnly)
        return;

    auto variant = hit.explicitVariant;
    if (variant < 0 && hit.roundRobinFamily >= 0)
    {
        auto& counter = roundRobinCounters[
            static_cast<std::size_t>(hit.roundRobinFamily)];
        variant = static_cast<int>(counter++ & 1U);
    }
    if (variant < 0)
        variant = 0;

    auto& voice = voiceForStart();
    voice = {};
    voice.active = true;
    voice.kind = hit.kind;
    voice.midiNote = note;
    voice.chokeGroup = hit.chokeGroup;
    voice.outputBus = hit.outputBus;
    const auto velocityValue = std::pow(
        juce::jlimit(0.0f, 1.0f, velocity),
        parameter(ParameterSlot::velocityCurve));
    voice.envelope = velocityValue;
    const auto tuning = std::pow(
        2.0f,
        parameter(ParameterSlot::tuning) / 12.0f);
    const auto variation = variant == 0 ? 0.992f : 1.008f;
    voice.frequency = hit.frequency * tuning * variation;
    voice.targetFrequency = voice.frequency
        * (hit.kind == VoiceKind::kick ? 0.48f : 0.94f);
    voice.frequencyTwo = voice.frequency
        * (hit.kind == VoiceKind::cymbal ? 1.4142f : 1.71f);
    auto duration = hit.durationSeconds;
    if (hit.kind == VoiceKind::hat)
    {
        const auto openness = 1.0f - hiHatFootControl;
        duration *= 0.35f + openness * 1.65f;
    }
    voice.decayMultiplier = decayFor(duration, currentSampleRate);
    voice.pan = (variant == 0 ? -1.0f : 1.0f)
        * (hit.kind == VoiceKind::cymbal ? 0.16f : 0.035f);
    voice.brightness = hit.brightness;
    voice.ordinal = ++voiceOrdinal;
    voice.noiseState = 0x9e3779b9U
        ^ (static_cast<std::uint32_t>(note) * 0x45d9f3bU)
        ^ static_cast<std::uint32_t>(voice.ordinal * 0x27d4eb2dU);
    if (voice.noiseState == 0)
        voice.noiseState = 1;
}

void DrumDeviceProcessor::chokeVoices(int chokeGroup,
                                      int releaseSamples) noexcept
{
    if (chokeGroup <= 0)
        return;
    const auto release = static_cast<float>(
        std::exp(-1.0 / std::max(1, releaseSamples)));
    for (auto& voice : voices)
    {
        if (voice.active && voice.chokeGroup == chokeGroup)
            voice.decayMultiplier = std::min(
                voice.decayMultiplier,
                release);
    }
}

void DrumDeviceProcessor::renderSamples(
    juce::AudioBuffer<float>& main,
    juce::AudioBuffer<float>& kick,
    juce::AudioBuffer<float>& snare,
    juce::AudioBuffer<float>& toms,
    juce::AudioBuffer<float>& cymbals,
    int startSample,
    int samples) noexcept
{
    if (samples <= 0)
        return;
    const std::array<float, outputBusCount> groupGains {
        1.0f,
        juce::Decibels::decibelsToGain(
            parameter(ParameterSlot::kickLevel)),
        juce::Decibels::decibelsToGain(
            parameter(ParameterSlot::snareLevel)),
        juce::Decibels::decibelsToGain(
            parameter(ParameterSlot::tomsLevel)),
        juce::Decibels::decibelsToGain(
            parameter(ParameterSlot::cymbalsLevel))
    };
    const auto mainGain = juce::Decibels::decibelsToGain(
        parameter(ParameterSlot::mainLevel));
    const auto roomAmount = parameter(ParameterSlot::room);
    std::array<juce::AudioBuffer<float>*, outputBusCount> outputs {
        &main,
        &kick,
        &snare,
        &toms,
        &cymbals
    };

    for (int sample = startSample; sample < startSample + samples; ++sample)
    {
        std::array<float, outputBusCount> left {};
        std::array<float, outputBusCount> right {};
        for (auto& voice : voices)
        {
            if (!voice.active)
                continue;
            const auto noise = nextNoise(voice.noiseState);
            voice.noiseLow += 0.075f * (noise - voice.noiseLow);
            const auto brightNoise = noise - voice.noiseLow;
            float value = 0.0f;
            switch (voice.kind)
            {
                case VoiceKind::kick:
                {
                    const auto click = brightNoise
                        * voice.envelope
                        * voice.envelope
                        * voice.brightness;
                    value = std::sin(voice.phase) * voice.envelope
                        + click * 0.32f;
                    voice.frequency +=
                        (voice.targetFrequency - voice.frequency)
                        * 0.0018f;
                    break;
                }
                case VoiceKind::snare:
                    value = (brightNoise * voice.brightness
                             + std::sin(voice.phase) * 0.42f)
                        * voice.envelope;
                    break;
                case VoiceKind::tom:
                    value = (std::sin(voice.phase)
                             + brightNoise * voice.brightness * 0.12f)
                        * voice.envelope;
                    voice.frequency +=
                        (voice.targetFrequency - voice.frequency)
                        * 0.00055f;
                    break;
                case VoiceKind::hat:
                    value = (brightNoise * 0.72f
                             + std::sin(voice.phase) * 0.13f
                             + std::sin(voice.phaseTwo) * 0.09f)
                        * voice.envelope
                        * voice.brightness;
                    break;
                case VoiceKind::cymbal:
                    value = (brightNoise * 0.36f
                             + std::sin(voice.phase) * 0.24f
                             + std::sin(voice.phaseTwo) * 0.19f)
                        * voice.envelope
                        * voice.brightness;
                    break;
            }

            voice.phase = wrapPhase(
                voice.phase
                + juce::MathConstants<float>::twoPi
                    * voice.frequency
                    / static_cast<float>(currentSampleRate));
            voice.phaseTwo = wrapPhase(
                voice.phaseTwo
                + juce::MathConstants<float>::twoPi
                    * voice.frequencyTwo
                    / static_cast<float>(currentSampleRate));
            voice.envelope *= voice.decayMultiplier;
            if (voice.envelope < 0.00003f
                || !std::isfinite(voice.envelope))
            {
                voice.active = false;
                continue;
            }

            const auto leftPan = voice.pan > 0.0f
                ? 1.0f - voice.pan
                : 1.0f;
            const auto rightPan = voice.pan < 0.0f
                ? 1.0f + voice.pan
                : 1.0f;
            const auto bus = static_cast<std::size_t>(voice.outputBus);
            left[bus] += value * leftPan;
            right[bus] += value * rightPan;
        }

        auto dryLeft = 0.0f;
        auto dryRight = 0.0f;
        for (int bus = kickOutput; bus < outputBusCount; ++bus)
        {
            const auto gain = groupGains[static_cast<std::size_t>(bus)];
            const auto busLeft = left[static_cast<std::size_t>(bus)] * gain;
            const auto busRight = right[static_cast<std::size_t>(bus)] * gain;
            dryLeft += busLeft;
            dryRight += busRight;
            auto* output = outputs[static_cast<std::size_t>(bus)];
            if (output->getNumChannels() > 0)
                output->setSample(0, sample, busLeft);
            if (output->getNumChannels() > 1)
                output->setSample(1, sample, busRight);
        }

        auto roomLeft = 0.0f;
        auto roomRight = 0.0f;
        if (roomDelay.getNumSamples() > 0)
        {
            roomLeft = roomDelay.getSample(0, roomWritePosition);
            roomRight = roomDelay.getSample(1, roomWritePosition);
            roomDelay.setSample(
                0,
                roomWritePosition,
                dryLeft + roomRight * (0.18f + roomAmount * 0.28f));
            roomDelay.setSample(
                1,
                roomWritePosition,
                dryRight + roomLeft * (0.18f + roomAmount * 0.28f));
            roomWritePosition =
                (roomWritePosition + 1) % roomDelay.getNumSamples();
        }
        if (main.getNumChannels() > 0)
            main.setSample(
                0,
                sample,
                (dryLeft + roomLeft * roomAmount * 0.32f)
                    * mainGain);
        if (main.getNumChannels() > 1)
            main.setSample(
                1,
                sample,
                (dryRight + roomRight * roomAmount * 0.32f)
                    * mainGain);
    }
}

DrumDeviceProcessor::Voice& DrumDeviceProcessor::voiceForStart() noexcept
{
    const auto inactive = std::find_if(
        voices.begin(),
        voices.end(),
        [](const auto& voice)
        {
            return !voice.active;
        });
    if (inactive != voices.end())
        return *inactive;
    return *std::min_element(
        voices.begin(),
        voices.end(),
        [](const auto& left, const auto& right)
        {
            if (left.envelope < right.envelope)
                return true;
            if (right.envelope < left.envelope)
                return false;
            return left.ordinal < right.ordinal;
        });
}

float DrumDeviceProcessor::nextNoise(std::uint32_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state)
            / static_cast<float>(
                std::numeric_limits<std::uint32_t>::max())
            * 2.0f
        - 1.0f;
}

float DrumDeviceProcessor::parameter(ParameterSlot slot) const noexcept
{
    const auto* value =
        realtimeParameters[static_cast<std::size_t>(slot)];
    return value != nullptr ? value->get() : 0.0f;
}

double DrumDeviceProcessor::getTailLengthSeconds() const
{
    return 4.0;
}

bool DrumDeviceProcessor::acceptsMidi() const
{
    return true;
}

bool DrumDeviceProcessor::producesMidi() const
{
    return false;
}

juce::AudioProcessorEditor* DrumDeviceProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}

bool DrumDeviceProcessor::hasEditor() const
{
    return true;
}

int DrumDeviceProcessor::getNumPrograms()
{
    return 1;
}

int DrumDeviceProcessor::getCurrentProgram()
{
    return 0;
}

void DrumDeviceProcessor::setCurrentProgram(int)
{
}

const juce::String DrumDeviceProcessor::getProgramName(int)
{
    return "Basic Metal Kit";
}

void DrumDeviceProcessor::changeProgramName(int, const juce::String&)
{
}

void DrumDeviceProcessor::getStateInformation(
    juce::MemoryBlock& destination)
{
    if (saveValidatedState(destination).failed())
        destination.reset();
}

void DrumDeviceProcessor::setStateInformation(const void* data, int size)
{
    restoreValidatedState(data, size);
}

juce::Result DrumDeviceProcessor::saveValidatedState(
    juce::MemoryBlock& destination)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("schema", stateSchema);
    object->setProperty("device", "studio.device.drum-composer");
    for (const auto& [id, value] : parameterLookup)
    {
        object->setProperty(
            id,
            static_cast<juce::AudioProcessorParameter*>(value)
                ->getValue());
    }
    const auto json = juce::JSON::toString(
        juce::var(object.release()),
        false);
    destination.replaceAll(
        json.toRawUTF8(),
        static_cast<std::size_t>(json.getNumBytesAsUTF8()));
    return destination.isEmpty()
        ? juce::Result::fail("Could not serialize drum device state.")
        : juce::Result::ok();
}

juce::Result DrumDeviceProcessor::restoreValidatedState(
    const void* data,
    int size)
{
    if (data == nullptr || size <= 0)
        return juce::Result::fail("Drum device state is empty.");
    const auto value = juce::JSON::parse(
        juce::String::fromUTF8(
            static_cast<const char*>(data),
            size));
    const auto* object = value.getDynamicObject();
    if (object == nullptr
        || static_cast<int>(object->getProperty("schema"))
            != stateSchema
        || object->getProperty("device").toString()
            != "studio.device.drum-composer")
    {
        return juce::Result::fail(
            "Drum device state has an unsupported format.");
    }

    std::vector<std::pair<juce::AudioParameterFloat*, float>> values;
    values.reserve(parameterLookup.size());
    for (const auto& [id, parameterValue] : parameterLookup)
    {
        const auto saved = object->getProperty(id);
        if (!(saved.isInt()
              || saved.isInt64()
              || saved.isDouble()))
        {
            return juce::Result::fail(
                "Drum device state is missing parameter " + id + ".");
        }
        const auto normalized = static_cast<float>(
            static_cast<double>(saved));
        if (!std::isfinite(normalized)
            || normalized < 0.0f
            || normalized > 1.0f)
        {
            return juce::Result::fail(
                "Drum device state contains an invalid parameter value.");
        }
        values.emplace_back(parameterValue, normalized);
    }
    for (const auto& [parameterValue, normalized] : values)
    {
        static_cast<juce::AudioProcessorParameter*>(parameterValue)
            ->setValue(normalized);
    }
    reset();
    return juce::Result::ok();
}

bool DrumDeviceProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const
{
    if (!layouts.getMainInputChannelSet().isDisabled()
        || layouts.outputBuses.size() != outputBusCount
        || layouts.getMainOutputChannelSet()
            != juce::AudioChannelSet::stereo())
        return false;
    for (int bus = 1; bus < layouts.outputBuses.size(); ++bus)
    {
        const auto channels = layouts.getChannelSet(false, bus);
        if (!channels.isDisabled()
            && channels != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

juce::String DrumDeviceProcessor::outputBusName(int index)
{
    switch (index)
    {
        case mainOutput: return "Main";
        case kickOutput: return "Kick";
        case snareOutput: return "Snare";
        case tomsOutput: return "Toms";
        case cymbalsOutput: return "Cymbals";
        default: return "Output";
    }
}
}

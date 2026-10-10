#include "DrumDeviceProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace studio
{
namespace
{
constexpr auto stateSchema = 3;
constexpr auto hiHatFootController = 4;
constexpr auto hiHatClosedThreshold = 0.85f;
constexpr auto voiceSilenceThreshold = 0.00003f;
constexpr auto maximumVoiceLifetimeSeconds = 3.8;
constexpr auto roomTailAllowanceSeconds = 0.2;
constexpr auto reportedTailSeconds =
    maximumVoiceLifetimeSeconds + roomTailAllowanceSeconds;

struct DrumTone
{
    float pitch = 1.0f;
    float decay = 1.0f;
    float brightness = 1.0f;
    float level = 1.0f;
    float drive = 0.0f;
};

struct SoundPreset
{
    const char* id;
    const char* name;
    std::array<DrumTone, 5> tones {};
    float roomScale = 1.0f;
};

// Tone rows follow VoiceKind: kick, snare, tom, hat, cymbal.
constexpr std::array soundPresets {
    SoundPreset { "basic-metal", "Basic Metal Kit" },
    SoundPreset { "punk", "Punk", {{
        { 1.15f, 1.20f, 0.50f, 0.95f, 0.40f },
        { 1.18f, 1.32f, 0.84f, 1.00f, 0.70f },
        { 1.08f, 1.20f, 0.80f, 0.92f, 0.20f },
        { 0.92f, 1.18f, 0.88f, 1.02f, 0.00f },
        { 0.92f, 1.12f, 0.90f, 1.06f, 0.00f }
    }}, 1.55f },
    SoundPreset { "hardcore-punk", "Hardcore Punk", {{
        { 1.08f, 0.72f, 0.85f, 1.10f, 1.50f },
        { 1.36f, 0.65f, 1.15f, 1.12f, 1.80f },
        { 1.18f, 0.78f, 0.96f, 1.02f, 0.80f },
        { 1.08f, 0.75f, 1.15f, 1.06f, 0.00f },
        { 1.05f, 0.78f, 1.05f, 0.98f, 0.00f }
    }}, 0.75f },
    SoundPreset { "death-metal", "Death Metal", {{
        { 0.86f, 0.65f, 1.95f, 1.12f, 1.10f },
        { 1.04f, 0.72f, 1.28f, 1.06f, 1.00f },
        { 0.88f, 0.66f, 1.24f, 1.08f, 0.45f },
        { 1.03f, 0.62f, 1.20f, 0.90f, 0.00f },
        { 1.10f, 0.70f, 1.10f, 0.88f, 0.00f }
    }}, 0.42f },
    SoundPreset { "modern-metal", "Modern Metal", {{
        { 0.98f, 0.90f, 1.38f, 1.18f, 1.25f },
        { 0.91f, 0.88f, 1.06f, 1.12f, 0.90f },
        { 0.97f, 0.92f, 1.06f, 1.10f, 0.50f },
        { 0.97f, 0.84f, 0.98f, 0.88f, 0.00f },
        { 0.98f, 0.90f, 0.96f, 0.93f, 0.00f }
    }}, 0.70f },
    SoundPreset { "deathcore", "Deathcore", {{
        { 0.76f, 0.82f, 2.50f, 1.22f, 1.60f },
        { 0.82f, 0.58f, 1.48f, 1.15f, 1.75f },
        { 0.73f, 0.78f, 1.38f, 1.18f, 1.00f },
        { 1.14f, 0.50f, 1.35f, 0.82f, 0.00f },
        { 0.85f, 0.65f, 1.15f, 0.90f, 0.00f }
    }}, 0.28f }
};

float decayForLifetime(float seconds, double sampleRate)
{
    const auto lifetimeSamples = std::max(
        1.0,
        static_cast<double>(seconds) * sampleRate);
    return static_cast<float>(
        std::exp(
            std::log(static_cast<double>(voiceSilenceThreshold))
            / lifetimeSamples));
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
    auto preset = std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("kitPreset", 1),
        "Kit sound preset",
        soundPresetNames(),
        0);
    kitPreset = preset.get();
    addParameter(preset.release());
    const auto& tuningParameters = kitTuningParameters();
    for (std::size_t index = 0; index < tuningParameters.size(); ++index)
    {
        const auto& control = tuningParameters[index];
        addFloat(static_cast<ParameterSlot>(
                     static_cast<std::size_t>(ParameterSlot::firstDrumTuning) + index),
                 control.id, control.name,
                 { control.minimum, control.maximum }, control.defaultValue, control.unit);
    }
}

juce::AudioParameterFloat* DrumDeviceProcessor::addFloat(
    ParameterSlot slot,
    const juce::String& id,
    const juce::String& name,
    juce::NormalisableRange<float> range,
    float defaultValue,
    const juce::String& unit)
{
    auto value = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(id, 1),
        name,
        range,
        defaultValue,
        juce::AudioParameterFloatAttributes().withLabel(unit));
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
    else if (message.isController()
             && message.getControllerNumber() == hiHatFootController)
    {
        hiHatFootControl = juce::jlimit(
            0.0f,
            1.0f,
            static_cast<float>(message.getControllerValue()) / 127.0f);
        if (hiHatFootControl > hiHatClosedThreshold)
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
            hit.durationSeconds = static_cast<float>(
                maximumVoiceLifetimeSeconds);
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
    const auto& preset = soundPresets[
        static_cast<std::size_t>(kitPreset->getIndex())];
    const auto& tone = preset.tones[static_cast<std::size_t>(hit.kind)];
    const auto drum = tunableDrumForNote(note);
    const auto batter = drum >= 0 ? drumTuning(drum, 0) : 0.0f;
    const auto resonant = drum >= 0 ? drumTuning(drum, 1) : 0.0f;
    const auto damping = drum >= 0 ? drumTuning(drum, 2) / 100.0f : 0.0f;
    const auto variation = variant == 0 ? 0.992f : 1.008f;
    voice.frequency = hit.frequency * tuning * variation * tone.pitch;
    const auto resonantFrequency = voice.frequency * 1.71f * std::pow(2.0f, resonant / 12.0f);
    if (drum >= 0)
        voice.frequency *= std::pow(2.0f, batter / 12.0f);
    voice.targetFrequency = voice.frequency
        * (hit.kind == VoiceKind::kick ? 0.48f : 0.94f);
    voice.frequencyTwo = voice.frequency
        * (hit.kind == VoiceKind::cymbal ? 1.4142f : 1.71f);
    if (drum >= 0)
    {
        voice.frequencyTwo = resonantFrequency;
        voice.resonance = std::abs(resonant) / 12.0f * 0.22f * (1.0f - damping);
    }
    auto duration = hit.durationSeconds;
    if (hit.kind == VoiceKind::hat)
    {
        const auto openness = 1.0f - hiHatFootControl;
        duration *= 0.35f + openness * 1.65f;
    }
    const auto sustain = std::pow(2.0f, -resonant / 24.0f) * (1.0f - damping * 0.85f);
    voice.decayMultiplier = decayForLifetime(
        std::min(duration * tone.decay * sustain,
                 static_cast<float>(maximumVoiceLifetimeSeconds)),
        currentSampleRate);
    voice.pan = (variant == 0 ? -1.0f : 1.0f)
        * (hit.kind == VoiceKind::cymbal ? 0.16f : 0.035f);
    voice.brightness = hit.brightness * tone.brightness;
    voice.drive = tone.drive;
    voice.gain = tone.level;
    if (hit.kind == VoiceKind::snare)
    {
        const auto wires = parameter(static_cast<ParameterSlot>(
            static_cast<int>(ParameterSlot::count) - 1)) / 100.0f;
        voice.wireLevel = std::min(wires * 2.0f, 1.0f)
            * (1.0f - std::max(0.0f, wires - 0.5f) * 0.6f);
        voice.wireDecay = 1.0f + std::max(0.0f, wires - 0.5f) * 1.5f;
    }
    if (tone.drive > 0.0f)
        voice.gain /= std::tanh(tone.drive);
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
    const auto& preset = soundPresets[
        static_cast<std::size_t>(kitPreset->getIndex())];
    const auto roomAmount = std::min(
        1.0f, parameter(ParameterSlot::room) * preset.roomScale);
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
                {
                    const auto wireEnvelope = std::abs(voice.wireDecay - 1.0f) > 0.000001f
                        ? std::pow(voice.envelope, voice.wireDecay - 1.0f) : 1.0f;
                    value = (brightNoise * voice.brightness * voice.wireLevel * wireEnvelope
                             + std::sin(voice.phase) * 0.42f) * voice.envelope;
                    break;
                }
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
            if (voice.resonance > 0.0f)
                value += std::sin(voice.phaseTwo) * voice.envelope * voice.resonance;
            if (voice.drive > 0.0f)
                value = std::tanh(value * voice.drive);
            value *= voice.gain;

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
            if (voice.envelope < voiceSilenceThreshold
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

float DrumDeviceProcessor::drumTuning(int drum, int control) const noexcept
{
    return parameter(static_cast<ParameterSlot>(
        static_cast<int>(ParameterSlot::firstDrumTuning) + drum * 3 + control));
}

double DrumDeviceProcessor::getTailLengthSeconds() const
{
    return reportedTailSeconds;
}

#if STUDIO_DUO_TESTING
int DrumDeviceProcessor::activeVoiceCountForTesting() const noexcept
{
    return static_cast<int>(std::count_if(
        voices.cbegin(),
        voices.cend(),
        [](const auto& voice)
        {
            return voice.active;
        }));
}
#endif

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
    return static_cast<int>(soundPresets.size());
}

int DrumDeviceProcessor::getCurrentProgram()
{
    return kitPreset->getIndex();
}

void DrumDeviceProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, getNumPrograms()))
    {
        juce::Logger::writeToLog(
            "Metal Drum Composer: invalid sound preset index "
            + juce::String(index) + ".");
        return;
    }
    *kitPreset = index;
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

const juce::String DrumDeviceProcessor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms())
        ? soundPresetNames()[index] : juce::String();
}

void DrumDeviceProcessor::changeProgramName(int, const juce::String&)
{
}

const juce::StringArray& DrumDeviceProcessor::soundPresetNames()
{
    static const auto names = []
    {
        juce::StringArray result;
        for (const auto& preset : soundPresets)
            result.add(preset.name);
        return result;
    }();
    return names;
}

const std::array<DrumDeviceProcessor::KitTuningParameter, DrumDeviceProcessor::tuningParameterCount>&
DrumDeviceProcessor::kitTuningParameters()
{
    static constexpr std::array<KitTuningParameter, tuningParameterCount> controls {{
        { "kickBatter", "Kick batter head", 0, -12.0f, 12.0f, 0.0f, "st" },
        { "kickResonant", "Kick resonant head", 0, -12.0f, 12.0f, 0.0f, "st" },
        { "kickDamping", "Kick damping", 0, 0.0f, 100.0f, 0.0f, "%" },
        { "snareBatter", "Snare batter head", 1, -12.0f, 12.0f, 0.0f, "st" },
        { "snareResonant", "Snare resonant head", 1, -12.0f, 12.0f, 0.0f, "st" },
        { "snareDamping", "Snare damping", 1, 0.0f, 100.0f, 0.0f, "%" },
        { "floorTomBatter", "Floor tom batter head", 2, -12.0f, 12.0f, 0.0f, "st" },
        { "floorTomResonant", "Floor tom resonant head", 2, -12.0f, 12.0f, 0.0f, "st" },
        { "floorTomDamping", "Floor tom damping", 2, 0.0f, 100.0f, 0.0f, "%" },
        { "midTomBatter", "Mid tom batter head", 3, -12.0f, 12.0f, 0.0f, "st" },
        { "midTomResonant", "Mid tom resonant head", 3, -12.0f, 12.0f, 0.0f, "st" },
        { "midTomDamping", "Mid tom damping", 3, 0.0f, 100.0f, 0.0f, "%" },
        { "highTomBatter", "High tom batter head", 4, -12.0f, 12.0f, 0.0f, "st" },
        { "highTomResonant", "High tom resonant head", 4, -12.0f, 12.0f, 0.0f, "st" },
        { "highTomDamping", "High tom damping", 4, 0.0f, 100.0f, 0.0f, "%" },
        { "snareWires", "Snare-wire tension", 1, 0.0f, 100.0f, 50.0f, "%" }
    }};
    return controls;
}

const juce::StringArray& DrumDeviceProcessor::tunableDrumNames()
{
    static const juce::StringArray names { "Kick", "Snare", "Floor tom", "Mid tom", "High tom" };
    return names;
}

int DrumDeviceProcessor::tunableDrumForNote(int note) noexcept
{
    switch (note)
    {
        case 35: case 36: case 37: return 0;
        case 38: case 39: case 40: return 1;
        case 41: case 43: return 2;
        case 45: case 47: return 3;
        case 48: case 50: return 4;
        default: return -1;
    }
}

int DrumDeviceProcessor::tunableDrumNote(int drum) noexcept
{
    constexpr std::array notes { 36, 38, 41, 45, 48 };
    return juce::isPositiveAndBelow(drum, tunableDrumCount)
        ? notes[static_cast<std::size_t>(drum)] : -1;
}

void DrumDeviceProcessor::getStateInformation(
    juce::MemoryBlock& destination)
{
    if (saveValidatedState(destination).failed())
        destination.reset();
}

void DrumDeviceProcessor::setStateInformation(const void* data, int size)
{
    if (const auto result = restoreValidatedState(data, size); result.failed())
        juce::Logger::writeToLog(
            "Metal Drum Composer: " + result.getErrorMessage());
}

juce::Result DrumDeviceProcessor::saveValidatedState(
    juce::MemoryBlock& destination)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("schema", stateSchema);
    object->setProperty("device", "studio.device.drum-composer");
    object->setProperty("kitPreset",
                       soundPresets[static_cast<std::size_t>(kitPreset->getIndex())].id);
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
    const auto schema = object != nullptr
        ? object->getProperty("schema") : juce::var();
    if (object == nullptr
        || !(schema.isInt() || schema.isInt64())
        || static_cast<juce::int64>(schema) < 1
        || static_cast<juce::int64>(schema) > stateSchema
        || object->getProperty("device").toString()
            != "studio.device.drum-composer")
    {
        return juce::Result::fail(
            "Drum device state has an unsupported format.");
    }

    auto presetIndex = 0;
    if (static_cast<juce::int64>(schema) >= 2)
    {
        const auto savedPreset = object->getProperty("kitPreset");
        if (!savedPreset.isString())
            return juce::Result::fail(
                "Drum device state is missing a valid kit sound preset.");
        const auto preset = std::find_if(
            soundPresets.cbegin(), soundPresets.cend(),
            [&savedPreset](const auto& candidate)
            {
                return savedPreset.toString() == candidate.id;
            });
        if (preset == soundPresets.cend())
            return juce::Result::fail(
                "Drum device state names an unknown kit sound preset: "
                + savedPreset.toString() + ".");
        presetIndex = static_cast<int>(std::distance(soundPresets.cbegin(), preset));
    }

    std::vector<std::pair<juce::AudioParameterFloat*, float>> values;
    values.reserve(parameterLookup.size());
    for (std::size_t index = 0; index < parameterLookup.size(); ++index)
    {
        const auto& [id, parameterValue] = parameterLookup[index];
        if (static_cast<juce::int64>(schema) < stateSchema
            && index >= static_cast<std::size_t>(ParameterSlot::firstDrumTuning))
        {
            values.emplace_back(parameterValue,
                static_cast<juce::AudioProcessorParameter*>(parameterValue)->getDefaultValue());
            continue;
        }
        const auto saved = object->getProperty(id);
        if (!(saved.isInt()
              || saved.isInt64()
              || saved.isDouble()))
        {
            return juce::Result::fail(
                "Drum device state is missing parameter " + id + ".");
        }
        const auto normalized = static_cast<double>(saved);
        if (!std::isfinite(normalized)
            || normalized < 0.0f
            || normalized > 1.0f)
        {
            return juce::Result::fail(
                "Drum device state contains an invalid parameter value.");
        }
        values.emplace_back(parameterValue, static_cast<float>(normalized));
    }
    for (const auto& [parameterValue, normalized] : values)
    {
        static_cast<juce::AudioProcessorParameter*>(parameterValue)
            ->setValue(normalized);
    }
    static_cast<juce::AudioProcessorParameter*>(kitPreset)->setValue(
        kitPreset->convertTo0to1(static_cast<float>(presetIndex)));
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

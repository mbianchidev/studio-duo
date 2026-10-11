#include "TestHarness.h"
#include "TestSuites.h"

#include "audio/StudioAudioEngine.h"
#include "devices/AmpDeviceProcessor.h"
#include "devices/DeviceRegistry.h"
#include "devices/DrumDeviceProcessor.h"
#include "midi/MidiEditing.h"
#include "model/ProjectCommands.h"
#include "model/ProjectModel.h"
#include "model/ProjectTemplates.h"
#include "plugin_host/ClapPluginFormat.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

namespace
{
bool setParameter(juce::AudioProcessor& processor,
                  const juce::String& id,
                  float normalized)
{
    for (auto* parameter : processor.getParameters())
    {
        const auto* identified =
            dynamic_cast<juce::AudioProcessorParameterWithID*>(
                parameter);
        if (identified != nullptr && identified->paramID == id)
        {
            parameter->setValueNotifyingHost(normalized);
            return true;
        }
    }
    return false;
}

float magnitude(const juce::AudioBuffer<float>& audio,
                int channel,
                int start = 0,
                int samples = -1)
{
    if (channel < 0 || channel >= audio.getNumChannels())
        return 0.0f;
    const auto count = samples < 0
        ? audio.getNumSamples() - start
        : std::min(samples, audio.getNumSamples() - start);
    return count > 0 ? audio.getMagnitude(channel, start, count) : 0.0f;
}

float maximumDifference(const juce::AudioBuffer<float>& first,
                        const juce::AudioBuffer<float>& second)
{
    if (first.getNumChannels() != second.getNumChannels()
        || first.getNumSamples() != second.getNumSamples())
        return std::numeric_limits<float>::infinity();
    auto difference = 0.0f;
    for (int channel = 0; channel < first.getNumChannels(); ++channel)
        for (int sample = 0; sample < first.getNumSamples(); ++sample)
        {
            const auto residual = std::abs(first.getSample(channel, sample)
                                           - second.getSample(channel, sample));
            if (!std::isfinite(residual))
                return std::numeric_limits<float>::infinity();
            difference = std::max(
                difference, residual);
        }
    return difference;
}

bool sameFloatBits(float first, float second)
{
    return std::bit_cast<std::uint32_t>(first)
        == std::bit_cast<std::uint32_t>(second);
}

double relativeTimbreDifference(const juce::AudioBuffer<float>& first,
                               const juce::AudioBuffer<float>& second, int channel)
{
    const auto* left = first.getReadPointer(channel);
    const auto* right = second.getReadPointer(channel);
    auto leftEnergy = 0.0;
    auto rightEnergy = 0.0;
    auto product = 0.0;
    for (int sample = 0; sample < first.getNumSamples(); ++sample)
    {
        leftEnergy += static_cast<double>(left[sample]) * left[sample];
        rightEnergy += static_cast<double>(right[sample]) * right[sample];
        product += static_cast<double>(left[sample]) * right[sample];
    }
    const auto gain = rightEnergy > 0.0 ? product / rightEnergy : 0.0;
    auto difference = 0.0;
    for (int sample = 0; sample < first.getNumSamples(); ++sample)
    {
        const auto residual = left[sample] - right[sample] * gain;
        difference += residual * residual;
    }
    return leftEnergy > 0.0 ? difference / leftEnergy : 0.0;
}

int outputChannel(const juce::AudioProcessor& processor,
                  int bus,
                  int channel)
{
    return processor.getChannelIndexInProcessBlockBuffer(
        false,
        bus,
        channel);
}

bool waitForRuntime(studio::StudioAudioEngine& engine)
{
    for (int attempt = 0;
         attempt < 200 && engine.pluginRuntimeTransitionPending();
         ++attempt)
        juce::Thread::sleep(10);
    return !engine.pluginRuntimeTransitionPending();
}

bool waitForFlag(const std::atomic<bool>& flag)
{
    for (int attempt = 0;
         attempt < 2000 && !flag.load(std::memory_order_acquire);
         ++attempt)
        juce::Thread::sleep(1);
    return flag.load(std::memory_order_acquire);
}

studio::StudioAudioEngine::PluginRuntimeRequest requestFor(
    const studio::Track& track,
    const studio::PluginInsert& insert,
    juce::MemoryBlock state = {})
{
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = track.id;
    request.insertId = insert.id;
    request.name = insert.name;
    request.deviceIdentifier = insert.pluginIdentifier;
    request.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    request.state = std::move(state);
    return request;
}

studio::Track& addInstrumentTrack(studio::Project& project,
                                  const juce::String& name)
{
    studio::Track track;
    track.name = name;
    track.type = studio::TrackType::instrument;
    project.tracks.insert(project.tracks.end() - 1, track);
    return *project.findTrack(track.id);
}

studio::PluginInsert addBundledInsert(
    studio::Track& track,
    const juce::String& identifier,
    const juce::String& name)
{
    studio::PluginInsert insert;
    insert.pluginIdentifier = identifier;
    insert.name = name;
    insert.manufacturer = "Studio Duo";
    insert.format = "Studio Duo";
    insert.bundledDevice = true;
    insert.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    track.inserts.push_back(insert);
    return insert;
}

juce::AudioBuffer<float> sineInput(int samples,
                                   double frequency = 110.0)
{
    juce::AudioBuffer<float> input(2, samples);
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto value = static_cast<float>(
            std::sin(
                juce::MathConstants<double>::twoPi
                * frequency
                * static_cast<double>(sample)
                / 48000.0)
            * 0.18);
        input.setSample(0, sample, value);
        input.setSample(1, sample, value);
    }
    return input;
}

bool writeCabinetIr(const juce::File& file, int samples)
{
    if (file.existsAsFile())
        file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream =
        file.createOutputStream();
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate(48000.0)
            .withNumChannels(1)
            .withBitsPerSample(24));
    if (writer == nullptr)
        return false;
    juce::AudioBuffer<float> impulse(1, samples);
    impulse.clear();
    impulse.setSample(0, 0, 0.8f);
    for (int sample = 1; sample < samples; ++sample)
    {
        impulse.setSample(
            0,
            sample,
            static_cast<float>(
                std::sin(
                    juce::MathConstants<double>::twoPi
                    * 2200.0
                    * static_cast<double>(sample)
                    / 48000.0)
                * std::exp(-static_cast<double>(sample) / 72.0)
                * 0.16));
    }
    return writer->writeFromAudioSampleBuffer(
        impulse,
        0,
        impulse.getNumSamples());
}

void registryMetadata()
{
    const auto* drums = studio::DeviceRegistry::descriptor(
        "studio.device.drum-composer");
    const auto* guitar = studio::DeviceRegistry::descriptor(
        "studio.device.guitar-amp");
    const auto* bass = studio::DeviceRegistry::descriptor(
        "studio.device.bass-amp");
    expect(drums != nullptr
               && drums->instrument
               && drums->inputChannels == 0
               && drums->outputChannels == 10
               && drums->outputBuses
                      == std::vector<juce::String> {
                          "Main", "Kick", "Snare", "Toms", "Cymbals"
                      },
           "The bundled registry advertises the drum instrument and its five stereo buses.");
    expect(guitar != nullptr
               && bass != nullptr
               && !guitar->instrument
               && !bass->instrument
               && guitar->category == "Amp"
               && bass->category == "Amp",
           "The bundled registry advertises both cabinet amp effects.");
}

void drumSoundPresetPrograms()
{
    studio::DrumDeviceProcessor drum;
    const std::array<juce::String, 6> names {
        "Basic Metal Kit", "Punk", "Hardcore Punk",
        "Death Metal", "Modern Metal", "Deathcore"
    };
    expect(drum.getNumPrograms() == static_cast<int>(names.size())
               && drum.getCurrentProgram() == 0,
           "Drums expose the original kit plus five genre sound presets, keeping the original default.");
    for (int index = 0; index < static_cast<int>(names.size()); ++index)
    {
        drum.setCurrentProgram(index);
        expect(drum.getProgramName(index) == names[static_cast<std::size_t>(index)]
                   && drum.getCurrentProgram() == index,
               "Each named drum sound preset is selectable through the processor program interface.");
    }
    expect(drum.getParameters().size() == 25
               && drum.getParameters()[8]->isDiscrete()
               && drum.getParameters()[8]->getNumSteps() == 6
               && setParameter(drum, "kitPreset", 0.4f)
               && drum.getCurrentProgram() == 2,
           "Kit sound is a discrete host parameter appended after the eight existing mix controls.");
    drum.setCurrentProgram(-1);
    drum.setCurrentProgram(static_cast<int>(names.size()));
    expect(drum.getCurrentProgram() == 2,
           "Out-of-range drum programs cannot change the selected kit.");
}

void drumKitTuningParameters()
{
    studio::DrumDeviceProcessor drum;
    const std::array<juce::String, 16> ids {
        "kickBatter", "kickResonant", "kickDamping",
        "snareBatter", "snareResonant", "snareDamping",
        "floorTomBatter", "floorTomResonant", "floorTomDamping",
        "midTomBatter", "midTomResonant", "midTomDamping",
        "highTomBatter", "highTomResonant", "highTomDamping",
        "snareWires"
    };
    expect(drum.getParameters().size() == 25,
           "Five drum shells expose independent batter/resonant head tuning and damping, plus snare-wire tension.");
    if (drum.getParameters().size() != 25)
        return;
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterFloat*>(
            drum.getParameters()[9 + static_cast<int>(index)]);
        expect(parameter != nullptr && parameter->paramID == ids[index]
                   && parameter->isAutomatable(),
               "Physical kit controls append stable, automatable IDs after the existing nine parameters.");
        if (parameter == nullptr)
            continue;
        const auto head = ids[index].endsWith("Batter") || ids[index].endsWith("Resonant");
        expect(sameFloatBits(parameter->range.start, head ? -12.0f : 0.0f)
                   && sameFloatBits(parameter->range.end, head ? 12.0f : 100.0f)
                   && sameFloatBits(parameter->get(), ids[index] == "snareWires" ? 50.0f : 0.0f),
               "Heads tune one octave either way, damping/wires use percent, and defaults preserve the original kit.");
    }
}

juce::AudioBuffer<float> renderDrumHit(studio::DrumDeviceProcessor& drum, int note,
                                     int samples = 24000)
{
    drum.reset();
    juce::AudioBuffer<float> audio(drum.getTotalNumOutputChannels(), samples);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(120)), 0);
    drum.processBlock(audio, midi);
    return audio;
}

float drumFundamental(const juce::AudioBuffer<float>& audio)
{
    constexpr auto fftOrder = 14;
    constexpr auto fftSize = 1 << fftOrder;
    juce::dsp::FFT transform(fftOrder);
    std::vector<float> spectrum(static_cast<std::size_t>(fftSize) * 2, 0.0f);
    for (int sample = 0; sample < fftSize; ++sample)
    {
        const auto window = 0.5f - 0.5f * std::cos(
            juce::MathConstants<float>::twoPi * static_cast<float>(sample)
            / static_cast<float>(fftSize - 1));
        spectrum[static_cast<std::size_t>(sample)] = audio.getSample(0, sample + 1024) * window;
    }
    transform.performFrequencyOnlyForwardTransform(spectrum.data());
    const auto peak = std::max_element(spectrum.cbegin() + 6, spectrum.cbegin() + 160);
    return static_cast<float>(std::distance(spectrum.cbegin(), peak))
        * 48000.0f / static_cast<float>(fftSize);
}

void drumKitHeadPitchAndIsolation()
{
    studio::DrumDeviceProcessor drum;
    studio::DrumDeviceProcessor original;
    drum.prepareToPlay(48000.0, 24000);
    original.prepareToPlay(48000.0, 24000);
    setParameter(drum, "room", 0.0f);
    setParameter(original, "room", 0.0f);
    const std::array<juce::String, 5> ids {
        "kickBatter", "snareBatter", "floorTomBatter", "midTomBatter", "highTomBatter"
    };
    constexpr std::array notes { 36, 38, 41, 45, 48 };
    for (std::size_t piece = 0; piece < notes.size(); ++piece)
    {
        const auto before = renderDrumHit(original, notes[piece]);
        expect(setParameter(drum, ids[piece], 1.0f),
               "Each shell's batter-head tension is directly editable.");
        const auto tuned = renderDrumHit(drum, notes[piece]);
        const auto ratio = drumFundamental(tuned) / drumFundamental(before);
        expect(ratio > 1.9f && ratio < 2.1f,
               "Raising one batter head by twelve semitones doubles that drum's audible fundamental.");
        for (const auto untouched : { 36, 38, 41, 45, 48, 42, 49 })
            if (untouched != notes[piece])
                expect(maximumDifference(renderDrumHit(drum, untouched, 2048),
                                         renderDrumHit(original, untouched, 2048)) < 0.000001f,
                       "Tuning one shell leaves every other drum and cymbal unchanged.");
        setParameter(drum, ids[piece], 0.5f);
    }
}

double drumWireEnergy(const juce::AudioBuffer<float>& audio, int start, int samples)
{
    auto energy = 0.0;
    for (int sample = start + 1; sample < start + samples; ++sample)
    {
        const auto difference = static_cast<double>(audio.getSample(0, sample))
            - audio.getSample(0, sample - 1);
        energy += difference * difference;
    }
    return energy / static_cast<double>(samples);
}

void drumKitResonanceDampingAndWires()
{
    studio::DrumDeviceProcessor drum;
    drum.prepareToPlay(48000.0, 24000);
    setParameter(drum, "room", 0.0f);
    const std::array<juce::String, 5> resonantIds {
        "kickResonant", "snareResonant", "floorTomResonant", "midTomResonant", "highTomResonant"
    };
    const std::array<juce::String, 5> dampingIds {
        "kickDamping", "snareDamping", "floorTomDamping", "midTomDamping", "highTomDamping"
    };
    constexpr std::array notes { 36, 38, 41, 45, 48 };
    for (std::size_t piece = 0; piece < notes.size(); ++piece)
    {
        const auto normal = renderDrumHit(drum, notes[piece]);
        setParameter(drum, resonantIds[piece], 1.0f);
        const auto tight = renderDrumHit(drum, notes[piece]);
        setParameter(drum, resonantIds[piece], 0.0f);
        const auto loose = renderDrumHit(drum, notes[piece]);
        expect(relativeTimbreDifference(tight, loose, 0) > 0.01
                   && relativeTimbreDifference(normal, tight, 0) > 0.005,
               "Resonant-head tension changes shell resonance and sustain, not just level.");
        setParameter(drum, resonantIds[piece], 0.5f);
        setParameter(drum, dampingIds[piece], 1.0f);
        const auto damped = renderDrumHit(drum, notes[piece]);
        expect(magnitude(damped, 0, 0, 512) > 0.01f
                   && damped.getRMSLevel(0, 8000, 4000) < normal.getRMSLevel(0, 8000, 4000) * 0.1f,
               "Full damping keeps the initial drum hit but removes at least 90 percent of late ring.");
        setParameter(drum, dampingIds[piece], 0.0f);
    }
    const auto normalSnare = renderDrumHit(drum, 38);
    setParameter(drum, "snareWires", 0.0f);
    const auto wiresOff = renderDrumHit(drum, 38);
    expect(magnitude(wiresOff, 0) > 0.01f
               && drumWireEnergy(wiresOff, 0, 4096) < drumWireEnergy(normalSnare, 0, 4096) * 0.02,
           "Disengaging snare wires removes the buzz while preserving the pitched shell.");
    setParameter(drum, "snareWires", 1.0f);
    const auto tightSnare = renderDrumHit(drum, 38);
    expect(drumWireEnergy(tightSnare, 8000, 4000)
               < drumWireEnergy(normalSnare, 8000, 4000) * 0.4,
           "Tightening snare wires gives a shorter, drier buzz than the neutral tension.");
}

void drumKitTuningState()
{
    studio::DrumDeviceProcessor drum;
    drum.prepareToPlay(48000.0, 4096);
    setParameter(drum, "kick", 0.7f);
    for (const auto& control : studio::DrumDeviceProcessor::kitTuningParameters())
        setParameter(drum, control.id, 0.7f);
    drum.setCurrentProgram(5);
    juce::MemoryBlock state;
    expect(drum.saveValidatedState(state).wasOk(), "Custom drum-head tuning can be saved.");
    const auto encoded = juce::JSON::parse(juce::String::fromUTF8(
        static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())));
    expect(encoded.getProperty("schema", {}) == juce::var(3),
           "Custom head and wire controls use versioned drum state.");
    const auto restore = [&drum](const juce::var& value)
    {
        const auto json = juce::JSON::toString(value, false);
        return drum.restoreValidatedState(json.toRawUTF8(), static_cast<int>(json.getNumBytesAsUTF8()));
    };
    auto invalid = encoded.clone();
    invalid.getDynamicObject()->setProperty("snareWires", 1.01);
    invalid.getDynamicObject()->setProperty("kickBatter", 0.0);
    invalid.getDynamicObject()->setProperty("kitPreset", "punk");
    expect(restore(invalid).failed(), "Invalid wire tension rejects the entire saved kit.");
    invalid = encoded.clone();
    invalid.getDynamicObject()->removeProperty("highTomResonant");
    expect(restore(invalid).failed(), "Current-schema kits require every head-tuning control.");
    juce::MemoryBlock unchanged;
    drum.saveValidatedState(unchanged);
    expect(state == unchanged, "Failed kit restores never partially retune a head or change the preset.");

    studio::DrumDeviceProcessor reopened;
    reopened.prepareToPlay(48000.0, 4096);
    expect(reopened.restoreValidatedState(state.getData(), static_cast<int>(state.getSize())).wasOk(),
           "Saved head, damping, and wire settings restore through validated plugin state.");
    for (const auto note : { 35, 36, 37, 38, 39, 40, 41, 43, 45, 47, 48, 50, 42, 49 })
        expect(maximumDifference(renderDrumHit(drum, note, 4096),
                                 renderDrumHit(reopened, note, 4096)) < 0.000001f,
               "Reopened tuning reproduces every shell articulation and leaves cymbals intact.");
    reopened.setCurrentProgram(1);
    for (int index = 9; index < reopened.getParameters().size(); ++index)
        expect(sameFloatBits(reopened.getParameters()[index]->getValue(),
                             drum.getParameters()[index]->getValue()),
               "Changing genre presets preserves the drummer's custom head and wire settings.");

    for (const auto schema : { 1, 2 })
    {
        auto legacy = encoded.clone();
        legacy.getDynamicObject()->setProperty("schema", schema);
        for (const auto& control : studio::DrumDeviceProcessor::kitTuningParameters())
            legacy.getDynamicObject()->removeProperty(control.id);
        if (schema == 1)
            legacy.getDynamicObject()->removeProperty("kitPreset");
        expect(restore(legacy).wasOk() && drum.getCurrentProgram() == (schema == 1 ? 0 : 5),
               "Older projects restore their original genre and mix without needing new head settings.");
        for (int index = 9; index < drum.getParameters().size(); ++index)
            expect(sameFloatBits(drum.getParameters()[index]->getValue(),
                                 drum.getParameters()[index]->getDefaultValue()),
                   "Loading an older kit resets new tuning controls to neutral, not previously edited values.");
        studio::DrumDeviceProcessor original;
        original.prepareToPlay(48000.0, 4096);
        original.setCurrentProgram(schema == 1 ? 0 : 5);
        setParameter(original, "kick", 0.7f);
        for (const auto note : { 36, 38, 41, 45, 48 })
            expect(maximumDifference(renderDrumHit(drum, note, 4096),
                                     renderDrumHit(original, note, 4096)) < 0.000001f,
                   "Neutral legacy migration preserves the kit's audible sound.");
    }
}

void drumSoundPresetTimbres()
{
    studio::DrumDeviceProcessor drum;
    drum.prepareToPlay(48000.0, 4096);
    constexpr std::array notes { 36, 38, 41, 42, 49 };
    constexpr std::array buses {
        studio::DrumDeviceProcessor::kickOutput,
        studio::DrumDeviceProcessor::snareOutput,
        studio::DrumDeviceProcessor::tomsOutput,
        studio::DrumDeviceProcessor::cymbalsOutput,
        studio::DrumDeviceProcessor::cymbalsOutput
    };
    for (std::size_t piece = 0; piece < notes.size(); ++piece)
    {
        std::array<juce::AudioBuffer<float>, 6> rendered;
        for (int preset = 0; preset < static_cast<int>(rendered.size()); ++preset)
        {
            drum.setCurrentProgram(preset);
            drum.reset();
            auto& audio = rendered[static_cast<std::size_t>(preset)];
            audio.setSize(drum.getTotalNumOutputChannels(), 4096);
            juce::MidiBuffer midi;
            midi.addEvent(
                juce::MidiMessage::noteOn(1, notes[piece], juce::uint8(110)), 0);
            drum.processBlock(audio, midi);
            const auto channel = outputChannel(drum, buses[piece], 0);
            auto finite = true;
            for (int output = 0; output < audio.getNumChannels(); ++output)
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                    finite = finite && std::isfinite(audio.getSample(output, sample));
            expect(finite && magnitude(audio, channel) > 0.01f
                       && drum.getCurrentProgram() == preset,
                   "Every preset renders finite, audible kit pieces and survives a transport reset.");
            for (int bus = studio::DrumDeviceProcessor::kickOutput;
                 bus < studio::DrumDeviceProcessor::outputBusCount; ++bus)
                if (bus != buses[piece])
                    expect(magnitude(audio, outputChannel(drum, bus, 0)) < 0.000001f,
                           "Genre presets preserve the existing kick/snare/tom/cymbal stem mapping.");
        }
        const auto channel = outputChannel(drum, buses[piece], 0);
        for (std::size_t first = 0; first < rendered.size(); ++first)
            for (std::size_t second = first + 1; second < rendered.size(); ++second)
            {
                expect(relativeTimbreDifference(rendered[first], rendered[second], channel) > 0.0025,
                       "Every pair of genre presets changes each kit piece's timbre by more than gain alone.");
            }
    }
}

void drumSoundPresetState()
{
    studio::DrumDeviceProcessor drum;
    drum.prepareToPlay(48000.0, 4096);
    expect(setParameter(drum, "kick", 0.65f)
               && setParameter(drum, "room", 0.31f)
               && setParameter(drum, "tuning", 0.625f),
           "The preset state fixture retains user-edited mix, room, and tuning controls.");
    std::array<float, 8> mix;
    for (int index = 0; index < static_cast<int>(mix.size()); ++index)
        mix[static_cast<std::size_t>(index)] = drum.getParameters()[index]->getValue();
    const std::array<juce::String, 6> ids {
        "basic-metal", "punk", "hardcore-punk",
        "death-metal", "modern-metal", "deathcore"
    };
    juce::MidiBuffer sequence;
    for (const auto note : { 36, 38, 41, 42, 49 })
        sequence.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(105)), 0);
    for (int preset = 0; preset < static_cast<int>(ids.size()); ++preset)
    {
        drum.setCurrentProgram(preset);
        juce::MemoryBlock state;
        expect(drum.saveValidatedState(state).wasOk(),
               "Every genre sound preset can be saved as processor state.");
        const auto encoded = juce::JSON::parse(
            juce::String::fromUTF8(static_cast<const char*>(state.getData()),
                                   static_cast<int>(state.getSize())));
        expect(encoded.getProperty("schema", {}) == juce::var(3)
                   && encoded.getProperty("kitPreset", {}).toString()
                       == ids[static_cast<std::size_t>(preset)],
               "Drum preset state uses a stable preset ID rather than a menu index.");
        studio::DrumDeviceProcessor restored;
        restored.prepareToPlay(48000.0, 4096);
        restored.setCurrentProgram((preset + 1) % static_cast<int>(ids.size()));
        expect(restored.restoreValidatedState(
                   state.getData(), static_cast<int>(state.getSize())).wasOk()
                   && restored.getCurrentProgram() == preset,
               "Reopening processor state restores the selected genre sound.");
        for (int index = 0; index < static_cast<int>(mix.size()); ++index)
            expect(sameFloatBits(drum.getParameters()[index]->getValue(),
                                 mix[static_cast<std::size_t>(index)])
                       && sameFloatBits(restored.getParameters()[index]->getValue(),
                                        mix[static_cast<std::size_t>(index)]),
                   "Selecting and restoring presets preserves the user's existing mix controls.");
        drum.reset();
        juce::AudioBuffer<float> original(drum.getTotalNumOutputChannels(), 4096);
        juce::AudioBuffer<float> reopened(restored.getTotalNumOutputChannels(), 4096);
        drum.processBlock(original, sequence);
        restored.processBlock(reopened, sequence);
        expect(maximumDifference(original, reopened) < 0.000001f,
               "Restoring every preset reproduces its complete stereo mix and stems deterministically.");
    }

    juce::MemoryBlock state;
    drum.saveValidatedState(state);
    const auto encoded = juce::JSON::parse(
        juce::String::fromUTF8(static_cast<const char*>(state.getData()),
                               static_cast<int>(state.getSize())));
    const auto restore = [&drum](const juce::var& value)
    {
        const auto json = juce::JSON::toString(value, false);
        const auto size = json.getNumBytesAsUTF8();
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return juce::Result::fail("The synthetic drum state is too large.");
        return drum.restoreValidatedState(json.toRawUTF8(), static_cast<int>(size));
    };
    for (const auto& invalidPreset : {
             juce::var(), juce::var("unknown-kit"), juce::var(4) })
    {
        auto invalid = encoded.clone();
        invalid.getDynamicObject()->setProperty("schema", 2);
        invalid.getDynamicObject()->setProperty("kitPreset", invalidPreset);
        expect(restore(invalid).failed(),
               "Missing, unknown, and non-string drum preset IDs fail validation.");
    }
    for (const auto& invalidSchema : {
             juce::var(4), juce::var(3.5),
             juce::var(static_cast<juce::int64>(0x100000003LL)) })
    {
        auto invalid = encoded.clone();
        invalid.getDynamicObject()->setProperty("schema", invalidSchema);
        expect(restore(invalid).failed(),
               "Unsupported, fractional, and oversized drum state schemas cannot become valid by truncation.");
    }
    auto invalid = encoded.clone();
    invalid.getDynamicObject()->setProperty("schema", 2);
    invalid.getDynamicObject()->setProperty("kitPreset", "punk");
    invalid.getDynamicObject()->setProperty("room", 1.5);
    expect(restore(invalid).failed(),
           "Invalid mix settings reject the entire preset state.");
    juce::MemoryBlock afterInvalid;
    drum.saveValidatedState(afterInvalid);
    expect(state == afterInvalid,
           "Rejected preset states leave both the selected kit and all mix controls untouched.");

    auto legacy = encoded.clone();
    legacy.getDynamicObject()->setProperty("schema", 1);
    legacy.getDynamicObject()->removeProperty("kitPreset");
    expect(restore(legacy).wasOk() && drum.getCurrentProgram() == 0,
           "Legacy drum states reopen with their original Basic Metal Kit sound.");
    for (int index = 0; index < static_cast<int>(mix.size()); ++index)
        expect(sameFloatBits(drum.getParameters()[index]->getValue(),
                             mix[static_cast<std::size_t>(index)]),
               "Legacy drum states preserve every existing mix parameter.");
}

void drumSoundPresetClap()
{
    studio::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, STUDIO_DUO_BUNDLED_CLAP_PATH);
    const auto description = std::find_if(
        descriptions.begin(), descriptions.end(),
        [](const auto* candidate) { return candidate->isInstrument; });
    expect(descriptions.size() == 3 && description != descriptions.end(),
           "The actual bundled CLAP module exports the drum instrument and both amps.");
    if (description == descriptions.end())
        return;
    juce::String error;
    auto instance = format.createInstanceFromDescription(**description, 48000.0, 512, error);
    expect(instance != nullptr, error.toRawUTF8());
    if (instance == nullptr)
        return;
    expect(instance->getParameters().size() == 25,
           "Bundled CLAP drums preserve the eight existing parameter IDs and append kit presets.");
    if (instance->getParameters().size() != 25)
        return;
    auto* preset = instance->getParameters()[8];
    expect(preset->getName(128) == "Kit sound preset" && preset->isDiscrete()
               && preset->getNumSteps() == 6
               && std::abs(preset->getValueForText("Hardcore Punk") - 0.4f) < 0.000001f,
           "CLAP advertises six integer-stepped kit choices with correct normalized text conversion.");

    studio::DrumDeviceProcessor reference;
    reference.prepareToPlay(48000.0, 512);
    juce::MidiBuffer sequence;
    auto offset = 0;
    for (const auto note : { 36, 38, 41, 42, 49 })
    {
        sequence.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(105)), offset);
        offset += 64;
    }
    for (int index = 0; index < reference.getNumPrograms(); ++index)
    {
        instance->prepareToPlay(48000.0, 512);
        reference.reset();
        reference.setCurrentProgram(index);
        preset->setValueNotifyingHost(static_cast<float>(index) / 5.0f);
        juce::AudioBuffer<float> hosted(instance->getTotalNumOutputChannels(), 512);
        juce::AudioBuffer<float> direct(reference.getTotalNumOutputChannels(), 512);
        auto hostedMidi = sequence;
        instance->processBlock(hosted, hostedMidi);
        reference.processBlock(direct, sequence);
        const auto difference = maximumDifference(hosted, direct);
        expect(preset->getText(preset->getValue(), 128) == reference.getProgramName(index)
                   && magnitude(hosted, 0) > 0.01f && difference < 0.000001f,
               ("CLAP must render the in-app " + reference.getProgramName(index)
                + " sound and stems (difference " + juce::String(difference, 8) + ").").toRawUTF8());
    }
    instance->prepareToPlay(48000.0, 512);
    reference.reset();
    reference.setCurrentProgram(3);
    preset->setValueNotifyingHost(0.74f);
    juce::AudioBuffer<float> stepped(instance->getTotalNumOutputChannels(), 512);
    juce::AudioBuffer<float> truncated(reference.getTotalNumOutputChannels(), 512);
    auto steppedMidi = sequence;
    instance->processBlock(stepped, steppedMidi);
    reference.processBlock(truncated, sequence);
    expect(preset->getText(preset->getValue(), 128) == "Death Metal"
               && maximumDifference(stepped, truncated) < 0.000001f,
           "Fractional CLAP enum values truncate to an integer step as required by the CLAP specification.");
    instance->prepareToPlay(48000.0, 512);
    reference.reset();
    for (int index = 9; index < reference.getParameters().size(); ++index)
    {
        const auto normalized = 0.25f + static_cast<float>(index % 4) * 0.2f;
        instance->getParameters()[index]->setValueNotifyingHost(normalized);
        reference.getParameters()[index]->setValueNotifyingHost(normalized);
    }
    juce::AudioBuffer<float> tunedClap(instance->getTotalNumOutputChannels(), 512);
    juce::AudioBuffer<float> tunedDirect(reference.getTotalNumOutputChannels(), 512);
    auto tunedMidi = sequence;
    instance->processBlock(tunedClap, tunedMidi);
    reference.processBlock(tunedDirect, sequence);
    expect(maximumDifference(tunedClap, tunedDirect) < 0.000001f
               && std::abs(instance->getParameters()[9]->getValueForText("6.0") - 0.75f) < 0.000001f,
           "Actual CLAP head, damping, and wire controls match native DSP and preserve physical value conversion.");
    preset->setValueNotifyingHost(0.4f);
    juce::MemoryBlock state;
    auto* validated = dynamic_cast<studio::ValidatedPluginStateTarget*>(instance.get());
    expect(validated != nullptr && validated->saveValidatedState(state).wasOk(),
           "Saving a CLAP kit flushes a pending preset change even without another audio block.");
    if (validated == nullptr || state.isEmpty())
        return;
    auto restored = format.createInstanceFromDescription(**description, 48000.0, 512, error);
    expect(restored != nullptr, error.toRawUTF8());
    if (restored == nullptr)
        return;
    auto* restoredState = dynamic_cast<studio::ValidatedPluginStateTarget*>(restored.get());
    expect(restoredState != nullptr
               && restoredState->restoreValidatedState(
                   state.getData(), static_cast<int>(state.getSize())).wasOk(),
           "The actual bundled CLAP drum preset restores through its state extension.");
    restored->prepareToPlay(48000.0, 512);
    reference.reset();
    reference.setCurrentProgram(2);
    juce::AudioBuffer<float> reopened(restored->getTotalNumOutputChannels(), 512);
    juce::AudioBuffer<float> expected(reference.getTotalNumOutputChannels(), 512);
    auto reopenedMidi = sequence;
    restored->processBlock(reopened, reopenedMidi);
    reference.processBlock(expected, sequence);
    expect(maximumDifference(reopened, expected) < 0.000001f,
           "CLAP state preserves the genre and every custom head, damping, and wire setting.");
}

void drumKitTuningTailAndVoiceSnapshot()
{
    studio::DrumDeviceProcessor drum;
    studio::DrumDeviceProcessor original;
    drum.prepareToPlay(48000.0, 512);
    original.prepareToPlay(48000.0, 512);
    setParameter(drum, "room", 0.0f);
    setParameter(original, "room", 0.0f);
    juce::MidiBuffer hit;
    hit.addEvent(juce::MidiMessage::noteOn(1, 38, juce::uint8(110)), 0);
    juce::AudioBuffer<float> changed(drum.getTotalNumOutputChannels(), 512);
    juce::AudioBuffer<float> reference(original.getTotalNumOutputChannels(), 512);
    drum.processBlock(changed, hit);
    original.processBlock(reference, hit);
    setParameter(drum, "snareBatter", 1.0f);
    setParameter(drum, "snareResonant", 0.0f);
    setParameter(drum, "snareDamping", 1.0f);
    setParameter(drum, "snareWires", 1.0f);
    juce::MidiBuffer empty;
    drum.processBlock(changed, empty);
    original.processBlock(reference, empty);
    expect(maximumDifference(changed, reference) < 0.000001f,
           "Retuning heads or wires leaves an already-ringing drum untouched.");
    drum.processBlock(changed, hit);
    original.processBlock(reference, hit);
    expect(maximumDifference(changed, reference) > 0.01f,
           "The next hit adopts the new head tension, damping, and snare wires.");

    constexpr auto sampleRate = 8000.0;
    constexpr auto blockSamples = 256;
    drum.prepareToPlay(sampleRate, blockSamples);
    setParameter(drum, "room", 1.0f);
    for (const auto& control : studio::DrumDeviceProcessor::kitTuningParameters())
        setParameter(drum, control.id,
                     juce::String(control.id) == "snareWires" ? 0.25f : 0.0f);
    for (int preset = 0; preset < drum.getNumPrograms(); ++preset)
    {
        drum.setCurrentProgram(preset);
        drum.reset();
        const auto tail = static_cast<int>(std::ceil(drum.getTailLengthSeconds() * sampleRate));
        juce::AudioBuffer<float> audio(drum.getTotalNumOutputChannels(), blockSamples);
        auto finite = true;
        for (int cursor = 0; cursor < tail; cursor += blockSamples)
        {
            audio.setSize(drum.getTotalNumOutputChannels(), std::min(blockSamples, tail - cursor));
            juce::MidiBuffer events;
            if (cursor == 0)
                for (const auto note : { 36, 38, 41, 45, 48, 49 })
                    events.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(127)), 0);
            drum.processBlock(audio, events);
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                    finite = finite && std::isfinite(audio.getSample(channel, sample));
        }
        expect(finite, "Extreme head tuning remains finite on every output bus.");
        audio.setSize(drum.getTotalNumOutputChannels(), blockSamples);
        drum.processBlock(audio, empty);
        expect(drum.activeVoiceCountForTesting() == 0 && magnitude(audio, 0) < 0.0001f,
               "The loosest heads and snare wires still expire within every preset's declared export tail.");
    }
}

void drumSoundPresetTails()
{
    studio::DrumDeviceProcessor drum;
    constexpr auto sampleRate = 8000.0;
    constexpr auto blockSamples = 256;
    drum.prepareToPlay(sampleRate, blockSamples);
    setParameter(drum, "room", 1.0f);
    for (int preset = 0; preset < drum.getNumPrograms(); ++preset)
    {
        drum.setCurrentProgram(preset);
        drum.reset();
        const auto tailSamples = static_cast<int>(
            std::ceil(drum.getTailLengthSeconds() * sampleRate));
        juce::AudioBuffer<float> audio(drum.getTotalNumOutputChannels(), blockSamples);
        juce::MidiBuffer empty;
        for (int cursor = 0; cursor < tailSamples; cursor += blockSamples)
        {
            audio.setSize(drum.getTotalNumOutputChannels(),
                          std::min(blockSamples, tailSamples - cursor));
            juce::MidiBuffer midi;
            if (cursor == 0)
            {
                midi.addEvent(juce::MidiMessage::controllerEvent(1, 4, 0), 0);
                for (const auto note : { 46, 49, 51 })
                    midi.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(127)), 0);
            }
            drum.processBlock(audio, midi);
        }
        audio.setSize(drum.getTotalNumOutputChannels(), blockSamples);
        drum.processBlock(audio, empty);
        expect(drum.activeVoiceCountForTesting() == 0
                   && magnitude(audio, 0) < 0.0001f
                   && magnitude(audio, outputChannel(
                       drum, studio::DrumDeviceProcessor::cymbalsOutput, 0)) < 0.0001f,
               "Every preset's longest cymbals and maximum room decay fit the declared export tail.");
    }

    studio::DrumDeviceProcessor original;
    original.prepareToPlay(48000.0, 512);
    drum.prepareToPlay(48000.0, 512);
    drum.setCurrentProgram(0);
    setParameter(drum, "room", 0.0f);
    setParameter(original, "room", 0.0f);
    juce::AudioBuffer<float> changedAudio(drum.getTotalNumOutputChannels(), 512);
    juce::AudioBuffer<float> originalAudio(original.getTotalNumOutputChannels(), 512);
    juce::MidiBuffer kick;
    kick.addEvent(juce::MidiMessage::noteOn(1, 36, juce::uint8(110)), 0);
    drum.processBlock(changedAudio, kick);
    original.processBlock(originalAudio, kick);
    drum.setCurrentProgram(5);
    juce::MidiBuffer empty;
    drum.processBlock(changedAudio, empty);
    original.processBlock(originalAudio, empty);
    expect(drum.activeVoiceCountForTesting() == 1
               && maximumDifference(changedAudio, originalAudio) < 0.000001f,
           "Changing kit presets leaves already-sounding drum voices intact.");
    drum.processBlock(changedAudio, kick);
    original.processBlock(originalAudio, kick);
    expect(maximumDifference(changedAudio, originalAudio) > 0.01f,
           "New hits adopt the new preset while earlier hits finish naturally.");
}

void drumSoundPresetEngine()
{
    auto project = studio::ProjectTemplates::createBlankSong();
    project.metronomeEnabled = false;
    auto track = studio::ProjectTemplates::createDrumPerformanceTrack(project, 0.0);
    track.midiClips.front().notes.push_back(studio::createMidiNote(
        track.midiClips.front(), 0.0, 38, 0.25, 110,
        project.findDrumMap(track.midiClips.front().drumMapId)));
    studio::AddTrackCommand add(track);
    juce::String error;
    expect(add.perform(project, error), error.toRawUTF8());
    const auto& insert = track.inserts.front();
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project, { requestFor(track, insert) }).wasOk()
               && waitForRuntime(engine),
           "The drum preset integration fixture creates an ordinary instrument runtime.");
    auto statuses = engine.pluginRuntimeStatuses();
    const auto status = std::find_if(statuses.cbegin(), statuses.cend(),
        [&insert](const auto& candidate) { return candidate.insertId == insert.id; });
    expect(status != statuses.cend(), "The bundled drum runtime exposes its parameter metadata.");
    if (status == statuses.cend())
        return;
    const auto parameter = std::find_if(
        status->parameters.cbegin(), status->parameters.cend(),
        [](const auto& candidate) { return candidate.id == "kitPreset"; });
    expect(parameter != status->parameters.cend() && parameter->index == 8
               && parameter->automatable,
           "The kit preset uses a stable, appended, automatable engine parameter.");
    if (parameter == status->parameters.cend())
        return;
    expect(engine.setPluginParameter(insert.id, parameter->index, 1.0f, error),
           error.toRawUTF8());
    const auto head = std::find_if(
        status->parameters.cbegin(), status->parameters.cend(),
        [](const auto& candidate) { return candidate.id == "snareBatter"; });
    expect(head != status->parameters.cend()
               && engine.setPluginParameter(insert.id, head->index, 0.75f, error),
           "Per-drum head tuning uses the same live engine parameter path as the preset.");
    expect(engine.updateProject(project, { requestFor(track, insert) }).wasOk()
               && waitForRuntime(engine),
           "Refreshing the project preserves the live drum processor.");
    const auto captures = engine.capturePluginStates(2000);
    const auto capture = std::find_if(captures.cbegin(), captures.cend(),
        [&insert](const auto& candidate) { return candidate.insertId == insert.id; });
    expect(capture != captures.cend() && capture->result.wasOk(),
           "The live genre preset participates in the existing project-save and export state capture.");
    if (capture == captures.cend() || capture->result.failed())
        return;
    studio::DrumDeviceProcessor restored;
    expect(restored.restoreValidatedState(
               capture->state.getData(), static_cast<int>(capture->state.getSize())).wasOk()
               && restored.getCurrentProgram() == 5
               && std::abs(restored.getParameters()[12]->getValue() - 0.75f) < 0.000001f,
           "Project refresh and live state capture retain the Deathcore sound and custom snare head.");
    studio::StudioAudioEngine renderer;
    juce::AudioBuffer<float> deathcore;
    juce::AudioBuffer<float> basic;
    expect(renderer.renderToBuffer(
               project, deathcore, 8000.0, { requestFor(track, insert, capture->state) }).wasOk()
               && renderer.renderToBuffer(
                   project, basic, 8000.0, { requestFor(track, insert) }).wasOk(),
           "Offline MIDI exports accept captured genre state and the legacy default kit.");
    expect(deathcore.getNumSamples() == basic.getNumSamples()
               && deathcore.getNumChannels() == basic.getNumChannels()
               && magnitude(deathcore, 0) > 0.001f && magnitude(basic, 0) > 0.001f
               && maximumDifference(deathcore, basic) > 0.01f,
           "Mix export renders the captured live preset rather than silently falling back to Basic Metal Kit.");
}

void drumProcessor()
{
    auto drum = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    expect(drum != nullptr
               && drum->acceptsMidi()
               && drum->getBusCount(false)
                      == studio::DrumDeviceProcessor::outputBusCount,
           "The bundled drum device is a MIDI instrument with multi-output buses.");
    drum->prepareToPlay(48000.0, 1024);

    juce::AudioBuffer<float> audio(
        drum->getTotalNumOutputChannels(),
        1024);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120), 0);
    drum->processBlock(audio, midi);
    expect(magnitude(
               audio,
               outputChannel(
                   *drum,
                   studio::DrumDeviceProcessor::mainOutput,
                   0))
               > 0.01f,
           "A normal kick MIDI note renders drum audio.");
    expect(magnitude(
               audio,
               outputChannel(
                   *drum,
                   studio::DrumDeviceProcessor::kickOutput,
                   0))
               > 0.01f
               && magnitude(
                      audio,
                      outputChannel(
                          *drum,
                          studio::DrumDeviceProcessor::snareOutput,
                          0))
                      < 0.00001f,
           "Kick audio appears on the deterministic kick stem only.");

    const auto renderVelocity = [&drum](int velocity)
    {
        drum->reset();
        juce::AudioBuffer<float> rendered(
            drum->getTotalNumOutputChannels(),
            512);
        rendered.clear();
        juce::MidiBuffer events;
        events.addEvent(
            juce::MidiMessage::noteOn(
                1,
                38,
                static_cast<juce::uint8>(velocity)),
            0);
        drum->processBlock(rendered, events);
        return magnitude(
            rendered,
            outputChannel(
                *drum,
                studio::DrumDeviceProcessor::snareOutput,
                0));
    };
    expect(renderVelocity(120) > renderVelocity(36) * 2.0f,
           "Drum synthesis honors MIDI velocity.");

    drum->reset();
    juce::AudioBuffer<float> deterministicA(
        drum->getTotalNumOutputChannels(),
        1024);
    deterministicA.clear();
    juce::MidiBuffer sequence;
    sequence.addEvent(
        juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110),
        0);
    sequence.addEvent(
        juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110),
        512);
    drum->processBlock(deterministicA, sequence);
    drum->reset();
    juce::AudioBuffer<float> deterministicB(
        drum->getTotalNumOutputChannels(),
        1024);
    deterministicB.clear();
    drum->processBlock(deterministicB, sequence);
    expect(maximumDifference(deterministicA, deterministicB) < 0.000001f,
           "Drum round robin and noise reset to a deterministic sequence.");
    const auto kickChannel = outputChannel(
        *drum,
        studio::DrumDeviceProcessor::kickOutput,
        0);
    auto roundRobinDifference = 0.0f;
    for (int sample = 4; sample < 120; ++sample)
    {
        roundRobinDifference += std::abs(
            deterministicA.getSample(kickChannel, sample)
            - deterministicA.getSample(
                kickChannel,
                512 + sample));
    }
    expect(roundRobinDifference > 0.01f,
           "Repeated base notes use deterministic round-robin variations.");

    auto crash = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    auto chokedCrash = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    crash->prepareToPlay(48000.0, 512);
    chokedCrash->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> crashBlock(
        crash->getTotalNumOutputChannels(),
        512);
    juce::AudioBuffer<float> chokeBlock(
        chokedCrash->getTotalNumOutputChannels(),
        512);
    crashBlock.clear();
    chokeBlock.clear();
    juce::MidiBuffer crashHit;
    crashHit.addEvent(
        juce::MidiMessage::noteOn(1, 49, (juce::uint8) 118),
        0);
    crash->processBlock(crashBlock, crashHit);
    chokedCrash->processBlock(chokeBlock, crashHit);
    crashBlock.clear();
    chokeBlock.clear();
    juce::MidiBuffer noEvents;
    juce::MidiBuffer choke;
    choke.addEvent(
        juce::MidiMessage::noteOn(1, 57, (juce::uint8) 100),
        0);
    crash->processBlock(crashBlock, noEvents);
    chokedCrash->processBlock(chokeBlock, choke);
    const auto cymbalChannel = outputChannel(
        *crash,
        studio::DrumDeviceProcessor::cymbalsOutput,
        0);
    expect(magnitude(chokeBlock, cymbalChannel, 128, 384)
               < magnitude(crashBlock, cymbalChannel, 128, 384) * 0.2f,
           "Mapped cymbal choke notes terminate the matching cymbal state.");

    const auto map = studio::createDefaultMetalDrumMap();
    studio::MidiClip mappedClip;
    mappedClip.editorMode = studio::MidiEditorMode::drums;
    mappedClip.drumMapId = map.id;
    const auto openHat = studio::createMidiNote(
        mappedClip,
        0.0,
        46,
        0.25,
        110,
        &map);
    const auto closedHat = studio::createMidiNote(
        mappedClip,
        0.0,
        42,
        0.25,
        110,
        &map);
    const auto pedalHat = studio::createMidiNote(
        mappedClip,
        0.0,
        44,
        0.25,
        110,
        &map);
    expect(openHat.footControlValue == 0
               && closedHat.footControlValue == 127
               && pedalHat.footControlValue > 0
               && pedalHat.footControlValue < 127,
           "Hi-hat metadata follows the MIDI CC4 convention: 0 is open and 127 is closed.");

    const auto renderHatTail = [](int footValue)
    {
        auto instrument = studio::DeviceRegistry::create(
            "studio.device.drum-composer");
        instrument->prepareToPlay(48000.0, 1024);
        juce::AudioBuffer<float> first(
            instrument->getTotalNumOutputChannels(),
            1024);
        first.clear();
        juce::MidiBuffer hit;
        hit.addEvent(
            juce::MidiMessage::controllerEvent(1, 4, footValue),
            0);
        hit.addEvent(
            juce::MidiMessage::noteOn(1, 46, (juce::uint8) 110),
            0);
        instrument->processBlock(first, hit);
        juce::AudioBuffer<float> tail(
            instrument->getTotalNumOutputChannels(),
            1024);
        juce::MidiBuffer empty;
        for (int block = 0; block < 32; ++block)
        {
            tail.clear();
            instrument->processBlock(tail, empty);
        }
        return magnitude(
            tail,
            outputChannel(
                *instrument,
                studio::DrumDeviceProcessor::cymbalsOutput,
                0));
    };
    expect(renderHatTail(0) > renderHatTail(127) * 2.0f,
           "CC4 value 0 produces a longer open hi-hat decay than closed value 127.");

    const auto renderHatAfterControl = [](int footValue)
    {
        auto instrument = studio::DeviceRegistry::create(
            "studio.device.drum-composer");
        instrument->prepareToPlay(48000.0, 1024);
        juce::AudioBuffer<float> renderedHat(
            instrument->getTotalNumOutputChannels(),
            1024);
        renderedHat.clear();
        juce::MidiBuffer hit;
        hit.addEvent(
            juce::MidiMessage::controllerEvent(1, 4, 0),
            0);
        hit.addEvent(
            juce::MidiMessage::noteOn(1, 46, (juce::uint8) 110),
            0);
        instrument->processBlock(renderedHat, hit);
        renderedHat.clear();
        juce::MidiBuffer control;
        control.addEvent(
            juce::MidiMessage::controllerEvent(1, 4, footValue),
            0);
        instrument->processBlock(renderedHat, control);
        return magnitude(
            renderedHat,
            outputChannel(
                *instrument,
                studio::DrumDeviceProcessor::cymbalsOutput,
                0),
            512,
            512);
    };
    expect(renderHatAfterControl(127)
               < renderHatAfterControl(0) * 0.1f,
           "Closing CC4 to 127 rapidly chokes an already-open hi-hat.");

    juce::MemoryBlock state;
    drum->getStateInformation(state);
    auto restored = studio::DeviceRegistry::create(
        "studio.device.drum-composer");
    auto* validated =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            restored.get());
    expect(validated != nullptr
               && validated->restoreValidatedState(
                      state.getData(),
                      static_cast<int>(state.getSize()))
                      .wasOk(),
           "Drum device parameters restore through validated state.");
}

void drumTailAndVoiceReuse()
{
    constexpr auto sampleRate = 8000.0;
    constexpr auto blockSamples = 256;
    studio::DrumDeviceProcessor drum;
    drum.prepareToPlay(sampleRate, blockSamples);
    expect(setParameter(drum, "room", 1.0f),
           "The drum tail fixture enables the maximum room amount.");

    const auto declaredTailSamples = static_cast<int>(
        std::ceil(drum.getTailLengthSeconds() * sampleRate));
    auto renderedSamples = 0;
    while (renderedSamples < declaredTailSamples)
    {
        const auto samples = std::min(
            blockSamples,
            declaredTailSamples - renderedSamples);
        juce::AudioBuffer<float> audio(
            drum.getTotalNumOutputChannels(),
            samples);
        audio.clear();
        juce::MidiBuffer midi;
        if (renderedSamples == 0)
        {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1,
                    51,
                    static_cast<juce::uint8>(127)),
                0);
        }
        drum.processBlock(audio, midi);
        renderedSamples += samples;
    }

    juce::AudioBuffer<float> afterTail(
        drum.getTotalNumOutputChannels(),
        blockSamples);
    afterTail.clear();
    juce::MidiBuffer empty;
    drum.processBlock(afterTail, empty);
    const auto mainChannel = outputChannel(
        drum,
        studio::DrumDeviceProcessor::mainOutput,
        0);
    const auto cymbalChannel = outputChannel(
        drum,
        studio::DrumDeviceProcessor::cymbalsOutput,
        0);
    expect(std::max(
               magnitude(afterTail, mainChannel),
               magnitude(afterTail, cymbalChannel))
               < 0.0001f,
           "The longest cymbal and room output are near silence at the declared drum tail.");

    drum.reset();
    constexpr auto repeatedHitSeconds = 10;
    const auto totalSamples =
        static_cast<int>(sampleRate) * repeatedHitSeconds;
    const auto hitIntervalSamples =
        static_cast<int>(sampleRate) / 32;
    constexpr std::array repeatedPattern { 36, 38, 41, 49 };
    auto nextHitSample = 0;
    auto nextPatternNote = std::size_t { 0 };
    for (int blockStart = 0;
         blockStart < totalSamples;
         blockStart += blockSamples)
    {
        const auto samples = std::min(
            blockSamples,
            totalSamples - blockStart);
        juce::AudioBuffer<float> audio(
            drum.getTotalNumOutputChannels(),
            samples);
        audio.clear();
        juce::MidiBuffer midi;
        while (nextHitSample < blockStart + samples)
        {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1,
                    repeatedPattern[nextPatternNote],
                    static_cast<juce::uint8>(110)),
                nextHitSample - blockStart);
            nextPatternNote =
                (nextPatternNote + 1) % repeatedPattern.size();
            nextHitSample += hitIntervalSamples;
        }
        drum.processBlock(audio, midi);
    }
    expect(drum.activeVoiceCountForTesting() < 40,
           "A dense repeated kit pattern with eight cymbal hits per second reuses expired voices without exhausting the fixed pool.");
}

void liveDrumPadAudio()
{
    auto project = studio::ProjectTemplates::createBlankSong();
    project.metronomeEnabled = false;
    auto track = studio::ProjectTemplates::createDrumPerformanceTrack(project, 0.0);
    track.armed = false;
    studio::AddTrackCommand addTrack(track);
    juce::String error;
    expect(addTrack.perform(project, error), error.toRawUTF8());
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project, { requestFor(track, track.inserts.front()) }).wasOk()
               && waitForRuntime(engine),
           "The ready-to-play drum preset creates an actual audio runtime.");
    engine.setMidiAuditionEnabled(true);
    const auto silence = engine.renderActiveBlockForTesting(512);
    expect(magnitude(silence, 0) < 0.000001f,
           "The drum preset is silent until a pad is played.");
    expect(engine.enqueueMidiInput(
               track.id, juce::MidiMessage::noteOn(1, 36, juce::uint8(112))).wasOk(),
           "A visual drum-pad hit reaches the bundled instrument.");
    expect(engine.enqueueMidiInput(track.id, juce::MidiMessage::noteOff(1, 36)).wasOk(),
           "A tuning tap can enqueue its note-on and release together without swallowing the one-shot.");
    juce::ignoreUnused(engine.renderActiveBlockForTesting(512));
    const auto hit = engine.renderActiveBlockForTesting(2048);
    expect(magnitude(hit, 0) > 0.001f && magnitude(hit, 1) > 0.001f,
           "Drum pads produce stereo audio through the ordinary mixer while stopped and unarmed.");
    expect(engine.enqueueMidiInput(track.id, juce::MidiMessage::noteOff(1, 36)).wasOk(),
           "The live drum-pad note can be released.");
    juce::ignoreUnused(engine.renderActiveBlockForTesting(512));
    const auto tail = engine.renderActiveBlockForTesting(512);
    expect(magnitude(tail, 0) > 0.0001f,
           "One-shot drum tails continue between input events instead of being truncated.");
}

void drumEngineRoutingAndRender()
{
    auto project = studio::Project::createDefault();
    auto& instrument = addInstrumentTrack(project, "Bundled drums");
    instrument.armed = true;
    const auto instrumentId = instrument.id;
    const auto insert = addBundledInsert(
        instrument,
        "studio.device.drum-composer",
        "Metal Drum Composer");

    auto stateProcessor = studio::DeviceRegistry::create(
        insert.pluginIdentifier);
    setParameter(*stateProcessor, "main", 0.0f);
    setParameter(*stateProcessor, "room", 0.0f);
    juce::MemoryBlock quietMainState;
    stateProcessor->getStateInformation(quietMainState);

    studio::Track kickAux;
    kickAux.name = "Kick stem";
    kickAux.type = studio::TrackType::aux;
    project.tracks.insert(project.tracks.end() - 1, kickAux);
    studio::RoutingConnection kickRoute;
    kickRoute.name = "Kick";
    kickRoute.kind = studio::RouteKind::send;
    kickRoute.tap = studio::RouteTap::preFader;
    kickRoute.sourceTrackId = instrumentId;
    kickRoute.sourceInsertId = insert.id;
    kickRoute.sourceBusIndex =
        studio::DrumDeviceProcessor::kickOutput;
    kickRoute.destination.type =
        studio::RouteEndpointType::track;
    kickRoute.destination.trackId = kickAux.id;
    project.routingConnections.push_back(kickRoute);
    juce::String validationError;
    expect(project.validateRoutingGraph(validationError),
           validationError.toRawUTF8());
    const auto restoredProject = studio::Project::fromVar(
        project.toVar(),
        validationError);
    expect(restoredProject.has_value()
               && std::any_of(
                   restoredProject->routingConnections.cbegin(),
                   restoredProject->routingConnections.cend(),
                   [&kickRoute](const auto& route)
                   {
                       return route.id == kickRoute.id
                           && route.sourceInsertId
                                  == kickRoute.sourceInsertId
                           && route.sourceBusIndex
                                  == kickRoute.sourceBusIndex;
                   }),
           "Bundled processor-output routing survives project serialization.");
    auto removalProject = project;
    studio::CommandStack commands;
    juce::String commandError;
    expect(commands.perform(
               std::make_unique<studio::RemovePluginInsertCommand>(
                   instrumentId,
                   insert.id),
               removalProject,
               commandError)
               && std::none_of(
                   removalProject.routingConnections.cbegin(),
                   removalProject.routingConnections.cend(),
                   [&kickRoute](const auto& route)
                   {
                       return route.id == kickRoute.id;
                   }),
           "Removing a multi-output insert removes its dependent output routes.");
    expect(commands.undo(removalProject)
               && std::any_of(
                   removalProject.routingConnections.cbegin(),
                   removalProject.routingConnections.cend(),
                   [&kickRoute](const auto& route)
                   {
                       return route.id == kickRoute.id;
                   }),
           "Undo restores a removed multi-output insert and its routes.");

    studio::StudioAudioEngine routedEngine;
    const auto* routedTrack = project.findTrack(instrumentId);
    auto routedRequest = requestFor(
        *routedTrack,
        insert,
        quietMainState);
    expect(routedEngine.updateProject(
               project,
               { routedRequest })
               .wasOk()
               && waitForRuntime(routedEngine),
           "A routed bundled drum runtime becomes ready.");
    juce::MidiBuffer kick;
    kick.addEvent(
        juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120),
        0);
    const auto routed = routedEngine.renderActiveBlockWithMidiForTesting(
        kick,
        1024);

    auto dryProject = project;
    dryProject.routingConnections.erase(
        std::remove_if(
            dryProject.routingConnections.begin(),
            dryProject.routingConnections.end(),
            [&kickRoute](const auto& route)
            {
                return route.id == kickRoute.id;
            }),
        dryProject.routingConnections.end());
    studio::StudioAudioEngine mainOnlyEngine;
    expect(mainOnlyEngine.updateProject(
               dryProject,
               { routedRequest })
               .wasOk()
               && waitForRuntime(mainOnlyEngine),
           "A main-only bundled drum runtime becomes ready.");
    const auto mainOnly =
        mainOnlyEngine.renderActiveBlockWithMidiForTesting(
            kick,
            1024);
    expect(magnitude(routed, 0)
               > magnitude(mainOnly, 0) * 20.0f,
           "A processor-output route carries the kick stem through the normal audio graph.");

    auto renderProject = studio::Project::createDefault();
    auto& renderTrack = addInstrumentTrack(
        renderProject,
        "Rendered drums");
    const auto renderInsert = addBundledInsert(
        renderTrack,
        "studio.device.drum-composer",
        "Metal Drum Composer");
    studio::MidiClip clip;
    clip.name = "Drum render";
    clip.editorMode = studio::MidiEditorMode::drums;
    clip.drumMapId = renderProject.drumMaps.front().id;
    clip.durationBeats = 1.0;
    studio::MidiNote note;
    note.pitch = 38;
    note.velocity = 116;
    note.durationBeats = 0.25;
    note.roundRobinHint = 1;
    const auto* entry =
        renderProject.drumMaps.front().entryForPitch(note.pitch);
    note.drumMapEntryId = entry->id;
    note.articulation = entry->articulation;
    renderTrack.midiClips.push_back(clip);
    renderTrack.midiClips.back().notes.push_back(note);
    studio::AutomationLane drumAutomation;
    drumAutomation.name = "Drum main";
    drumAutomation.target.type =
        studio::AutomationTargetType::deviceParameter;
    drumAutomation.target.trackId = renderTrack.id;
    drumAutomation.target.insertId = renderInsert.id;
    drumAutomation.target.parameterId = "main";
    drumAutomation.target.parameterIndex = 0;
    drumAutomation.interpolation =
        studio::AutomationInterpolation::step;
    drumAutomation.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.82
    });
    renderProject.automationLanes.push_back(drumAutomation);

    studio::StudioAudioEngine renderEngine;
    juce::AudioBuffer<float> rendered;
    expect(renderEngine.renderToBuffer(
               renderProject,
               rendered,
               48000.0,
               { requestFor(renderTrack, renderInsert) })
               .wasOk(),
           "Offline rendering instantiates the bundled MIDI instrument.");
    expect(magnitude(rendered, 0, 0, 4096) > 0.01f,
           "Offline rendering schedules persisted drum notes into audio.");
}

void ampCabinetPublicationRace()
{
    const auto cabinetIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoPublicationRaceCabinet",
                ".wav",
                false);
    expect(writeCabinetIr(cabinetIr, 512),
           "The cabinet publication race fixture writes.");

    studio::AmpDeviceProcessor processing(
        studio::AmpDeviceType::guitar);
    studio::AmpDeviceProcessor reference(
        studio::AmpDeviceType::guitar);
    processing.prepareToPlay(48000.0, 2048);
    reference.prepareToPlay(48000.0, 2048);

    auto publicationsSucceeded = true;
    auto readersUsedPublishedKernel = true;
    auto currentIsCustom = false;
    for (int iteration = 0; iteration < 12; ++iteration)
    {
        const auto publishCustom = !currentIsCustom;
        currentIsCustom = publishCustom;
        const auto referenceResult = publishCustom
            ? reference.loadCabinetFile(cabinetIr)
            : reference.useEmbeddedDefaultCabinet();
        if (referenceResult.failed())
        {
            publicationsSucceeded = false;
            break;
        }

        processing.reset();
        reference.reset();
        auto actual = sineInput(
            2048,
            90.0 + static_cast<double>(iteration));
        auto expected = actual;
        studio::AmpDeviceProcessor::CabinetReaderBarrierForTesting
            barrier;
        processing.setCabinetReaderBarrierForTesting(&barrier);
        juce::MidiBuffer actualMidi;
        std::thread audioThread(
            [&processing, &actual, &actualMidi]
            {
                processing.processBlock(actual, actualMidi);
            });

        const auto readerPaused = waitForFlag(barrier.slotLoaded);
        const auto publicationResult = publishCustom
            ? processing.loadCabinetFile(cabinetIr)
            : processing.useEmbeddedDefaultCabinet();
        barrier.resume.store(true, std::memory_order_release);
        audioThread.join();
        processing.setCabinetReaderBarrierForTesting(nullptr);
        if (!readerPaused || publicationResult.failed())
        {
            publicationsSucceeded = false;
            break;
        }

        juce::MidiBuffer expectedMidi;
        reference.processBlock(expected, expectedMidi);
        auto difference = 0.0f;
        for (int channel = 0; channel < actual.getNumChannels(); ++channel)
        {
            for (int sample = 0; sample < actual.getNumSamples(); ++sample)
            {
                difference = std::max(
                    difference,
                    std::abs(
                        actual.getSample(channel, sample)
                        - expected.getSample(channel, sample)));
            }
        }
        readersUsedPublishedKernel =
            readersUsedPublishedKernel
            && difference < 0.00001f;
    }

    expect(publicationsSucceeded,
           "Repeated cabinet publication succeeds while processing is paused in reader acquisition.");
    expect(readersUsedPublishedKernel,
           "Cabinet readers retry acquisition when publication changes the active slot.");
    cabinetIr.deleteFile();
}

void ampProcessorAndCabinetState()
{
    auto guitar = studio::DeviceRegistry::create(
        "studio.device.guitar-amp");
    auto bass = studio::DeviceRegistry::create(
        "studio.device.bass-amp");
    guitar->prepareToPlay(48000.0, 4096);
    bass->prepareToPlay(48000.0, 4096);
    auto guitarAudio = sineInput(4096, 110.0);
    auto bassAudio = guitarAudio;
    juce::MidiBuffer midi;
    guitar->processBlock(guitarAudio, midi);
    bass->processBlock(bassAudio, midi);
    expect(guitar->getLatencySamples() == 128
               && bass->getLatencySamples() == 128
               && guitar->getTailLengthSeconds() > 0.0
               && bass->getTailLengthSeconds() > 0.0,
           "Cabinet convolution reports fixed latency and a finite tail.");
    expect(magnitude(guitarAudio, 0, 128, 3968) > 0.001f
               && magnitude(guitarAudio, 0, 128, 3968) < 2.0f
               && magnitude(bassAudio, 0, 128, 3968) > 0.001f
               && magnitude(bassAudio, 0, 128, 3968) < 2.0f,
           "Guitar and bass amps perform real nonlinear cabinet processing.");
    auto ampDifference = 0.0f;
    for (int sample = 128; sample < 4096; ++sample)
        ampDifference += std::abs(
            guitarAudio.getSample(0, sample)
            - bassAudio.getSample(0, sample));
    expect(ampDifference > 0.1f,
           "The guitar and bass amp signal paths have distinct voicing.");
    expect(std::all_of(
               guitar->getParameters().begin(),
               guitar->getParameters().end(),
               [](const auto* parameter)
               {
                   return parameter->isAutomatable();
               }),
           "Amp gain, tone, cabinet mix, and output controls are automatable.");

    const auto validIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoCabinet",
                ".wav",
                false);
    const auto longIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoLongCabinet",
                ".wav",
                false);
    const auto invalidIr =
        juce::File::getCurrentWorkingDirectory()
            .getNonexistentChildFile(
                "StudioDuoInvalidCabinet",
                ".txt",
                false);
    expect(writeCabinetIr(validIr, 512),
           "The synthetic cabinet fixture writes.");
    expect(writeCabinetIr(longIr, 1200),
           "The oversized synthetic cabinet fixture writes.");
    invalidIr.replaceWithText("not audio");

    auto* guitarAmp =
        dynamic_cast<studio::AmpDeviceProcessor*>(guitar.get());
    expect(guitarAmp != nullptr,
           "The registry creates the reusable amp processor.");
    const auto invalidResult =
        guitarAmp->loadCabinetFile(invalidIr);
    expect(invalidResult.failed()
               && invalidResult.getErrorMessage()
                      .containsIgnoreCase("valid audio"),
           "Invalid cabinet files return an explicit error.");
    const auto longResult =
        guitarAmp->loadCabinetFile(longIr);
    expect(longResult.failed()
               && longResult.getErrorMessage()
                      .containsIgnoreCase("too long"),
           "Cabinet IRs outside the bounded real-time configuration are rejected.");
    expect(guitarAmp->loadCabinetFile(validIr).wasOk()
               && guitarAmp->cabinetDescription().contains(
                   validIr.getFileName()),
           "A valid synthetic cabinet IR loads outside the audio callback.");
    setParameter(*guitar, "gain", 0.31f);
    setParameter(*guitar, "mid", 0.72f);
    juce::MemoryBlock state;
    auto* validated =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            guitar.get());
    expect(validated != nullptr
               && validated->saveValidatedState(state).wasOk(),
           "Amp parameters and cabinet data serialize together.");
    validIr.deleteFile();

    auto restored = studio::DeviceRegistry::create(
        "studio.device.guitar-amp");
    restored->prepareToPlay(48000.0, 4096);
    auto* restoredAmp =
        dynamic_cast<studio::AmpDeviceProcessor*>(
            restored.get());
    auto* restoredValidated =
        dynamic_cast<studio::ValidatedPluginStateTarget*>(
            restored.get());
    expect(restoredValidated != nullptr
               && restoredValidated->restoreValidatedState(
                      state.getData(),
                      static_cast<int>(state.getSize()))
                      .wasOk()
               && restoredAmp->cabinetDescription().contains(
                   validIr.getFileName()),
           "Cabinet audio restores from opaque state after the source file is removed.");

    guitar->reset();
    restored->reset();
    auto originalOutput = sineInput(4096, 146.83);
    auto restoredOutput = originalOutput;
    guitar->processBlock(originalOutput, midi);
    restored->processBlock(restoredOutput, midi);
    auto stateDifference = 0.0f;
    for (int sample = 0; sample < 4096; ++sample)
        stateDifference = std::max(
            stateDifference,
            std::abs(
                originalOutput.getSample(0, sample)
                - restoredOutput.getSample(0, sample)));
    expect(stateDifference < 0.00001f,
           "Restored cabinet and amp state reproduce deterministic audio.");

    expect(restoredValidated->restoreValidatedState(
               state.getData(),
               static_cast<int>(state.getSize() / 2))
               .failed(),
           "Truncated cabinet state is rejected instead of silently using a fallback.");
    restored->setStateInformation(
        state.getData(),
        static_cast<int>(state.getSize() / 2));
    expect(restoredAmp->cabinetDescription()
               .containsIgnoreCase("error"),
           "Standalone state restoration exposes invalid cabinet state in the editor.");
    expect(restoredAmp->useEmbeddedDefaultCabinet().wasOk()
               && restoredAmp->cabinetDescription()
                      .containsIgnoreCase("embedded"),
           "A documented embedded cabinet is always available.");

    longIr.deleteFile();
    invalidIr.deleteFile();
}

void ampRuntimeStateAndAutomation()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.inputMonitoring = true;
    const auto insert = addBundledInsert(
        track,
        "studio.device.guitar-amp",
        "Guitar Amp");
    auto request = requestFor(track, insert);
    studio::StudioAudioEngine engine;
    expect(engine.updateProject(project, { request }).wasOk()
               && waitForRuntime(engine),
           "The guitar amp activates in the live insert graph.");
    const auto statuses = engine.pluginRuntimeStatuses();
    expect(statuses.size() == 1
               && statuses.front().state
                      == studio::StudioAudioEngine::
                          PluginRuntimeStatus::State::ready
               && statuses.front().latencySamples == 128
               && statuses.front().tailSeconds > 0.0
               && statuses.front().parameters.size() == 8,
           "Amp runtime metadata exposes latency, tail, and parameters.");
    juce::String error;
    expect(engine.setPluginParameter(
               insert.id,
               0,
               0.82f,
               error),
           error.toRawUTF8());
    const auto captures = engine.capturePluginStates(1000);
    expect(captures.size() == 1
               && captures.front().result.wasOk()
               && !captures.front().state.isEmpty(),
           "Live amp automation changes are captured in persistent state.");

    auto invalidRequest = request;
    const char invalidState[] { 'b', 'a', 'd' };
    invalidRequest.state.append(invalidState, sizeof(invalidState));
    studio::StudioAudioEngine invalidEngine;
    expect(invalidEngine.updateProject(
               project,
               { invalidRequest })
               .wasOk()
               && waitForRuntime(invalidEngine),
           "Invalid bundled state remains a recoverable runtime transition.");
    const auto invalidStatuses =
        invalidEngine.pluginRuntimeStatuses();
    expect(invalidStatuses.size() == 1
               && invalidStatuses.front().state
                      == studio::StudioAudioEngine::
                          PluginRuntimeStatus::State::failed
               && invalidStatuses.front().message
                      .containsIgnoreCase("state"),
           "Invalid cabinet state is reported through the existing missing/failed processor UI.");

    auto renderProject = studio::Project::createDefault();
    auto& renderTrack = renderProject.tracks.front();
    const auto generator = addBundledInsert(
        renderTrack,
        "studio.device.generator",
        "Signal Generator");
    const auto amp = addBundledInsert(
        renderTrack,
        "studio.device.bass-amp",
        "Bass Amp");
    studio::StudioAudioEngine renderEngine;
    juce::AudioBuffer<float> rendered;
    expect(renderEngine.renderToBuffer(
               renderProject,
               rendered,
               48000.0,
               {
                   requestFor(renderTrack, generator),
                   requestFor(renderTrack, amp)
               })
               .wasOk(),
           "The bundled amp renders through the processor-inclusive offline path.");
    expect(rendered.getNumSamples() > 8 * 48000
               && magnitude(rendered, 0, 256, 4096) > 0.001f,
           "Offline amp rendering includes reported latency, tail, and processed audio.");
}
}

void bundledDeviceTests()
{
    registryMetadata();
    drumSoundPresetPrograms();
    drumKitTuningParameters();
    drumKitHeadPitchAndIsolation();
    drumKitResonanceDampingAndWires();
    drumKitTuningState();
    drumKitTuningTailAndVoiceSnapshot();
    drumSoundPresetTimbres();
    drumSoundPresetState();
    drumSoundPresetClap();
    drumSoundPresetTails();
    drumSoundPresetEngine();
    drumProcessor();
    drumTailAndVoiceReuse();
    liveDrumPadAudio();
    drumEngineRoutingAndRender();
    ampCabinetPublicationRace();
    ampProcessorAndCabinetState();
    ampRuntimeStateAndAutomation();
}

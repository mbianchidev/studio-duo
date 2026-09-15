#pragma once

#include "plugin_host/ValidatedPluginStateTarget.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstdint>
#include <vector>

namespace studio
{
class DrumDeviceProcessor final : public juce::AudioProcessor,
                                  public ValidatedPluginStateTarget
{
public:
    enum OutputBus
    {
        mainOutput = 0,
        kickOutput,
        snareOutput,
        tomsOutput,
        cymbalsOutput,
        outputBusCount
    };

    DrumDeviceProcessor();

    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>& audio,
                      juce::MidiBuffer& midi) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& name) override;
    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int size) override;
    bool isBusesLayoutSupported(
        const BusesLayout& layouts) const override;
    juce::Result saveValidatedState(
        juce::MemoryBlock& destination) override;
    juce::Result restoreValidatedState(
        const void* data,
        int size) override;

    static juce::String outputBusName(int index);

private:
    enum class ParameterSlot
    {
        mainLevel,
        kickLevel,
        snareLevel,
        tomsLevel,
        cymbalsLevel,
        room,
        tuning,
        velocityCurve,
        count
    };

    enum class VoiceKind
    {
        kick,
        snare,
        tom,
        hat,
        cymbal
    };

    struct Voice
    {
        bool active = false;
        VoiceKind kind = VoiceKind::kick;
        int midiNote = 0;
        int chokeGroup = 0;
        int outputBus = kickOutput;
        float phase = 0.0f;
        float phaseTwo = 0.0f;
        float frequency = 60.0f;
        float targetFrequency = 60.0f;
        float frequencyTwo = 120.0f;
        float envelope = 0.0f;
        float decayMultiplier = 0.99f;
        float noiseLow = 0.0f;
        float pan = 0.0f;
        float brightness = 0.5f;
        std::uint32_t noiseState = 1;
        std::uint64_t ordinal = 0;
    };

    struct HitDescription
    {
        bool valid = false;
        bool chokeOnly = false;
        VoiceKind kind = VoiceKind::kick;
        int outputBus = kickOutput;
        int chokeGroup = 0;
        int roundRobinFamily = -1;
        int explicitVariant = -1;
        float frequency = 60.0f;
        float durationSeconds = 0.5f;
        float brightness = 0.5f;
    };

    static BusesProperties buses();
    juce::AudioParameterFloat* addFloat(
        ParameterSlot slot,
        const juce::String& id,
        const juce::String& name,
        juce::NormalisableRange<float> range,
        float defaultValue);
    [[nodiscard]] float parameter(ParameterSlot slot) const noexcept;
    [[nodiscard]] HitDescription describeHit(int note) const noexcept;
    void handleMidiMessage(const juce::MidiMessage& message) noexcept;
    void startVoice(int note, float velocity) noexcept;
    void chokeVoices(int chokeGroup, int releaseSamples) noexcept;
    void renderSamples(juce::AudioBuffer<float>& main,
                       juce::AudioBuffer<float>& kick,
                       juce::AudioBuffer<float>& snare,
                       juce::AudioBuffer<float>& toms,
                       juce::AudioBuffer<float>& cymbals,
                       int startSample,
                       int samples) noexcept;
    Voice& voiceForStart() noexcept;
    static float nextNoise(std::uint32_t& state) noexcept;

    std::vector<std::pair<juce::String, juce::AudioParameterFloat*>>
        parameterLookup;
    std::array<juce::AudioParameterFloat*,
               static_cast<std::size_t>(ParameterSlot::count)>
        realtimeParameters {};
    static constexpr int maximumVoices = 64;
    std::array<Voice, maximumVoices> voices;
    std::array<std::uint32_t, 8> roundRobinCounters {};
    juce::AudioBuffer<float> roomDelay;
    int roomWritePosition = 0;
    double currentSampleRate = 48000.0;
    float hiHatFootControl = 1.0f;
    std::uint64_t voiceOrdinal = 0;
};
}

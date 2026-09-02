#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>

namespace studio
{
enum class UtilityDeviceType
{
    equalizer,
    compressor,
    limiter,
    reverb,
    gate,
    gain,
    polarity,
    delay,
    tuner,
    generator
};

class UtilityDeviceProcessor final : public juce::AudioProcessor
{
public:
    explicit UtilityDeviceProcessor(UtilityDeviceType type);

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

    [[nodiscard]] UtilityDeviceType deviceType() const noexcept;
    [[nodiscard]] double meterValue(const juce::String& meter) const;

private:
    static BusesProperties busesFor(UtilityDeviceType type);

    enum class ParameterSlot
    {
        frequency,
        midGain,
        q,
        threshold,
        ratio,
        attack,
        release,
        makeup,
        mix,
        ceiling,
        truePeak,
        room,
        damping,
        width,
        range,
        gain,
        invert,
        samples,
        waveform,
        level,
        count
    };

    struct Biquad
    {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        float z1 = 0.0f;
        float z2 = 0.0f;

        float process(float input) noexcept;
        void reset() noexcept;
    };

    juce::AudioParameterFloat* addFloat(
        ParameterSlot slot,
        const juce::String& id,
        const juce::String& name,
        juce::NormalisableRange<float> range,
        float defaultValue);
    [[nodiscard]] float parameter(ParameterSlot slot) const noexcept;
    void processEqualizer(juce::AudioBuffer<float>& audio) noexcept;
    void processCompressor(juce::AudioBuffer<float>& audio) noexcept;
    void processLimiter(juce::AudioBuffer<float>& audio) noexcept;
    void processGate(juce::AudioBuffer<float>& audio) noexcept;
    void processDelay(juce::AudioBuffer<float>& audio) noexcept;
    void processTuner(const juce::AudioBuffer<float>& audio) noexcept;
    void processGenerator(juce::AudioBuffer<float>& audio) noexcept;

    UtilityDeviceType type;
    std::vector<std::pair<juce::String, juce::AudioParameterFloat*>>
        parameterLookup;
    std::array<juce::AudioParameterFloat*,
               static_cast<std::size_t>(ParameterSlot::count)>
        realtimeParameters {};
    double currentSampleRate = 48000.0;
    int maximumBlock = 512;
    std::array<Biquad, 2> equalizerFilters;
    std::array<float, 2> compressorEnvelope { 1.0f, 1.0f };
    std::array<float, 2> gateEnvelope {};
    std::array<float, 2> limiterGain { 1.0f, 1.0f };
    std::unique_ptr<juce::dsp::Oversampling<float>> limiterOversampling;
    juce::AudioBuffer<float> delayBuffer;
    int delayWritePosition = 0;
    juce::Reverb reverb;
    double generatorPhase = 0.0;
    std::uint32_t randomState = 0x6d2b79f5u;
    std::array<float, 2> pinkState {};
    static constexpr int tunerHistorySize = 4096;
    static constexpr int tunerDownsampleFactor = 4;
    std::array<float, tunerHistorySize> tunerHistory {};
    std::array<float, tunerHistorySize> tunerAnalysis {};
    std::array<float, tunerHistorySize / 2> tunerCorrelation {};
    int tunerWritePosition = 0;
    int tunerSamplesAvailable = 0;
    int tunerSamplesSinceAnalysis = 0;
    int tunerDecimationCount = 0;
    float tunerDecimationSum = 0.0f;
    std::atomic<double> detectedFrequency { 0.0 };
};
}

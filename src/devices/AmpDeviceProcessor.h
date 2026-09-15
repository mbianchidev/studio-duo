#pragma once

#include "plugin_host/ValidatedPluginStateTarget.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace studio
{
enum class AmpDeviceType
{
    guitar,
    bass
};

class AmpDeviceProcessor final : public juce::AudioProcessor,
                                 public ValidatedPluginStateTarget
{
public:
    explicit AmpDeviceProcessor(AmpDeviceType type);
    ~AmpDeviceProcessor() override;

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

    [[nodiscard]] AmpDeviceType ampType() const noexcept;
    juce::Result loadCabinetFile(const juce::File& file);
    juce::Result useEmbeddedDefaultCabinet();
    [[nodiscard]] juce::String cabinetDescription() const;

#if STUDIO_DUO_TESTING
    struct CabinetReaderBarrierForTesting
    {
        std::atomic<bool> slotLoaded { false };
        std::atomic<bool> resume { false };
    };

    void setCabinetReaderBarrierForTesting(
        CabinetReaderBarrierForTesting* barrier) noexcept;
#endif

private:
    class CabinetConvolver;

    enum class ParameterSlot
    {
        gain,
        bass,
        mid,
        treble,
        presence,
        blend,
        cabinetMix,
        output,
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

    static BusesProperties buses();
    juce::AudioParameterFloat* addFloat(
        ParameterSlot slot,
        const juce::String& id,
        const juce::String& name,
        juce::NormalisableRange<float> range,
        float defaultValue);
    [[nodiscard]] float parameter(ParameterSlot slot) const noexcept;
    void updateToneFilters() noexcept;
    static void setPeaking(Biquad& filter,
                           double sampleRate,
                           double frequency,
                           double q,
                           double gainDecibels) noexcept;
    static void setLowShelf(Biquad& filter,
                            double sampleRate,
                            double frequency,
                            double gainDecibels) noexcept;
    static void setHighShelf(Biquad& filter,
                             double sampleRate,
                             double frequency,
                             double gainDecibels) noexcept;

    AmpDeviceType type;
    std::vector<std::pair<juce::String, juce::AudioParameterFloat*>>
        parameterLookup;
    std::array<juce::AudioParameterFloat*,
               static_cast<std::size_t>(ParameterSlot::count)>
        realtimeParameters {};
    std::array<std::array<Biquad, 3>, 2> toneFilters;
    std::array<float, 2> inputHighPassState {};
    std::array<float, 2> bassLowState {};
    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> dryDelay;
    int dryDelayWritePosition = 0;
    std::unique_ptr<CabinetConvolver> cabinet;
    std::atomic<double> currentSampleRate { 48000.0 };
    int maximumBlock = 512;
};
}

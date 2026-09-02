#include <JuceHeader.h>

namespace
{
class AraFixtureDocumentController final
    : public juce::ARADocumentControllerSpecialisation
{
public:
    using juce::ARADocumentControllerSpecialisation::
        ARADocumentControllerSpecialisation;

protected:
    bool doRestoreObjectsFromStream(
        juce::ARAInputStream& input,
        const juce::ARARestoreObjectsFilter*) noexcept override
    {
        return input.setPosition(16)
            && input.readInt64() == archiveMarker
            && !input.failed();
    }

    bool doStoreObjectsToStream(
        juce::ARAOutputStream& output,
        const juce::ARAStoreObjectsFilter*) noexcept override
    {
        return output.setPosition(16)
            && output.writeInt64(archiveMarker);
    }

private:
    static constexpr juce::int64 archiveMarker =
        0x53545544494f4152LL;
};

class AraFixtureProcessor final
    : public juce::AudioProcessor,
      private juce::AudioProcessorARAExtension
{
public:
    AraFixtureProcessor()
        : juce::AudioProcessor(
              BusesProperties()
                  .withInput(
                      "Input",
                      juce::AudioChannelSet::stereo(),
                      true)
                  .withOutput(
                      "Output",
                      juce::AudioChannelSet::stereo(),
                      true))
    {
        gainParameter = new juce::AudioParameterFloat(
            juce::ParameterID { "gain", 1 },
            "Gain",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.5f);
        addParameter(gainParameter);
    }

    void prepareToPlay(double sampleRate, int blockSize) override
    {
        prepareToPlayForARA(
            sampleRate,
            blockSize,
            getMainBusNumOutputChannels(),
            getProcessingPrecision());
    }

    void releaseResources() override
    {
        releaseResourcesForARA();
    }

    bool isBusesLayoutSupported(
        const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet()
                   == layouts.getMainOutputChannelSet()
            && (layouts.getMainOutputChannelSet()
                    == juce::AudioChannelSet::mono()
                || layouts.getMainOutputChannelSet()
                    == juce::AudioChannelSet::stereo());
    }

    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midi) override
    {
        juce::ignoreUnused(midi);
        if (!processBlockForARA(buffer, isRealtime(), getPlayHead()))
            buffer.applyGain(gainParameter->get());
    }

    const juce::String getName() const override
    {
        return "Studio Duo ARA Fixture";
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const juce::String getProgramName(int) override
    {
        return "Default";
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    void getStateInformation(juce::MemoryBlock& state) override
    {
        const auto gain = gainParameter->get();
        state.setSize(sizeof(gain), false);
        std::memcpy(state.getData(), &gain, sizeof(gain));
    }

    void setStateInformation(const void* data, int size) override
    {
        if (data == nullptr || size != sizeof(float))
            return;
        auto gain = 0.0f;
        std::memcpy(&gain, data, sizeof(gain));
        *gainParameter = juce::jlimit(0.0f, 1.0f, gain);
    }

    bool hasEditor() const override
    {
        return false;
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    juce::AudioProcessorARAExtension* getARAClientExtensions() override
    {
        return this;
    }

private:
    juce::AudioParameterFloat* gainParameter = nullptr;
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AraFixtureProcessor();
}

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::
        createARAFactory<AraFixtureDocumentController>();
}

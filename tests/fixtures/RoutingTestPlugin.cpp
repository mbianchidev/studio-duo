#include <JuceHeader.h>

namespace
{
class RoutingFixtureProcessor final : public juce::AudioProcessor
{
public:
    RoutingFixtureProcessor()
        : juce::AudioProcessor(
              BusesProperties()
                  .withInput(
                      "Input",
                      juce::AudioChannelSet::stereo(),
                      true)
                  .withInput(
                      "Sidechain",
                      juce::AudioChannelSet::stereo(),
                      false)
                  .withOutput(
                      "Output",
                      juce::AudioChannelSet::stereo(),
                      true))
    {
    }

    void prepareToPlay(double, int) override
    {
    }

    void releaseResources() override
    {
    }

    bool isBusesLayoutSupported(
        const BusesLayout& layouts) const override
    {
        const auto input = layouts.getMainInputChannelSet();
        const auto output = layouts.getMainOutputChannelSet();
        const auto sidechain = layouts.inputBuses.size() > 1
            ? layouts.inputBuses[1]
            : juce::AudioChannelSet::disabled();
        return input == output
            && (output == juce::AudioChannelSet::mono()
                || output == juce::AudioChannelSet::stereo())
            && (sidechain.isDisabled()
                || sidechain == juce::AudioChannelSet::mono()
                || sidechain == juce::AudioChannelSet::stereo());
    }

    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midi) override
    {
        if (getBusCount(true) > 1
            && getChannelCountOfBus(true, 1) > 0)
        {
            const auto sidechain = getBusBuffer(buffer, true, 1);
            auto main = getBusBuffer(buffer, false, 0);
            for (int channel = 0;
                 channel < std::min(
                     main.getNumChannels(),
                     sidechain.getNumChannels());
                 ++channel)
            {
                main.addFrom(
                    channel,
                    0,
                    sidechain,
                    channel,
                    0,
                    main.getNumSamples());
            }
        }

        auto main = getBusBuffer(buffer, false, 0);
        for (const auto metadata : midi)
        {
            if (!metadata.getMessage().isNoteOn())
                continue;
            const auto sample = juce::jlimit(
                0,
                std::max(0, main.getNumSamples() - 1),
                metadata.samplePosition);
            const auto value = metadata.getMessage().getFloatVelocity();
            for (int channel = 0; channel < main.getNumChannels(); ++channel)
                main.addSample(channel, sample, value);
        }
    }

    const juce::String getName() const override
    {
        return "Studio Duo Routing Fixture";
    }

    bool acceptsMidi() const override
    {
        return true;
    }

    bool producesMidi() const override
    {
        return true;
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

    void getStateInformation(juce::MemoryBlock&) override
    {
    }

    void setStateInformation(const void*, int) override
    {
    }

    bool hasEditor() const override
    {
        return false;
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RoutingFixtureProcessor();
}

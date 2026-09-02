#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace studio
{
class ClapPluginInstance final : public juce::AudioPluginInstance,
                                 private juce::Timer
{
public:
    static std::unique_ptr<ClapPluginInstance> create(
        const juce::PluginDescription& description,
        double sampleRate,
        int blockSize,
        juce::String& error);

    ~ClapPluginInstance() override;

    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
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
    void fillInPluginDescription(
        juce::PluginDescription& description) const override;
    bool isBusesLayoutSupported(
        const BusesLayout& layouts) const override;

private:
    class Impl;

    ClapPluginInstance(std::unique_ptr<Impl> implementation,
                       const BusesProperties& buses);
    static BusesProperties busesFor(const Impl& implementation);
    void timerCallback() override;

    std::unique_ptr<Impl> impl;
};
}

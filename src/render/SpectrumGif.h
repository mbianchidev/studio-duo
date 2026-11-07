#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <optional>
#include <vector>

namespace studio
{
struct SpectrumGifSettings
{
    int width = 1280;
    int height = 720;
    int framesPerSecond = 25;
    int barCount = 64;
    bool transparentBackground = true;
    juce::Colour barColour { 0xff68d8e6 };
    juce::Colour backgroundColour { 0xff1d2024 };
};

class SpectrumGif
{
public:
    static juce::Result validate(const SpectrumGifSettings&);
    static std::optional<std::vector<float>> analyseFrame(
        const juce::AudioBuffer<float>&, double sampleRate, double centreSeconds,
        int barCount, juce::String& error);
    static juce::Image drawFrame(const std::vector<float>&, const SpectrumGifSettings&);
    static juce::Result write(
        const juce::AudioBuffer<float>&, double sampleRate, const juce::File&,
        const SpectrumGifSettings&,
        const std::function<bool(double)>& progress = {});
};
}

#include "TestHarness.h"
#include "TestSuites.h"

#include "render/SpectrumGif.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace
{
struct GifFrame
{
    juce::Image image;
    int delay = 0;
    int disposal = 0;
    bool transparent = false;
};

struct Gif
{
    std::vector<GifFrame> frames;
    bool loops = false;
};

std::optional<Gif> decodeGif(const juce::File& file)
{
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() < 19)
        return std::nullopt;
    juce::MemoryInputStream stream(bytes, false);
    std::array<std::uint8_t, 13> header {};
    if (stream.read(header.data(), static_cast<int>(header.size())) != static_cast<int>(header.size())
        || std::memcmp(header.data(), "GIF89a", 6) != 0 || (header[10] & 0x80) == 0)
        return std::nullopt;
    const auto prefixSize = 13 + 3 * (1 << ((header[10] & 7) + 1));
    if (!stream.setPosition(prefixSize))
        return std::nullopt;
    const auto subBlocks = [&stream](juce::MemoryOutputStream& output)
    {
        for (;;)
        {
            if (stream.isExhausted())
                return false;
            const auto count = static_cast<std::uint8_t>(stream.readByte());
            output.writeByte(static_cast<char>(count));
            std::array<std::uint8_t, 255> block {};
            if (stream.read(block.data(), count) != count)
                return false;
            output.write(block.data(), count);
            if (count == 0)
                return true;
        }
    };
    Gif result;
    std::array<std::uint8_t, 8> control {};
    while (!stream.isExhausted())
    {
        const auto kind = static_cast<std::uint8_t>(stream.readByte());
        if (kind == 0x3b)
            return !result.frames.empty() && stream.isExhausted()
                ? std::optional<Gif>(std::move(result)) : std::nullopt;
        if (kind == 0x21)
        {
            const auto extension = static_cast<std::uint8_t>(stream.readByte());
            if (extension == 0xf9)
            {
                control[0] = 0x21;
                control[1] = 0xf9;
                if (stream.read(control.data() + 2, 6) != 6 || control[2] != 4 || control[7] != 0)
                    return std::nullopt;
            }
            else
            {
                juce::MemoryOutputStream blocks;
                if (!subBlocks(blocks))
                    return std::nullopt;
                if (extension == 0xff && blocks.getDataSize() == 17)
                {
                    const auto* data = static_cast<const std::uint8_t*>(blocks.getData());
                    result.loops = data[0] == 11 && std::memcmp(data + 1, "NETSCAPE2.0", 11) == 0
                        && data[12] == 3 && data[13] == 1 && data[14] == 0 && data[15] == 0;
                }
            }
            continue;
        }
        if (kind != 0x2c)
            return std::nullopt;
        std::array<std::uint8_t, 9> descriptor {};
        if (stream.read(descriptor.data(), static_cast<int>(descriptor.size())) != static_cast<int>(descriptor.size())
            || descriptor[8] != 0 || control[0] == 0)
            return std::nullopt;
        juce::MemoryOutputStream single;
        single.write(bytes.getData(), static_cast<std::size_t>(prefixSize));
        single.write(control.data(), control.size());
        single.writeByte(0x2c);
        single.write(descriptor.data(), descriptor.size());
        single.writeByte(stream.readByte());
        if (!subBlocks(single))
            return std::nullopt;
        single.writeByte(0x3b);
        GifFrame frame;
        frame.image = juce::ImageFileFormat::loadFrom(single.getData(), single.getDataSize());
        if (!frame.image.isValid())
            return std::nullopt;
        frame.delay = control[4] | (static_cast<int>(control[5]) << 8);
        frame.disposal = (control[3] >> 2) & 7;
        frame.transparent = (control[3] & 1) != 0;
        result.frames.push_back(std::move(frame));
        control = {};
    }
    return std::nullopt;
}

juce::AudioBuffer<float> tone(double frequency, int samples, bool oppositePhase = false)
{
    constexpr double sampleRate = 48000.0;
    juce::AudioBuffer<float> audio(2, samples);
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(0.5 * std::sin(
            juce::MathConstants<double>::twoPi * frequency * sample / sampleRate));
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, oppositePhase ? -value : value);
    }
    return audio;
}

void transparentAnimation()
{
    auto audio = tone(997.0, 4800, true);
    studio::SpectrumGifSettings settings;
    settings.width = 160;
    settings.height = 90;
    settings.barCount = 32;
    juce::TemporaryFile output(".gif");
    const auto result = studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings);
    expect(result.wasOk(), result.getErrorMessage().toRawUTF8());
    const auto image = juce::ImageFileFormat::loadFrom(output.getFile());
    expect(image.isValid() && image.getWidth() == 160 && image.getHeight() == 90
               && image.hasAlphaChannel() && image.getPixelAt(0, 0).isTransparent(),
           "Spectrum export writes a decodable transparent GIF at the requested canvas size.");
    const auto gif = decodeGif(output.getFile());
    expect(gif && gif->loops && gif->frames.size() == 3
               && gif->frames[0].delay == 4 && gif->frames[1].delay == 4 && gif->frames[2].delay == 2
               && std::all_of(gif->frames.begin(), gif->frames.end(), [](const auto& frame)
               { return frame.disposal == 2 && frame.transparent; }),
           "Every frame decodes, loops, clears old transparent bars and preserves the partial final frame.");
    if (gif)
    {
        auto visible = false;
        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
                visible = visible || image.getPixelAt(x, y).isOpaque();
        expect(visible, "Opposite-phase stereo still produces visible frequency bars.");
    }
}

void frequencyAndStereoAnalysis()
{
    juce::String error;
    const auto audio = tone(997.0, 48000);
    const auto opposite = tone(997.0, 48000, true);
    const auto levels = studio::SpectrumGif::analyseFrame(audio, 48000.0, 0.5, 64, error);
    const auto inverted = studio::SpectrumGif::analyseFrame(opposite, 48000.0, 0.5, 64, error);
    expect(levels && inverted && *levels == *inverted,
           "Stereo power analysis is invariant under polarity inversion of either channel.");
    if (levels)
    {
        const auto strongest = std::distance(levels->begin(), std::max_element(levels->begin(), levels->end()));
        const auto expected = static_cast<int>(std::log(997.0 / 20.0) / std::log(1000.0) * 64);
        expect(std::abs(strongest - expected) <= 1 && (*levels)[static_cast<std::size_t>(strongest)] > 0.85f,
               "A synthetic 997 Hz tone peaks in the corresponding logarithmic frequency band.");
    }
    juce::AudioBuffer<float> silence(1, 4800);
    silence.clear();
    const auto quiet = studio::SpectrumGif::analyseFrame(silence, 48000.0, 0.05, 64, error);
    expect(quiet && std::all_of(quiet->begin(), quiet->end(), [](float level) { return level == 0.0f; }),
           "Silence has no manufactured spectrum or noise floor bars.");
    auto rightOnly = audio;
    rightOnly.clear(0, 0, rightOnly.getNumSamples());
    const auto right = studio::SpectrumGif::analyseFrame(rightOnly, 48000.0, 0.5, 64, error);
    expect(right && *std::max_element(right->begin(), right->end()) > 0.8f,
           "A signal present only on the right channel contributes to the animation.");
    expect(!studio::SpectrumGif::analyseFrame(audio, 48000.0, -1.0, 64, error) && error.isNotEmpty(),
           "Invalid analysis positions report an explicit error.");
}

void clockAndSilentGaps()
{
    juce::AudioBuffer<float> audio(1, 30 * 8000);
    audio.clear();
    studio::SpectrumGifSettings settings;
    settings.width = 64;
    settings.height = 36;
    settings.barCount = 8;
    settings.framesPerSecond = 30;
    juce::TemporaryFile output(".gif");
    expect(studio::SpectrumGif::write(audio, 8000.0, output.getFile(), settings).wasOk(),
           "Song-length silent gaps export successfully.");
    const auto gif = decodeGif(output.getFile());
    expect(gif && gif->frames.size() == 900
               && std::accumulate(gif->frames.begin(), gif->frames.end(), 0,
                    [](int sum, const auto& frame) { return sum + frame.delay; }) == 3000
               && std::all_of(gif->frames.begin(), gif->frames.end(), [](const auto& frame)
                    { return frame.delay == 3 || frame.delay == 4; }),
           "30 fps uses 3/4-centisecond delays without accumulating drift over the song.");
    if (gif)
        expect(gif->frames.back().image.getPixelAt(32, 30).isTransparent(),
               "Silence remains transparent all the way to the end of the animation.");

    auto changing = tone(997.0, 48000, true);
    changing.clear(0, 4800, 43200);
    changing.clear(1, 4800, 43200);
    settings.width = 160;
    settings.height = 90;
    settings.barCount = 32;
    settings.framesPerSecond = 25;
    expect(studio::SpectrumGif::write(changing, 48000.0, output.getFile(), settings).wasOk(),
           "A tone followed by silence exports without dropping frames.");
    const auto decay = decodeGif(output.getFile());
    if (decay)
    {
        auto remainingBars = false;
        const auto& last = decay->frames.back().image;
        for (int y = 0; y < last.getHeight(); ++y)
            for (int x = 0; x < last.getWidth(); ++x)
                remainingBars = remainingBars || last.getPixelAt(x, y).isOpaque();
        expect(!remainingBars, "Release smoothing decays completely instead of leaving persistent bars in silence.");
    }
    else
        expect(false, "The decay animation can be decoded frame by frame.");
}

void largeFrameAndPalette()
{
    auto audio = tone(440.0, 1920, true);
    juce::Random random(0x12345);
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto value = random.nextFloat() - 0.5f;
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, -value);
    }
    studio::SpectrumGifSettings settings;
    settings.width = 1920;
    settings.height = 1080;
    settings.transparentBackground = false;
    settings.barColour = juce::Colour(0xffffc66d);
    juce::TemporaryFile output(".gif");
    expect(studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings).wasOk(),
           "A full-HD spectrum encodes with a solid background and the chosen palette.");
    const auto gif = decodeGif(output.getFile());
    juce::String error;
    const auto levels = studio::SpectrumGif::analyseFrame(audio, 48000.0, 0.02, settings.barCount, error);
    expect(gif && gif->frames.size() == 1 && levels, "A full-HD GIF frame decodes successfully.");
    if (!gif || !levels)
        return;
    const auto expected = studio::SpectrumGif::drawFrame(*levels, settings);
    const auto& decoded = gif->frames[0].image;
    auto matches = decoded.getWidth() == settings.width && decoded.getHeight() == settings.height
        && !gif->frames[0].transparent;
    for (int y = 0; matches && y < settings.height; ++y)
        for (int x = 0; matches && x < settings.width; ++x)
            matches = decoded.getPixelAt(x, y) == expected.getPixelAt(x, y);
    expect(matches, "LZW dictionary growth/reset preserves every full-HD pixel and palette color.");
}

void invalidInputAndAtomicCancellation()
{
    auto audio = tone(997.0, 4800);
    studio::SpectrumGifSettings settings;
    juce::TemporaryFile output(".gif");
    expect(output.getFile().replaceWithText("previous destination"), "The previous destination fixture can be written.");
    const auto unchanged = [&] { return output.getFile().loadFileAsString() == "previous destination"; };
    expect(studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings,
               [](double progress) { return progress < 0.5; }).failed() && unchanged(),
           "Canceling after encoding starts preserves the previous destination file.");
    expect(studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings,
               [](double) { return false; }).failed() && unchanged(),
           "Canceling before encoding starts preserves the previous destination file.");
    for (const auto rate : { 0.0, -1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN() })
        expect(studio::SpectrumGif::write(audio, rate, output.getFile(), settings).failed() && unchanged(),
               "Invalid sample rates fail without publishing an export.");
    auto invalid = settings;
    invalid.width = std::numeric_limits<int>::max();
    expect(studio::SpectrumGif::validate(invalid).failed(), "Canvas sizes are bounded before allocation.");
    invalid = settings;
    invalid.framesPerSecond = 60;
    expect(studio::SpectrumGif::validate(invalid).failed(), "Unsupported GIF frame rates are rejected explicitly.");
    invalid = settings;
    invalid.barCount = 0;
    expect(studio::SpectrumGif::validate(invalid).failed(), "An empty spectrum is rejected.");
    invalid = settings;
    invalid.barColour = juce::Colours::transparentBlack;
    expect(studio::SpectrumGif::validate(invalid).failed(), "GIF bars cannot silently disappear through alpha.");
    audio.setSample(1, 4799, std::numeric_limits<float>::quiet_NaN());
    expect(studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings).failed() && unchanged(),
           "Nonfinite samples anywhere in the audio fail before publishing a GIF.");
    audio.clear();
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        audio.setSample(0, sample, std::numeric_limits<float>::max());
    expect(studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings).failed() && unchanged(),
           "Overflow during FFT analysis fails explicitly instead of publishing fabricated spectrum levels.");
    audio.setSize(0, 0);
    expect(studio::SpectrumGif::write(audio, 48000.0, output.getFile(), settings).failed() && unchanged(),
           "Empty audio fails without replacing the previous file.");
}
}

void spectrumGifTests()
{
    transparentAnimation();
    frequencyAndStereoAnalysis();
    clockAndSilentGaps();
    largeFrameAndPalette();
    invalidInputAndAtomicCancellation();
}

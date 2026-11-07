#include "SpectrumGif.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <new>

namespace studio
{
namespace
{
constexpr int fftOrder = 12;
constexpr int fftSize = 1 << fftOrder;
constexpr float floorDecibels = -72.0f;

juce::Result validateAudio(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 192000.0)
        return juce::Result::fail("Spectrum analysis requires a sample rate between 8 and 192 kHz.");
    if (audio.getNumSamples() <= 0
        || (audio.getNumChannels() != 1 && audio.getNumChannels() != 2))
        return juce::Result::fail("Spectrum export requires non-empty mono or stereo audio.");
    return juce::Result::ok();
}

class Analyser
{
public:
    Analyser(double rate, int bars) : sampleRate(rate), barCount(bars)
    {
        for (int sample = 0; sample < fftSize; ++sample)
        {
            window[static_cast<std::size_t>(sample)] = static_cast<float>(
                0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi
                                     * sample / (fftSize - 1)));
            windowSum += window[static_cast<std::size_t>(sample)];
        }
    }

    std::optional<std::vector<float>> analyse(
        const juce::AudioBuffer<float>& audio, double centreSeconds, juce::String& error)
    {
        std::array<double, fftSize / 2 + 1> power {};
        const auto start = static_cast<std::int64_t>(std::llround(centreSeconds * sampleRate))
            - fftSize / 2;
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            data.fill(0.0f);
            for (int sample = 0; sample < fftSize; ++sample)
            {
                const auto source = start + sample;
                if (source < 0 || source >= audio.getNumSamples())
                    continue;
                const auto value = audio.getSample(channel, static_cast<int>(source));
                if (!std::isfinite(value))
                {
                    error = "Spectrum export cannot analyze NaN or infinite audio samples.";
                    return std::nullopt;
                }
                data[static_cast<std::size_t>(sample)] =
                    value * window[static_cast<std::size_t>(sample)];
            }
            fft.performFrequencyOnlyForwardTransform(data.data(), true);
            for (std::size_t bin = 0; bin < power.size(); ++bin)
            {
                if (!std::isfinite(data[bin]))
                {
                    error = "Spectrum analysis overflowed. Reduce the audio gain before exporting.";
                    return std::nullopt;
                }
                power[bin] += static_cast<double>(data[bin]) * data[bin]
                    / audio.getNumChannels();
            }
        }
        std::vector<float> levels(static_cast<std::size_t>(barCount));
        const auto maximumFrequency = std::min(20000.0, sampleRate * 0.5);
        const auto frequencyRatio = maximumFrequency / 20.0;
        const auto binWidth = sampleRate / fftSize;
        for (int bar = 0; bar < barCount; ++bar)
        {
            const auto low = 20.0 * std::pow(frequencyRatio, static_cast<double>(bar) / barCount);
            const auto high = 20.0 * std::pow(frequencyRatio, static_cast<double>(bar + 1) / barCount);
            const auto first = juce::jlimit(1, fftSize / 2, static_cast<int>(std::floor(low / binWidth)));
            const auto last = juce::jlimit(first, fftSize / 2, static_cast<int>(std::ceil(high / binWidth)));
            double peakPower = 0.0;
            for (int bin = first; bin <= last; ++bin)
                peakPower = std::max(peakPower, power[static_cast<std::size_t>(bin)]);
            const auto amplitude = std::sqrt(peakPower) * 2.0 / windowSum;
            const auto decibels = juce::Decibels::gainToDecibels(amplitude, static_cast<double>(floorDecibels));
            levels[static_cast<std::size_t>(bar)] = static_cast<float>(
                juce::jlimit(0.0, 1.0, (decibels - floorDecibels) / -floorDecibels));
        }
        return levels;
    }

private:
    double sampleRate;
    int barCount;
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> window {};
    std::array<float, fftSize * 2> data {};
    double windowSum = 0.0;
};

std::vector<std::uint8_t> rasterize(
    const std::vector<float>& levels, const SpectrumGifSettings& settings)
{
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(settings.width) * static_cast<std::size_t>(settings.height), 0);
    const auto insetX = settings.width / 32;
    const auto insetY = settings.height / 16;
    const auto cellWidth = static_cast<double>(settings.width - 2 * insetX) / settings.barCount;
    const auto barWidth = std::max(1, static_cast<int>(std::floor(cellWidth * 0.72)));
    const auto baseline = settings.height - insetY;
    for (int bar = 0; bar < settings.barCount; ++bar)
    {
        const auto level = levels[static_cast<std::size_t>(bar)];
        const auto height = static_cast<int>(std::lround(
            juce::jlimit(0.0f, 1.0f, level) * static_cast<float>(settings.height - 2 * insetY)));
        const auto left = insetX + static_cast<int>(std::floor(
            bar * cellWidth + (cellWidth - barWidth) * 0.5));
        for (int y = baseline - height; y < baseline; ++y)
            std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(y * settings.width + left),
                        barWidth, std::uint8_t { 1 });
    }
    return pixels;
}

class GifCodes
{
public:
    explicit GifCodes(juce::OutputStream& output) : stream(output) {}

    bool write(int code, int width)
    {
        bits |= static_cast<std::uint32_t>(code) << bitCount;
        bitCount += width;
        while (bitCount >= 8)
        {
            if (!byte(static_cast<std::uint8_t>(bits & 0xff)))
                return false;
            bits >>= 8;
            bitCount -= 8;
        }
        return true;
    }

    bool finish()
    {
        if (bitCount > 0 && !byte(static_cast<std::uint8_t>(bits & 0xff)))
            return false;
        return flush() && stream.writeByte(0);
    }

private:
    bool byte(std::uint8_t value)
    {
        block[used++] = value;
        return used < block.size() || flush();
    }

    bool flush()
    {
        if (used == 0)
            return true;
        const auto result = stream.writeByte(static_cast<char>(used))
            && stream.write(block.data(), used);
        used = 0;
        return result;
    }

    juce::OutputStream& stream;
    std::array<std::uint8_t, 255> block {};
    std::size_t used = 0;
    std::uint32_t bits = 0;
    int bitCount = 0;
};

bool writePixels(juce::OutputStream& stream, const std::vector<std::uint8_t>& pixels)
{
    if (!stream.writeByte(2))
        return false;
    GifCodes codes(stream);
    constexpr int clearCode = 4, endCode = 5;
    std::array<std::array<std::uint16_t, 2>, 4096> dictionary {};
    int nextCode = 6, width = 3;
    if (!codes.write(clearCode, width))
        return false;
    int prefix = pixels.front();
    for (std::size_t index = 1; index < pixels.size(); ++index)
    {
        const auto pixel = pixels[index];
        auto& entry = dictionary[static_cast<std::size_t>(prefix)][pixel];
        if (entry != 0)
        {
            prefix = entry;
            continue;
        }
        if (!codes.write(prefix, width))
            return false;
        if (nextCode < 4096)
        {
            entry = static_cast<std::uint16_t>(nextCode++);
            // The decoder adds each dictionary entry one emitted code later.
            if (nextCode > (1 << width) && width < 12)
                ++width;
        }
        else
        {
            if (!codes.write(clearCode, width))
                return false;
            dictionary = {};
            nextCode = 6;
            width = 3;
        }
        prefix = pixel;
    }
    if (!codes.write(prefix, width))
        return false;
    if (nextCode == (1 << width) && width < 12)
        ++width;
    return codes.write(endCode, width) && codes.finish();
}

bool writeHeader(juce::OutputStream& stream, const SpectrumGifSettings& settings)
{
    constexpr std::array<std::uint8_t, 19> loop {
        0x21, 0xff, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
        3, 1, 0, 0, 0
    };
    const std::array<std::uint8_t, 6> palette {
        settings.backgroundColour.getRed(), settings.backgroundColour.getGreen(),
        settings.backgroundColour.getBlue(), settings.barColour.getRed(),
        settings.barColour.getGreen(), settings.barColour.getBlue()
    };
    return stream.write("GIF89a", 6)
        && stream.writeShort(static_cast<short>(settings.width))
        && stream.writeShort(static_cast<short>(settings.height))
        && stream.writeByte(static_cast<char>(0xf0))
        && stream.writeShort(0)
        && stream.write(palette.data(), palette.size())
        && stream.write(loop.data(), loop.size());
}

bool writeFrame(juce::OutputStream& stream, const SpectrumGifSettings& settings,
                const std::vector<std::uint8_t>& pixels, int delay)
{
    const std::array<std::uint8_t, 4> control {
        0x21, 0xf9, 4,
        static_cast<std::uint8_t>(8 | (settings.transparentBackground ? 1 : 0))
    };
    return stream.write(control.data(), control.size())
        && stream.writeShort(static_cast<short>(delay))
        && stream.writeShort(0)
        && stream.writeByte(0x2c)
        && stream.writeInt(0)
        && stream.writeShort(static_cast<short>(settings.width))
        && stream.writeShort(static_cast<short>(settings.height))
        && stream.writeByte(0)
        && writePixels(stream, pixels);
}
}

juce::Result SpectrumGif::validate(const SpectrumGifSettings& settings)
{
    if (settings.width < 64 || settings.width > 1920
        || settings.height < 36 || settings.height > 1080)
        return juce::Result::fail("Choose a GIF canvas between 64 x 36 and 1920 x 1080 pixels.");
    if (settings.framesPerSecond != 10 && settings.framesPerSecond != 20
        && settings.framesPerSecond != 25 && settings.framesPerSecond != 30)
        return juce::Result::fail("Choose 10, 20, 25 or 30 frames per second.");
    if (settings.barCount < 8 || settings.barCount > 128
        || settings.barCount > settings.width / 4)
        return juce::Result::fail("Choose 8 to 128 spectrum bars, with at least four pixels per bar.");
    if (!settings.barColour.isOpaque() || !settings.backgroundColour.isOpaque())
        return juce::Result::fail("GIF bar and background colors must be opaque; use the transparency option for overlays.");
    if (!settings.transparentBackground && settings.barColour == settings.backgroundColour)
        return juce::Result::fail("The spectrum bars must have a different color from the background.");
    return juce::Result::ok();
}

std::optional<std::vector<float>> SpectrumGif::analyseFrame(
    const juce::AudioBuffer<float>& audio, double sampleRate, double centreSeconds,
    int barCount, juce::String& error)
{
    error.clear();
    if (const auto valid = validateAudio(audio, sampleRate); valid.failed())
        error = valid.getErrorMessage();
    else if (!std::isfinite(centreSeconds) || centreSeconds < 0.0
             || centreSeconds > static_cast<double>(audio.getNumSamples()) / sampleRate)
        error = "Choose a finite spectrum position within the audio.";
    else if (barCount < 8 || barCount > 128)
        error = "Spectrum analysis requires 8 to 128 bars.";
    if (error.isNotEmpty())
        return std::nullopt;
    Analyser analyser(sampleRate, barCount);
    return analyser.analyse(audio, centreSeconds, error);
}

juce::Image SpectrumGif::drawFrame(
    const std::vector<float>& levels, const SpectrumGifSettings& settings)
{
    if (const auto valid = validate(settings); valid.failed())
    {
        juce::Logger::writeToLog(valid.getErrorMessage());
        return {};
    }
    if (levels.size() != static_cast<std::size_t>(settings.barCount)
        || std::any_of(levels.begin(), levels.end(), [](float value) { return !std::isfinite(value); }))
    {
        juce::Logger::writeToLog("Spectrum drawing requires one finite level per bar.");
        return {};
    }
    const auto pixels = rasterize(levels, settings);
    juce::Image image(juce::Image::ARGB, settings.width, settings.height, true);
    juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::writeOnly);
    const auto background = settings.transparentBackground ? juce::Colours::transparentBlack
                                                           : settings.backgroundColour;
    for (int y = 0; y < settings.height; ++y)
        for (int x = 0; x < settings.width; ++x)
            bitmap.setPixelColour(x, y, pixels[static_cast<std::size_t>(y * settings.width + x)] != 0
                ? settings.barColour : background);
    return image;
}

juce::Result SpectrumGif::write(
    const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& destination,
    const SpectrumGifSettings& settings, const std::function<bool(double)>& progress)
{
    if (const auto valid = validate(settings); valid.failed())
        return valid;
    if (const auto valid = validateAudio(audio, sampleRate); valid.failed())
        return valid;
    if (destination == juce::File() || destination.isDirectory())
        return juce::Result::fail("Choose a GIF file, not a directory, as the spectrum destination.");
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            if (!std::isfinite(audio.getSample(channel, sample)))
                return juce::Result::fail("Spectrum export cannot encode NaN or infinite audio samples.");
    if (progress && !progress(0.0))
        return juce::Result::fail("Spectrum GIF export was canceled.");
    try
    {
        if (const auto created = destination.getParentDirectory().createDirectory(); created.failed())
            return juce::Result::fail("Could not create the GIF export directory: " + created.getErrorMessage());
        juce::TemporaryFile staged(destination, juce::TemporaryFile::useHiddenFile);
        auto stream = staged.getFile().createOutputStream();
        if (stream == nullptr || !writeHeader(*stream, settings))
            return juce::Result::fail("Could not open or write the staged spectrum GIF.");

        const auto duration = std::max<std::int64_t>(
            1, std::llround(static_cast<double>(audio.getNumSamples()) * 100.0 / sampleRate));
        const auto frameCount = (duration * settings.framesPerSecond + 99) / 100;
        Analyser analyser(sampleRate, settings.barCount);
        std::vector<float> previous(static_cast<std::size_t>(settings.barCount), 0.0f);
        for (std::int64_t frame = 0; frame < frameCount; ++frame)
        {
            const auto start = (frame * 100 + settings.framesPerSecond / 2) / settings.framesPerSecond;
            const auto end = std::min(duration,
                ((frame + 1) * 100 + settings.framesPerSecond / 2) / settings.framesPerSecond);
            if (end <= start)
                continue;
            const auto centre = std::min(
                static_cast<double>(audio.getNumSamples()) / sampleRate,
                static_cast<double>(start + end) * 0.005);
            juce::String error;
            auto levels = analyser.analyse(audio, centre, error);
            if (!levels)
                return juce::Result::fail(error);
            const auto release = static_cast<float>(std::exp(-static_cast<double>(end - start) * 0.01 / 0.18));
            for (std::size_t bar = 0; bar < levels->size(); ++bar)
            {
                previous[bar] = std::max((*levels)[bar], previous[bar] * release);
                if (previous[bar] < 0.01f)
                    previous[bar] = 0.0f;
            }
            if (!writeFrame(*stream, settings, rasterize(previous, settings), static_cast<int>(end - start)))
                return juce::Result::fail("Could not write spectrum GIF frames. Check free space and destination permissions.");
            if (progress && !progress(static_cast<double>(frame + 1) / static_cast<double>(frameCount)))
                return juce::Result::fail("Spectrum GIF export was canceled.");
        }
        if (!stream->writeByte(0x3b))
            return juce::Result::fail("Could not finalize the spectrum GIF.");
        stream->flush();
        if (stream->getStatus().failed())
            return juce::Result::fail("Could not save the spectrum GIF: " + stream->getStatus().getErrorMessage());
        stream.reset();
        if (!staged.overwriteTargetFileWithTemporary())
            return juce::Result::fail("The GIF encoded successfully, but could not replace " + destination.getFullPathName() + ".");
        return juce::Result::ok();
    }
    catch (const std::bad_alloc&)
    {
        return juce::Result::fail("There is not enough memory to prepare the spectrum GIF.");
    }
}
}

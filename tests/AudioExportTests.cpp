#include "render/AudioExport.h"
#include "TestHarness.h"
#include "TestSuites.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <memory>

namespace
{
using studio::AudioExport;
using studio::AudioExportDither;
using studio::AudioExportFormat;
using studio::AudioExportSettings;
using studio::Mp3BitrateMode;

struct ExportDirectory
{
    ExportDirectory()
        : file(juce::File::getCurrentWorkingDirectory().getChildFile(
              ".audio-export-tests-" + juce::Uuid().toString()))
    {
        expect(file.createDirectory().wasOk(), "Create an isolated worktree export fixture.");
    }

    ~ExportDirectory() { file.deleteRecursively(); }

    juce::File file;
};

juce::AudioBuffer<float> signal(int samples = 4096, bool complex = false)
{
    juce::AudioBuffer<float> result(2, samples);
    juce::Random random(741);
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto value = complex
            ? static_cast<float>(0.35 * std::sin(sample * 0.041)
                                 + 0.12 * std::sin(sample * 0.151)
                                 + 0.015 * (random.nextFloat() - 0.5f))
            : static_cast<float>((sample * 37) % 1024 - 512) / 2048.0f;
        result.setSample(0, sample, value);
        result.setSample(1, sample, value * -0.5f);
    }
    return result;
}

bool encode(const juce::AudioBuffer<float>& audio,
            const juce::File& file,
            const AudioExportSettings& settings,
            const juce::StringPairArray& metadata = {})
{
    const auto result = AudioExport::write(audio, file, settings, metadata);
    expect(result.wasOk(), ("Encode " + file.getFileName() + ": "
                            + result.getErrorMessage()).toRawUTF8());
    return result.wasOk();
}

std::unique_ptr<juce::AudioFormatReader> readerFor(const juce::File& file,
                                                 AudioExportFormat format)
{
    auto input = file.createInputStream();
    if (input == nullptr)
        return {};
    std::unique_ptr<juce::AudioFormat> codec;
    switch (format)
    {
        case AudioExportFormat::wav: codec = std::make_unique<juce::WavAudioFormat>(); break;
        case AudioExportFormat::aiff: codec = std::make_unique<juce::AiffAudioFormat>(); break;
        case AudioExportFormat::flac: codec = std::make_unique<juce::FlacAudioFormat>(); break;
        case AudioExportFormat::oggVorbis: codec = std::make_unique<juce::OggVorbisAudioFormat>(); break;
        case AudioExportFormat::mp3: codec = std::make_unique<juce::MP3AudioFormat>(); break;
    }
    return std::unique_ptr<juce::AudioFormatReader>(
        codec->createReaderFor(input.release(), true));
}

juce::MemoryBlock bytes(const juce::File& file)
{
    juce::MemoryBlock result;
    expect(file.loadFileAsData(result), "Read encoded fixture bytes.");
    return result;
}

bool contains(const juce::MemoryBlock& data, const char* text)
{
    const auto* begin = static_cast<const char*>(data.getData());
    return std::search(begin, begin + data.getSize(), text, text + std::strlen(text))
        != begin + data.getSize();
}

void expectMp3GaplessTag(const juce::File& file, int originalFrames)
{
    const auto data = bytes(file);
    const auto* raw = static_cast<const unsigned char*>(data.getData());
    size_t start = 0;
    if (data.getSize() >= 10 && std::memcmp(raw, "ID3", 3) == 0)
        start = 10U + (static_cast<size_t>(raw[6] & 0x7f) << 21)
            + (static_cast<size_t>(raw[7] & 0x7f) << 14)
            + (static_cast<size_t>(raw[8] & 0x7f) << 7) + (raw[9] & 0x7f);
    if (start + 4 > data.getSize())
    {
        expect(false, "MP3 includes an MPEG header after its optional ID3 tag.");
        return;
    }
    const auto header = juce::ByteOrder::bigEndianInt(raw + start);
    const auto mono = ((header >> 6) & 3U) == 3U;
    const auto xing = start + 4 + (mono ? 17U : 32U)
        + ((header & 0x10000U) != 0 ? 0U : 2U);
    if (xing + 8 > data.getSize())
    {
        expect(false, "MP3 has room for the Xing/Info frame.");
        return;
    }
    expect(std::memcmp(raw + xing, "Xing", 4) == 0 || std::memcmp(raw + xing, "Info", 4) == 0,
           "MP3 seek information is at the correct frame offset, including mono and ID3 tags.");
    const auto flags = juce::ByteOrder::bigEndianInt(raw + xing + 4);
    auto position = xing + 8;
    if ((flags & 1U) == 0 || position + 4 > data.getSize())
    {
        expect(false, "MP3 gapless tag records its encoded frame count.");
        return;
    }
    const auto frames = juce::ByteOrder::bigEndianInt(raw + position);
    position += 4;
    if ((flags & 2U) != 0) position += 4;
    if ((flags & 4U) != 0) position += 100;
    if ((flags & 8U) != 0) position += 4;
    if (position + 24 > data.getSize())
    {
        expect(false, "MP3 contains the complete LAME delay/padding extension.");
        return;
    }
    expect(std::memcmp(raw + position, "LAME", 4) == 0, "LAME tag is finalized, not an empty placeholder.");
    const auto delay = (static_cast<int>(raw[position + 21]) << 4) | (raw[position + 22] >> 4);
    const auto padding = ((raw[position + 22] & 0xf) << 8) | raw[position + 23];
    expect(delay > 0 && static_cast<juce::int64>(frames) * 1152 - delay - padding == originalFrames,
           "MP3 frame count, delay, and padding recover the exact original duration.");
}

void expectLosslessAudio(juce::AudioFormatReader& reader,
                         const juce::AudioBuffer<float>& expected,
                         float tolerance)
{
    expect(reader.lengthInSamples == expected.getNumSamples(),
           "Lossless export preserves exact frame count.");
    juce::AudioBuffer<float> decoded(expected.getNumChannels(), expected.getNumSamples());
    expect(reader.read(&decoded, 0, decoded.getNumSamples(), 0, true,
                       decoded.getNumChannels() > 1),
           "Decode the complete lossless file.");
    for (int channel = 0; channel < expected.getNumChannels(); ++channel)
        for (int sample = 0; sample < expected.getNumSamples(); ++sample)
            if (std::abs(decoded.getSample(channel, sample)
                         - expected.getSample(channel, sample)) > tolerance)
            {
                expect(false, "Lossless samples match the source within one quantization step.");
                return;
            }
}

void defaultSettingsAreSupported()
{
    const AudioExportSettings settings;
    expect(AudioExport::validate(settings).wasOk(),
           "Default professional export settings are supported.");
    expect(AudioExport::formatName(settings.format) == "WAV",
           "WAV has a human-readable name.");
    expect(AudioExport::extension(settings.format) == "wav",
           "WAV has an extension without a leading dot.");
    expect(AudioExport::formatName(AudioExportFormat::aiff) == "AIFF"
               && AudioExport::formatName(AudioExportFormat::flac) == "FLAC"
               && AudioExport::formatName(AudioExportFormat::oggVorbis) == "Ogg Vorbis"
               && AudioExport::formatName(AudioExportFormat::mp3) == "MP3",
           "All five formats have explicit display names.");
    expect(AudioExport::extension(AudioExportFormat::aiff) == "aiff"
               && AudioExport::extension(AudioExportFormat::flac) == "flac"
               && AudioExport::extension(AudioExportFormat::oggVorbis) == "ogg"
               && AudioExport::extension(AudioExportFormat::mp3) == "mp3",
           "All five formats have explicit extensions.");
    const auto serialized = settings.toVar();
    expect(serialized["format"].toString() == "wav"
               && static_cast<double>(serialized["sampleRate"]) == 48000.0
               && static_cast<int>(serialized["bitDepth"]) == 24
               && static_cast<int>(serialized["channels"]) == 2
               && serialized["dither"].toString() == "none"
               && serialized["mp3BitrateMode"].toString() == "constant",
           "Export settings serialize without losing their meaning.");
    expect(AudioExport::description(settings).contains("24")
               && AudioExport::description(settings).contains("48"),
           "The export description includes precision and sample rate.");
}

void writesFiveActualFormats()
{
    ExportDirectory directory;
    const auto audio = signal(12000, true);
    constexpr std::array formats {
        AudioExportFormat::wav, AudioExportFormat::aiff, AudioExportFormat::flac,
        AudioExportFormat::oggVorbis, AudioExportFormat::mp3
    };
    for (const auto format : formats)
    {
        AudioExportSettings settings;
        settings.format = format;
        settings.bitDepth = format == AudioExportFormat::mp3
                || format == AudioExportFormat::oggVorbis ? 32 : 24;
        const auto file = directory.file.getChildFile("audio." + AudioExport::extension(format));
        if (!encode(audio, file, settings))
            continue;
        const auto data = bytes(file);
        const auto* raw = static_cast<const unsigned char*>(data.getData());
        const auto hasMagic = [&](const char* magic)
        {
            return data.getSize() >= 12 && std::memcmp(raw, magic, 4) == 0;
        };
        switch (format)
        {
            case AudioExportFormat::wav:
                expect(hasMagic("RIFF") && std::memcmp(raw + 8, "WAVE", 4) == 0,
                       "WAV exports have RIFF/WAVE headers.");
                break;
            case AudioExportFormat::aiff:
                expect(hasMagic("FORM") && std::memcmp(raw + 8, "AIFF", 4) == 0,
                       "AIFF exports have FORM/AIFF headers.");
                break;
            case AudioExportFormat::flac:
                expect(hasMagic("fLaC"), "FLAC exports have a real FLAC header.");
                break;
            case AudioExportFormat::oggVorbis:
                expect(hasMagic("OggS") && contains(data, "vorbis"),
                       "Ogg exports contain a Vorbis stream.");
                break;
            case AudioExportFormat::mp3:
                expect(contains(data, "Info") && contains(data, "LAME"),
                       "CBR MP3 contains the finalized LAME gapless/Info frame.");
                break;
        }
        auto reader = readerFor(file, format);
        expect(reader != nullptr, "Each codec's actual decoder recognizes the output.");
        if (reader == nullptr)
            continue;
        expect(std::abs(reader->sampleRate - settings.sampleRate) < 0.001 && reader->numChannels == 2,
               "All codecs preserve the requested rate and channels.");
        juce::AudioBuffer<float> decoded(2, static_cast<int>(reader->lengthInSamples));
        expect(reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true)
                   && decoded.getMagnitude(0, decoded.getNumSamples()) > 0.01f,
               "Each encoded file decodes to non-silent audio, not just a valid header.");
    }
}

void losslessPrecisionAndRates()
{
    ExportDirectory directory;
    const auto audio = signal(512);
    for (const auto format : { AudioExportFormat::wav, AudioExportFormat::aiff,
                               AudioExportFormat::flac })
        for (const auto rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
            for (const auto depth : AudioExport::bitDepths(format))
            {
                AudioExportSettings settings;
                settings.format = format;
                settings.sampleRate = rate;
                settings.bitDepth = depth;
                const auto file = directory.file.getChildFile("precision."
                                                               + AudioExport::extension(format));
                if (!encode(audio, file, settings))
                    continue;
                auto reader = readerFor(file, format);
                expect(reader != nullptr, "Decode each supported lossless precision/rate combination.");
                if (reader == nullptr)
                    continue;
                expect(std::abs(reader->sampleRate - rate) < 0.001
                           && reader->bitsPerSample == static_cast<unsigned int>(depth),
                       "Lossless headers report exact sample rate and bit depth.");
                expect(reader->usesFloatingPointData
                           == (format == AudioExportFormat::wav && depth == 32),
                       "32-bit WAV is IEEE float; AIFF and FLAC are integer PCM.");
                expectLosslessAudio(*reader, audio, depth == 32 ? 0.0f
                    : std::ldexp(1.0f, 1 - depth));
            }
}

void channelConversionAndPeakNormalization()
{
    ExportDirectory directory;
    juce::AudioBuffer<float> source(2, 16);
    for (int index = 0; index < source.getNumSamples(); ++index)
    {
        source.setSample(0, index, index % 2 == 0 ? 0.25f : -0.5f);
        source.setSample(1, index, index % 2 == 0 ? 0.75f : 0.25f);
    }
    AudioExportSettings settings;
    settings.channels = 1;
    settings.bitDepth = 32;
    auto file = directory.file.getChildFile("mono.wav");
    if (encode(source, file, settings))
    {
        auto reader = readerFor(file, settings.format);
        expect(reader != nullptr && reader->numChannels == 1, "Mono output has one channel.");
        juce::AudioBuffer<float> expected(1, source.getNumSamples());
        for (int index = 0; index < expected.getNumSamples(); ++index)
            expected.setSample(0, index, index % 2 == 0 ? 0.5f : -0.125f);
        if (reader != nullptr)
            expectLosslessAudio(*reader, expected, 0.0f);
    }
    settings.normalizePeak = true;
    settings.normalizePeakDbfs = -6.0;
    file = directory.file.getChildFile("normalized.wav");
    if (encode(source, file, settings))
    {
        auto reader = readerFor(file, settings.format);
        expect(reader != nullptr, "Decode peak-normalized output.");
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> decoded(1, source.getNumSamples());
            reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, false);
            expect(std::abs(decoded.getMagnitude(0, decoded.getNumSamples())
                            - std::pow(10.0, -6.0 / 20.0)) < 0.000001,
                   "Peak normalization measures the downmix, not the original channels.");
            expect(std::abs(decoded.getSample(0, 1) / decoded.getSample(0, 0) + 0.25f) < 0.000001f,
                   "Normalization uses uniform gain and preserves sample ratios.");
        }
    }
    expect(source.getSample(0, 0) == 0.25f && source.getSample(1, 0) == 0.75f,
           "Export never modifies the caller's audio buffer.");
    juce::AudioBuffer<float> mono(1, 16);
    mono.clear();
    settings.channels = 2;
    file = directory.file.getChildFile("silent.wav");
    if (encode(mono, file, settings))
    {
        auto reader = readerFor(file, settings.format);
        juce::AudioBuffer<float> expected(2, mono.getNumSamples());
        expected.clear();
        expect(reader != nullptr, "Silent audio remains a decodable stereo file.");
        if (reader != nullptr)
            expectLosslessAudio(*reader, expected, 0.0f);
    }
    mono.applyGain(0.0f);
    mono.setSample(0, 0, 1.5f);
    mono.setSample(0, 1, -1.25f);
    settings.normalizePeak = false;
    file = directory.file.getChildFile("float-headroom.wav");
    if (encode(mono, file, settings))
    {
        auto reader = readerFor(file, settings.format);
        juce::AudioBuffer<float> expected(2, mono.getNumSamples());
        expected.copyFrom(0, 0, mono, 0, 0, mono.getNumSamples());
        expected.copyFrom(1, 0, mono, 0, 0, mono.getNumSamples());
        if (reader != nullptr)
            expectLosslessAudio(*reader, expected, 0.0f);
        else
            expect(false, "Float WAV preserves headroom and mono-to-stereo duplication.");
    }
}

void deterministicIntegerDither()
{
    ExportDirectory directory;
    juce::AudioBuffer<float> source(2, 8192);
    source.clear();
    for (const auto depth : { 16, 24 })
    {
        AudioExportSettings settings;
        settings.bitDepth = depth;
        settings.dither = AudioExportDither::tpdf;
        const auto first = directory.file.getChildFile("dither-a.wav");
        const auto second = directory.file.getChildFile("dither-b.wav");
        if (!encode(source, first, settings) || !encode(source, second, settings))
            continue;
        expect(bytes(first) == bytes(second), "TPDF dither is reproducible byte for byte.");
        auto reader = readerFor(first, settings.format);
        expect(reader != nullptr, "Decode dithered PCM.");
        if (reader == nullptr)
            continue;
        juce::AudioBuffer<float> decoded(2, source.getNumSamples());
        reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true);
        const auto lsb = std::ldexp(1.0f, 1 - depth);
        const auto magnitude = decoded.getMagnitude(0, decoded.getNumSamples());
        expect(magnitude > 0.0f && magnitude <= lsb,
               "Integer dither produces bounded one-LSB quantized noise.");
        double sum = 0.0;
        bool independent = false;
        for (int sample = 0; sample < decoded.getNumSamples(); ++sample)
        {
            sum += decoded.getSample(0, sample);
            independent = independent
                || std::abs(decoded.getSample(0, sample) - decoded.getSample(1, sample)) >= lsb;
        }
        expect(std::abs(sum / decoded.getNumSamples()) < lsb * 0.05,
               "TPDF quantization does not introduce a DC bias.");
        expect(independent, "Dither noise is decorrelated between channels.");
    }
}

void codecQualityControlsAreEffective()
{
    ExportDirectory directory;
    const auto audio = signal(24000, true);
    AudioExportSettings settings;
    for (const auto format : { AudioExportFormat::mp3, AudioExportFormat::oggVorbis,
                               AudioExportFormat::flac })
    {
        settings.format = format;
        settings.bitDepth = format == AudioExportFormat::flac ? 24 : 32;
        settings.mp3BitrateKbps = 64;
        settings.oggQuality = 0;
        settings.flacCompressionLevel = 1;
        const auto low = directory.file.getChildFile("low." + AudioExport::extension(format));
        const auto high = directory.file.getChildFile("high." + AudioExport::extension(format));
        if (!encode(audio, low, settings))
            continue;
        settings.mp3BitrateKbps = 320;
        settings.oggQuality = 10;
        settings.flacCompressionLevel = 8;
        if (!encode(audio, high, settings))
            continue;
        expect(bytes(low) != bytes(high), "Codec quality options affect the actual encoded stream.");
        if (format == AudioExportFormat::mp3)
            expect(high.getSize() > low.getSize() * 3,
                   "320 kbps CBR produces observably more data than 64 kbps.");
        else if (format == AudioExportFormat::oggVorbis)
            expect(high.getSize() > low.getSize(),
                   "Ogg quality 10 produces more data than quality 0.");
        else
        {
            expect(high.getSize() < low.getSize(),
                   "FLAC level 8 compresses this correlated fixture more than level 1.");
            for (const auto& file : { low, high })
                if (auto reader = readerFor(file, format))
                    expectLosslessAudio(*reader, audio, std::ldexp(1.0f, -23));
                else
                    expect(false, "Both FLAC compression levels decode.");
        }
    }
}

void mp3ModesRatesAndBitrates()
{
    ExportDirectory directory;
    const auto shortAudio = signal(1152, true);
    AudioExportSettings settings;
    settings.format = AudioExportFormat::mp3;
    settings.bitDepth = 32;
    for (const auto rate : { 32000.0, 44100.0, 48000.0 })
        for (const auto bitrate : { 64, 96, 128, 160, 192, 224, 256, 320 })
        {
            settings.sampleRate = rate;
            settings.mp3BitrateKbps = bitrate;
            settings.channels = bitrate % 3 == 0 ? 1 : 2;
            const auto file = directory.file.getChildFile("cbr.mp3");
            if (!encode(shortAudio, file, settings))
                continue;
            expectMp3GaplessTag(file, shortAudio.getNumSamples());
            auto reader = readerFor(file, settings.format);
            expect(reader != nullptr && std::abs(reader->sampleRate - rate) < 0.001
                       && reader->numChannels == static_cast<unsigned int>(settings.channels),
                   "Every offered MP3 CBR rate/bitrate pair actually encodes without substitution.");
        }
    settings.mp3BitrateMode = Mp3BitrateMode::variable;
    for (const auto rate : { 32000.0, 44100.0, 48000.0 })
        for (int quality = 0; quality <= 9; ++quality)
        {
            settings.sampleRate = rate;
            settings.mp3VbrQuality = quality;
            const auto file = directory.file.getChildFile("vbr-options.mp3");
            if (!encode(shortAudio, file, settings))
                continue;
            expectMp3GaplessTag(file, shortAudio.getNumSamples());
            auto reader = readerFor(file, settings.format);
            expect(reader != nullptr && std::abs(reader->sampleRate - rate) < 0.001,
                   "All VBR qualities work at every offered MP3 sample rate.");
        }
    const auto audio = signal(12000, true);
    settings.mp3BitrateMode = Mp3BitrateMode::variable;
    settings.channels = 2;
    settings.mp3VbrQuality = 0;
    const auto best = directory.file.getChildFile("vbr-best.mp3");
    const auto smallest = directory.file.getChildFile("vbr-small.mp3");
    if (!encode(audio, best, settings))
        return;
    settings.mp3VbrQuality = 9;
    if (!encode(audio, smallest, settings))
        return;
    expect(best.getSize() > smallest.getSize(), "VBR quality 0 and 9 are honored.");
    for (const auto& file : { best, smallest })
    {
        expect(contains(bytes(file), "Xing") && contains(bytes(file), "LAME"),
               "VBR exports have a finalized Xing/LAME seek and gapless tag.");
        expectMp3GaplessTag(file, audio.getNumSamples());
        auto reader = readerFor(file, settings.format);
        expect(reader != nullptr && reader->lengthInSamples > 0,
               "VBR exports decode using the built-in MP3 decoder.");
    }
}

void metadataUsesCodecKeys()
{
    ExportDirectory directory;
    const auto audio = signal();
    juce::StringPairArray metadata;
    metadata.set("title", "Synthetic export");
    metadata.set("artist", "Studio Duo");
    metadata.set("album", "Encoder tests");
    metadata.set("comment", "Only generated audio");
    metadata.set("date", "2026-09-19");
    metadata.set("genre", "Metal");
    metadata.set("copyright", "Synthetic fixture");
    metadata.set("trackNumber", "2");
    for (const auto format : { AudioExportFormat::wav, AudioExportFormat::aiff,
                               AudioExportFormat::flac, AudioExportFormat::oggVorbis,
                               AudioExportFormat::mp3 })
    {
        AudioExportSettings settings;
        settings.format = format;
        settings.bitDepth = format == AudioExportFormat::mp3
                || format == AudioExportFormat::oggVorbis ? 32 : 24;
        const auto file = directory.file.getChildFile("tagged." + AudioExport::extension(format));
        if (!encode(audio, file, settings, metadata))
            continue;
        const auto data = bytes(file);
        auto reader = readerFor(file, format);
        expect(reader != nullptr, "Metadata does not invalidate the audio container.");
        if (reader != nullptr && format != AudioExportFormat::oggVorbis
            && format != AudioExportFormat::mp3)
            expectLosslessAudio(*reader, audio, std::ldexp(1.0f, -23));
        if (format == AudioExportFormat::mp3)
            expectMp3GaplessTag(file, audio.getNumSamples());
        if (format == AudioExportFormat::wav && reader != nullptr)
            expect(reader->metadataValues[juce::WavAudioFormat::riffInfoTitle] == metadata["title"]
                       && reader->metadataValues[juce::WavAudioFormat::riffInfoArtist] == metadata["artist"],
                   "WAV maps generic title/artist into RIFF INFO keys.");
        else if (format == AudioExportFormat::oggVorbis && reader != nullptr)
            expect(reader->metadataValues[juce::OggVorbisAudioFormat::id3title] == metadata["title"]
                       && reader->metadataValues[juce::OggVorbisAudioFormat::id3artist] == metadata["artist"],
                   "Ogg maps generic title/artist into Vorbis comments.");
        else if (format == AudioExportFormat::flac)
            expect(contains(data, "TITLE=Synthetic export") && contains(data, "ARTIST=Studio Duo"),
                   "FLAC contains a standards-compliant Vorbis comment block.");
        else if (format == AudioExportFormat::aiff)
            expect(contains(data, "NAME") && contains(data, "AUTH")
                       && contains(data, "Synthetic export") && contains(data, "TALB"),
                   "AIFF carries native text chunks and an ID3 album tag.");
        else if (format == AudioExportFormat::mp3)
            expect(contains(data, "ID3") && contains(data, "TIT2") && contains(data, "TPE1")
                       && contains(data, "Info"),
                   "MP3 metadata precedes a correctly positioned finalized Info frame.");
    }
}

void independentMp3ExportsCanRunConcurrently()
{
    ExportDirectory directory;
    const auto audio = signal(8192, true);
    std::vector<std::future<juce::Result>> exports;
    for (int index = 0; index < 3; ++index)
        exports.push_back(std::async(std::launch::async, [&, index]
        {
            AudioExportSettings settings;
            settings.format = AudioExportFormat::mp3;
            settings.bitDepth = 32;
            return AudioExport::write(audio, directory.file.getChildFile(
                "parallel-" + juce::String(index) + ".mp3"), settings);
        }));
    for (auto& pending : exports)
        expect(pending.get().wasOk(), "Independent callers may safely share the MP3 export helper.");
    for (int index = 0; index < 3; ++index)
    {
        const auto file = directory.file.getChildFile("parallel-" + juce::String(index) + ".mp3");
        expect(readerFor(file, AudioExportFormat::mp3) != nullptr, "Every concurrent export is decodable.");
        expectMp3GaplessTag(file, audio.getNumSamples());
    }
}

void rejectsInvalidSettingsAndKeepsDestination()
{
    const auto invalid = [](const AudioExportSettings& settings)
    {
        const auto result = AudioExport::validate(settings);
        expect(result.failed() && result.getErrorMessage().isNotEmpty(),
               "Invalid settings fail with a descriptive message instead of falling back.");
    };
    const AudioExportSettings defaults;
    for (const auto rate : { 0.0, -48000.0, 48000.5, 12345.0,
                             std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity() })
    {
        auto settings = defaults;
        settings.sampleRate = rate;
        invalid(settings);
    }
    for (const auto peak : { -24.01, 0.01, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity() })
    {
        auto settings = defaults;
        settings.normalizePeakDbfs = peak;
        invalid(settings);
    }
    for (const auto channels : { 0, -1, 3 })
    {
        auto settings = defaults;
        settings.channels = channels;
        invalid(settings);
    }
    auto settings = defaults;
    settings.format = static_cast<AudioExportFormat>(99);
    invalid(settings);
    settings = defaults;
    settings.dither = static_cast<AudioExportDither>(99);
    invalid(settings);
    settings = defaults;
    settings.mp3BitrateMode = static_cast<Mp3BitrateMode>(99);
    invalid(settings);
    for (const auto bitrate : { 0, -1, 65, 500 })
    {
        settings = defaults;
        settings.mp3BitrateKbps = bitrate;
        invalid(settings);
    }
    for (const auto quality : { -1, 10 })
    {
        settings = defaults;
        settings.mp3VbrQuality = quality;
        invalid(settings);
    }
    for (const auto quality : { -1, 11 })
    {
        settings = defaults;
        settings.oggQuality = quality;
        invalid(settings);
    }
    for (const auto level : { -1, 0, 9 })
    {
        settings = defaults;
        settings.flacCompressionLevel = level;
        invalid(settings);
    }
    for (const auto format : { AudioExportFormat::aiff, AudioExportFormat::flac })
    {
        settings = defaults;
        settings.format = format;
        settings.bitDepth = 32;
        invalid(settings);
    }
    for (const auto format : { AudioExportFormat::wav, AudioExportFormat::oggVorbis,
                               AudioExportFormat::mp3 })
    {
        settings = defaults;
        settings.format = format;
        settings.bitDepth = 32;
        settings.dither = AudioExportDither::tpdf;
        invalid(settings);
    }
    settings = defaults;
    settings.format = AudioExportFormat::mp3;
    invalid(settings);
    settings.bitDepth = 32;
    settings.sampleRate = 96000.0;
    invalid(settings);

    ExportDirectory directory;
    const auto destination = directory.file.getChildFile("preserved.flac");
    expect(destination.replaceWithText("existing destination"), "Create the preservation sentinel.");
    const auto before = bytes(destination);
    expect(AudioExport::write(signal(), destination, settings).failed(),
           "Invalid export cannot replace a destination.");
    settings = defaults;
    auto corrupt = signal();
    corrupt.setSample(1, 20, std::numeric_limits<float>::quiet_NaN());
    expect(AudioExport::write(corrupt, destination, settings).failed(),
           "Non-finite input samples are rejected.");
    corrupt.setSample(1, 20, std::numeric_limits<float>::infinity());
    expect(AudioExport::write(corrupt, destination, settings).failed(),
           "Infinite input samples are rejected.");
    juce::AudioBuffer<float> empty;
    expect(AudioExport::write(empty, destination, settings).failed(),
           "Empty input is rejected before encoding.");
    juce::AudioBuffer<float> surround(3, 16);
    surround.clear();
    expect(AudioExport::write(surround, destination, settings).failed(),
           "Unsupported input channels are not silently discarded.");
    settings.format = AudioExportFormat::flac;
    juce::StringPairArray badMetadata;
    badMetadata.set("invalid=key", "bad");
    expect(AudioExport::write(signal(), destination, settings, badMetadata).failed(),
           "A late metadata failure does not commit the staged audio.");
    expect(bytes(destination) == before, "Every failed export leaves existing destination bytes intact.");
    expect(directory.file.findChildFiles(juce::File::findFiles, false).size() == 1,
           "Failed exports remove every sibling staging file after closing their handles.");
    const auto targetDirectory = directory.file.getChildFile("not-a-file.wav");
    targetDirectory.createDirectory();
    expect(AudioExport::write(signal(), targetDirectory, defaults).failed(),
           "A directory cannot be replaced by an audio file.");
    const auto impossibleParent = destination.getChildFile("child.wav");
    expect(AudioExport::write(signal(), impossibleParent, defaults).failed(),
           "Filesystem creation errors propagate without changing the existing parent file.");
    expect(bytes(destination) == before, "Filesystem failures preserve the sentinel as well.");
    expect(encode(signal(), destination, defaults), "A successful export replaces the sentinel.");
    expect(bytes(destination) != before, "Replacement happens only after a complete successful export.");
    expect(directory.file.findChildFiles(juce::File::findFiles, false).size() == 1,
           "Successful exports also clean up their staging files.");
}
}

void audioExportTests()
{
    defaultSettingsAreSupported();
    writesFiveActualFormats();
    losslessPrecisionAndRates();
    channelConversionAndPeakNormalization();
    deterministicIntegerDither();
    codecQualityControlsAreEffective();
    mp3ModesRatesAndBitrates();
    metadataUsesCodecKeys();
    independentMp3ExportsCanRunConcurrently();
    rejectsInvalidSettingsAndKeepsDestination();
}

#if defined(STUDIO_DUO_AUDIO_EXPORT_TEST_MAIN)
int main()
{
    audioExportTests();
    if (failures == 0)
        std::cout << "All audio export tests passed.\n";
    return failures == 0 ? 0 : 1;
}
#endif

#include "AudioExport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <lame.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace studio
{
namespace
{
constexpr std::array mp3Bitrates { 64, 96, 128, 160, 192, 224, 256, 320 };
constexpr int encodeBlockSize = 8192;

std::unique_ptr<juce::AudioFormat> nativeFormat(AudioExportFormat format)
{
    switch (format)
    {
        case AudioExportFormat::wav: return std::make_unique<juce::WavAudioFormat>();
        case AudioExportFormat::aiff: return std::make_unique<juce::AiffAudioFormat>();
        case AudioExportFormat::flac: return std::make_unique<juce::FlacAudioFormat>();
        case AudioExportFormat::oggVorbis: return std::make_unique<juce::OggVorbisAudioFormat>();
        case AudioExportFormat::mp3: break;
    }
    return {};
}

bool isLossy(AudioExportFormat format)
{
    return format == AudioExportFormat::mp3 || format == AudioExportFormat::oggVorbis;
}

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double uniformUnit(std::uint64_t value) noexcept
{
    return static_cast<double>(splitMix64(value) >> 11U)
        * (1.0 / 9007199254740992.0);
}

void applyDither(juce::AudioBuffer<float>& audio, int bitDepth)
{
    constexpr std::uint64_t seed = 0x53545544494f4455ULL;
    const auto lsb = std::ldexp(1.0, 1 - bitDepth);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto counter = seed
                ^ (static_cast<std::uint64_t>(channel + 1) * 0xd6e8feb86659fd93ULL)
                ^ (static_cast<std::uint64_t>(sample + 1) * 0xa0761d6478bd642fULL);
            const auto noise = (uniformUnit(counter)
                               - uniformUnit(counter ^ 0xe7037ed1a0b428dbULL)) * lsb;
            // Quantize here: JUCE's integer writers truncate lower bits, which
            // otherwise adds a half-LSB DC bias to the triangular noise.
            const auto quantized = std::round(
                (static_cast<double>(audio.getSample(channel, sample)) + noise) / lsb) * lsb;
            audio.setSample(channel, sample,
                            static_cast<float>(juce::jlimit(-1.0, 1.0 - lsb, quantized)));
        }
}

struct OutputState
{
    juce::FileOutputStream& file;
    bool failed = false;
};

// The writer owns this adapter, not the file. Its destructor can finalize
// headers/codec trailers before we inspect sticky I/O errors and close the file.
class CheckedOutputStream final : public juce::OutputStream
{
public:
    explicit CheckedOutputStream(OutputState& stateToUse) : state(stateToUse) {}

    bool write(const void* data, size_t size) override
    {
        const auto ok = state.file.write(data, size);
        state.failed = state.failed || !ok;
        return ok;
    }

    bool setPosition(juce::int64 position) override
    {
        const auto ok = state.file.setPosition(position);
        state.failed = state.failed || !ok;
        return ok;
    }

    juce::int64 getPosition() override { return state.file.getPosition(); }

    void flush() override
    {
        state.file.flush();
        state.failed = state.failed || state.file.getStatus().failed();
    }

private:
    OutputState& state;
};

juce::Result flushFile(juce::FileOutputStream& file)
{
    file.flush();
    return file.getStatus().failed()
        ? juce::Result::fail("Could not flush the encoded audio: "
                             + file.getStatus().getErrorMessage())
        : juce::Result::ok();
}

juce::StringPairArray nativeMetadata(AudioExportFormat format,
                                     const juce::StringPairArray& metadata)
{
    auto result = metadata;
    const auto map = [&](const char* source, const char* destination)
    {
        if (metadata[source].isNotEmpty())
            result.set(destination, metadata[source]);
    };
    if (format == AudioExportFormat::wav)
    {
        map("title", juce::WavAudioFormat::riffInfoTitle);
        map("artist", juce::WavAudioFormat::riffInfoArtist);
        map("album", juce::WavAudioFormat::riffInfoProductName);
        map("comment", juce::WavAudioFormat::riffInfoComments);
        map("date", juce::WavAudioFormat::riffInfoDateCreated);
        map("genre", juce::WavAudioFormat::riffInfoGenre);
        map("copyright", juce::WavAudioFormat::riffInfoCopyright);
        map("trackNumber", juce::WavAudioFormat::riffInfoTrackNumber);
    }
    else if (format == AudioExportFormat::oggVorbis)
    {
        map("title", juce::OggVorbisAudioFormat::id3title);
        map("artist", juce::OggVorbisAudioFormat::id3artist);
        map("album", juce::OggVorbisAudioFormat::id3album);
        map("comment", juce::OggVorbisAudioFormat::id3comment);
        map("date", juce::OggVorbisAudioFormat::id3date);
        map("genre", juce::OggVorbisAudioFormat::id3genre);
        map("trackNumber", juce::OggVorbisAudioFormat::id3trackNumber);
        result.set(juce::OggVorbisAudioFormat::encoderName, "Studio Duo / JUCE");
    }
    else if (format == AudioExportFormat::aiff)
    {
        result.remove("MetaDataSource");
    }
    return result;
}

juce::Result writeNative(const juce::AudioBuffer<float>& audio,
                         const juce::File& file,
                         const AudioExportSettings& settings,
                         const juce::StringPairArray& metadata)
{
    auto output = file.createOutputStream();
    if (output == nullptr || output->getStatus().failed())
        return juce::Result::fail("Could not open the staged audio file for writing.");
    OutputState state { *output };
    std::unique_ptr<juce::OutputStream> adapter = std::make_unique<CheckedOutputStream>(state);
    auto options = juce::AudioFormatWriterOptions {}
        .withSampleRate(settings.sampleRate)
        .withNumChannels(settings.channels)
        .withBitsPerSample(settings.bitDepth)
        .withSampleFormat(settings.format == AudioExportFormat::wav && settings.bitDepth == 32
            ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
            : juce::AudioFormatWriterOptions::SampleFormat::integral);
    const auto mapped = nativeMetadata(settings.format, metadata);
    for (int index = 0; index < mapped.size(); ++index)
        options = options.withMetadata(mapped.getAllKeys()[index], mapped.getAllValues()[index]);
    if (settings.format == AudioExportFormat::oggVorbis)
        options = options.withQualityOptionIndex(settings.oggQuality);
    else if (settings.format == AudioExportFormat::flac)
        options = options.withQualityOptionIndex(settings.flacCompressionLevel);
    auto codec = nativeFormat(settings.format);
    auto writer = codec->createWriterFor(adapter, options);
    if (writer == nullptr)
        return juce::Result::fail("Could not initialize the " + AudioExport::formatName(settings.format)
                                 + " encoder with the requested settings.");
    const auto written = writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    writer.reset();
    const auto flushed = flushFile(*output);
    if (!written || state.failed || flushed.failed())
        return juce::Result::fail("Could not finalize the " + AudioExport::formatName(settings.format)
                                 + " export. " + flushed.getErrorMessage());
    return juce::Result::ok();
}

struct LameDeleter
{
    void operator()(lame_global_flags* encoder) const { lame_close(encoder); }
};
using LameEncoder = std::unique_ptr<lame_global_flags, LameDeleter>;

std::vector<unsigned short> utf16TagText(const juce::String& text)
{
    std::vector<unsigned short> result { 0xfeff };
    const auto utf16 = text.toUTF16();
    for (const auto* unit = utf16.getAddress(); *unit != 0; ++unit)
        result.push_back(static_cast<unsigned short>(*unit));
    result.push_back(0);
    return result;
}

juce::Result setId3Metadata(lame_t encoder, const juce::StringPairArray& metadata)
{
    id3tag_init(encoder);
    id3tag_v2_only(encoder);
    const std::array<std::pair<const char*, const char*>, 6> textTags {{
        { "title", "TIT2" }, { "artist", "TPE1" }, { "album", "TALB" },
        { "genre", "TCON" }, { "copyright", "TCOP" }, { "trackNumber", "TRCK" }
    }};
    for (const auto& [key, frame] : textTags)
        if (metadata[key].isNotEmpty())
        {
            const auto text = utf16TagText(metadata[key]);
            if (id3tag_set_textinfo_utf16(encoder, frame, text.data()) != 0)
                return juce::Result::fail("Could not encode the ID3 metadata field: "
                                         + juce::String(key));
        }
    const auto date = metadata["date"];
    if (date.isNotEmpty())
    {
        const auto year = date.substring(0, 4);
        if (id3tag_set_textinfo_latin1(encoder, "TYER", year.toRawUTF8()) != 0)
            return juce::Result::fail("Could not encode the ID3 release year.");
        if (date.length() == 10 && date[4] == '-' && date[7] == '-')
        {
            const auto dayAndMonth = date.substring(8, 10) + date.substring(5, 7);
            if (id3tag_set_textinfo_latin1(encoder, "TDAT", dayAndMonth.toRawUTF8()) != 0)
                return juce::Result::fail("Could not encode the ID3 release date.");
        }
    }
    if (metadata["comment"].isNotEmpty())
    {
        const auto text = utf16TagText(metadata["comment"]);
        const std::array<unsigned short, 2> description { 0xfeff, 0 };
        if (id3tag_set_comment_utf16(encoder, "eng", description.data(), text.data()) != 0)
            return juce::Result::fail("Could not encode the ID3 comment.");
    }
    return juce::Result::ok();
}

juce::Result getId3Tag(lame_t encoder, std::vector<unsigned char>& tag)
{
    const auto size = lame_get_id3v2_tag(encoder, nullptr, 0);
    if (size > 16U * 1024U * 1024U)
        return juce::Result::fail("The ID3 metadata is too large.");
    tag.resize(size);
    if (size != 0 && lame_get_id3v2_tag(encoder, tag.data(), tag.size()) != size)
        return juce::Result::fail("Could not finalize the ID3 metadata.");
    return juce::Result::ok();
}

juce::Result writeMp3(const juce::AudioBuffer<float>& audio,
                      const juce::File& file,
                      const AudioExportSettings& settings,
                      const juce::StringPairArray& metadata)
{
    // LAME 3.100 rewrites shared quantization tables in every encoder's
    // iteration_init; initialization must not race another active encoder.
    static std::mutex encoderMutex;
    const std::lock_guard lock(encoderMutex);
    LameEncoder encoder(lame_init());
    if (encoder == nullptr)
        return juce::Result::fail("Could not allocate the built-in MP3 encoder.");
    const auto rate = static_cast<int>(settings.sampleRate);
    const auto variable = settings.mp3BitrateMode == Mp3BitrateMode::variable;
    if (lame_set_in_samplerate(encoder.get(), rate) != 0
        || lame_set_out_samplerate(encoder.get(), rate) != 0
        || lame_set_num_channels(encoder.get(), settings.channels) != 0
        || lame_set_num_samples(encoder.get(), static_cast<unsigned long>(audio.getNumSamples())) != 0
        || lame_set_mode(encoder.get(), settings.channels == 1 ? MONO : JOINT_STEREO) != 0
        || lame_set_quality(encoder.get(), 2) != 0
        || lame_set_bWriteVbrTag(encoder.get(), 1) != 0
        || lame_set_findReplayGain(encoder.get(), 0) != 0
        || lame_set_VBR(encoder.get(), variable ? vbr_mtrh : vbr_off) != 0
        || (variable ? lame_set_VBR_q(encoder.get(), settings.mp3VbrQuality)
                     : lame_set_brate(encoder.get(), settings.mp3BitrateKbps)) != 0)
        return juce::Result::fail("The MP3 encoder rejected the requested settings.");
    lame_set_write_id3tag_automatic(encoder.get(), 0);
    if (const auto result = setId3Metadata(encoder.get(), metadata); result.failed())
        return result;
    const auto initialized = lame_init_params(encoder.get());
    if (initialized < 0)
        return juce::Result::fail("Could not initialize MP3 encoding (LAME error "
                                 + juce::String(initialized) + ").");
    if (lame_get_out_samplerate(encoder.get()) != rate
        || lame_get_num_channels(encoder.get()) != settings.channels
        || (!variable && lame_get_brate(encoder.get()) != settings.mp3BitrateKbps)
        || (variable && lame_get_VBR_q(encoder.get()) != settings.mp3VbrQuality))
        return juce::Result::fail("The MP3 encoder could not honor the requested settings exactly.");
    std::vector<unsigned char> id3;
    if (const auto result = getId3Tag(encoder.get(), id3); result.failed())
        return result;
    auto output = file.createOutputStream();
    if (output == nullptr || output->getStatus().failed())
        return juce::Result::fail("Could not open the staged MP3 file.");
    if (!id3.empty() && !output->write(id3.data(), id3.size()))
        return juce::Result::fail("Could not write the MP3 metadata.");
    const auto tagPosition = output->getPosition();
    std::array<unsigned char, encodeBlockSize * 5 / 4 + 7200> encoded {};
    for (int offset = 0; offset < audio.getNumSamples();)
    {
        const auto count = std::min(encodeBlockSize, audio.getNumSamples() - offset);
        const auto size = lame_encode_buffer_ieee_float(
            encoder.get(), audio.getReadPointer(0, offset),
            audio.getReadPointer(settings.channels == 1 ? 0 : 1, offset),
            count, encoded.data(), static_cast<int>(encoded.size()));
        if (size < 0)
            return juce::Result::fail("MP3 encoding failed (LAME error " + juce::String(size) + ").");
        if (!output->write(encoded.data(), static_cast<size_t>(size)))
            return juce::Result::fail("Could not write the encoded MP3 audio.");
        offset += count;
    }
    const auto finalSize = lame_encode_flush(
        encoder.get(), encoded.data(), static_cast<int>(encoded.size()));
    if (finalSize < 0)
        return juce::Result::fail("Could not finalize MP3 encoding (LAME error "
                                 + juce::String(finalSize) + ").");
    if (!output->write(encoded.data(), static_cast<size_t>(finalSize)))
        return juce::Result::fail("Could not write the final MP3 audio frames.");
    const auto endPosition = output->getPosition();
    const auto tagSize = lame_get_lametag_frame(encoder.get(), encoded.data(), encoded.size());
    if (tagSize == 0 || tagSize > encoded.size()
        || !output->setPosition(tagPosition)
        || !output->write(encoded.data(), tagSize)
        || !output->setPosition(endPosition))
        return juce::Result::fail("Could not finalize the MP3 seek table and encoder delay/padding tag.");
    return flushFile(*output);
}

bool copyBytes(juce::FileInputStream& input, juce::FileOutputStream& output, juce::int64 remaining)
{
    std::array<char, 16384> buffer {};
    while (remaining > 0)
    {
        const auto count = static_cast<int>(std::min(remaining, static_cast<juce::int64>(buffer.size())));
        if (input.read(buffer.data(), count) != count || !output.write(buffer.data(), static_cast<size_t>(count)))
            return false;
        remaining -= count;
    }
    return input.getStatus().wasOk();
}

juce::Result addFlacMetadata(const juce::File& file, const juce::StringPairArray& metadata)
{
    if (metadata.size() == 0)
        return juce::Result::ok();
    juce::MemoryOutputStream comments;
    const juce::String vendor("Studio Duo");
    comments.writeInt(static_cast<int>(vendor.getNumBytesAsUTF8()));
    comments.write(vendor.toRawUTF8(), vendor.getNumBytesAsUTF8());
    comments.writeInt(metadata.size());
    for (int index = 0; index < metadata.size(); ++index)
    {
        const auto key = metadata.getAllKeys()[index].toUpperCase();
        if (key.isEmpty() || !std::all_of(key.begin(), key.end(), [](juce::juce_wchar character)
            { return character >= 0x20 && character <= 0x7d && character != '='; }))
            return juce::Result::fail("FLAC metadata keys must be printable ASCII without '='.");
        const auto entry = key + "=" + metadata.getAllValues()[index];
        comments.writeInt(static_cast<int>(entry.getNumBytesAsUTF8()));
        comments.write(entry.toRawUTF8(), entry.getNumBytesAsUTF8());
    }
    if (comments.getDataSize() > 0xffffffU)
        return juce::Result::fail("The FLAC metadata exceeds the format's 24-bit block size.");

    // JUCE does not forward FLAC metadata. Replace its vendor-only comment block
    // after finalization without touching the encoded audio or STREAMINFO checksum.
    juce::TemporaryFile tagged(file, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileInputStream input(file);
        juce::FileOutputStream output(tagged.getFile());
        std::array<char, 4> magic {};
        if (input.getStatus().failed() || output.getStatus().failed()
            || input.read(magic.data(), 4) != 4
            || std::memcmp(magic.data(), "fLaC", 4) != 0
            || !output.write(magic.data(), magic.size()))
            return juce::Result::fail("Could not open the staged FLAC file for metadata.");
        bool last = false;
        while (!last)
        {
            std::array<unsigned char, 4> header {};
            if (input.read(header.data(), 4) != 4)
                return juce::Result::fail("The encoded FLAC metadata header is incomplete.");
            last = (header[0] & 0x80) != 0;
            header[0] &= 0x7f;
            const auto length = (static_cast<int>(header[1]) << 16)
                | (static_cast<int>(header[2]) << 8) | header[3];
            if (length > input.getTotalLength() - input.getPosition())
                return juce::Result::fail("The encoded FLAC metadata block is incomplete.");
            if (header[0] == 4)
            {
                if (!input.setPosition(input.getPosition() + length))
                    return juce::Result::fail("Could not replace the FLAC comment block.");
            }
            else if (!output.write(header.data(), header.size()) || !copyBytes(input, output, length))
                return juce::Result::fail("Could not preserve the FLAC stream metadata.");
        }
        const auto size = comments.getDataSize();
        const std::array<unsigned char, 4> header {
            0x84, static_cast<unsigned char>(size >> 16),
            static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)
        };
        if (!output.write(header.data(), header.size())
            || !output.write(comments.getData(), size)
            || !copyBytes(input, output, input.getTotalLength() - input.getPosition()))
            return juce::Result::fail("Could not write the tagged FLAC stream.");
        if (const auto result = flushFile(output); result.failed())
            return result;
    }
    if (!tagged.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not finalize the staged FLAC metadata.");
    return juce::Result::ok();
}

juce::Result addAiffMetadata(const juce::File& file, const juce::StringPairArray& metadata)
{
    if (metadata.size() == 0)
        return juce::Result::ok();
    LameEncoder tagEncoder(lame_init());
    if (tagEncoder == nullptr)
        return juce::Result::fail("Could not allocate the AIFF metadata encoder.");
    if (const auto result = setId3Metadata(tagEncoder.get(), metadata); result.failed())
        return result;
    std::vector<unsigned char> id3;
    if (const auto result = getId3Tag(tagEncoder.get(), id3); result.failed())
        return result;
    juce::FileOutputStream output(file);
    if (output.getStatus().failed() || !output.setPosition(file.getSize()))
        return juce::Result::fail("Could not append AIFF metadata.");
    const auto writeChunk = [&](const char* name, const void* data, size_t size)
    {
        return output.write(name, 4)
            && output.writeIntBigEndian(static_cast<int>(size))
            && output.write(data, size)
            && ((size & 1U) == 0 || output.writeByte(0));
    };
    const std::array<std::pair<const char*, const char*>, 4> chunks {{
        { "title", "NAME" }, { "artist", "AUTH" },
        { "copyright", "(c) " }, { "comment", "ANNO" }
    }};
    for (const auto& [key, chunk] : chunks)
    {
        const auto text = metadata[key];
        if (text.isNotEmpty() && !writeChunk(chunk, text.toRawUTF8(), text.getNumBytesAsUTF8()))
            return juce::Result::fail("Could not write an AIFF text metadata chunk.");
    }
    if (!id3.empty() && !writeChunk("ID3 ", id3.data(), id3.size()))
        return juce::Result::fail("Could not write the AIFF ID3 metadata.");
    const auto length = output.getPosition();
    if (length < 8 || length - 8 > std::numeric_limits<int>::max()
        || !output.setPosition(4)
        || !output.writeIntBigEndian(static_cast<int>(length - 8)))
        return juce::Result::fail("Could not finalize the tagged AIFF container length.");
    return flushFile(output);
}
}

juce::var AudioExportSettings::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("format", AudioExport::extension(format));
    object->setProperty("sampleRate", sampleRate);
    object->setProperty("bitDepth", bitDepth);
    object->setProperty("channels", channels);
    object->setProperty("dither", dither == AudioExportDither::tpdf ? "tpdf" : "none");
    object->setProperty("mp3BitrateKbps", mp3BitrateKbps);
    object->setProperty("mp3BitrateMode", mp3BitrateMode == Mp3BitrateMode::variable ? "variable" : "constant");
    object->setProperty("mp3VbrQuality", mp3VbrQuality);
    object->setProperty("oggQuality", oggQuality);
    object->setProperty("flacCompressionLevel", flacCompressionLevel);
    object->setProperty("normalizePeak", normalizePeak);
    object->setProperty("normalizePeakDbfs", normalizePeakDbfs);
    return juce::var(object.release());
}

juce::String AudioExport::formatName(AudioExportFormat format)
{
    switch (format)
    {
        case AudioExportFormat::wav: return "WAV";
        case AudioExportFormat::aiff: return "AIFF";
        case AudioExportFormat::flac: return "FLAC";
        case AudioExportFormat::oggVorbis: return "Ogg Vorbis";
        case AudioExportFormat::mp3: return "MP3";
    }
    return {};
}

juce::String AudioExport::extension(AudioExportFormat format)
{
    switch (format)
    {
        case AudioExportFormat::wav: return "wav";
        case AudioExportFormat::aiff: return "aiff";
        case AudioExportFormat::flac: return "flac";
        case AudioExportFormat::oggVorbis: return "ogg";
        case AudioExportFormat::mp3: return "mp3";
    }
    return {};
}

std::vector<double> AudioExport::sampleRates(AudioExportFormat format)
{
    if (format == AudioExportFormat::mp3)
        return { 32000.0, 44100.0, 48000.0 };
    if (auto codec = nativeFormat(format))
    {
        const auto rates = codec->getPossibleSampleRates();
        return { rates.begin(), rates.end() };
    }
    return {};
}

std::vector<int> AudioExport::bitDepths(AudioExportFormat format)
{
    switch (format)
    {
        case AudioExportFormat::wav: return { 16, 24, 32 };
        case AudioExportFormat::aiff:
        case AudioExportFormat::flac: return { 16, 24 };
        case AudioExportFormat::oggVorbis:
        case AudioExportFormat::mp3: return { 32 };
    }
    return {};
}

juce::String AudioExport::description(const AudioExportSettings& settings)
{
    auto result = formatName(settings.format) + " / "
        + juce::String(settings.sampleRate / 1000.0, 3).trimCharactersAtEnd("0").trimCharactersAtEnd(".")
        + " kHz / " + (settings.channels == 1 ? "mono" : "stereo");
    if (settings.format == AudioExportFormat::mp3)
        result += settings.mp3BitrateMode == Mp3BitrateMode::constant
            ? " / " + juce::String(settings.mp3BitrateKbps) + " kbps CBR"
            : " / VBR quality " + juce::String(settings.mp3VbrQuality);
    else if (settings.format == AudioExportFormat::oggVorbis)
        result += " / quality " + juce::String(settings.oggQuality);
    else
    {
        result += " / " + juce::String(settings.bitDepth)
            + (settings.bitDepth == 32 ? "-bit float" : "-bit PCM");
        if (settings.format == AudioExportFormat::flac)
            result += " / compression " + juce::String(settings.flacCompressionLevel);
    }
    if (settings.dither == AudioExportDither::tpdf)
        result += " / TPDF dither";
    if (settings.normalizePeak)
        result += " / peak " + juce::String(settings.normalizePeakDbfs, 1) + " dBFS";
    return result;
}

juce::Result AudioExport::validate(const AudioExportSettings& settings)
{
    if (formatName(settings.format).isEmpty())
        return juce::Result::fail("Unknown audio export format.");
    const auto rates = sampleRates(settings.format);
    if (!std::isfinite(settings.sampleRate)
        || std::find(rates.begin(), rates.end(), settings.sampleRate) == rates.end())
        return juce::Result::fail("The selected sample rate is not supported by "
                                 + formatName(settings.format) + ".");
    const auto depths = bitDepths(settings.format);
    if (std::find(depths.begin(), depths.end(), settings.bitDepth) == depths.end())
        return juce::Result::fail("The selected bit depth is not supported by "
                                 + formatName(settings.format) + ".");
    if (settings.channels != 1 && settings.channels != 2)
        return juce::Result::fail("Audio exports must be mono or stereo.");
    if (settings.dither != AudioExportDither::none && settings.dither != AudioExportDither::tpdf)
        return juce::Result::fail("Unknown audio export dither mode.");
    if (settings.dither != AudioExportDither::none
        && (isLossy(settings.format) || settings.bitDepth == 32))
        return juce::Result::fail("TPDF dither is only supported for 16-bit or 24-bit integer PCM.");
    if (settings.mp3BitrateMode != Mp3BitrateMode::constant
        && settings.mp3BitrateMode != Mp3BitrateMode::variable)
        return juce::Result::fail("Unknown MP3 bitrate mode.");
    if (std::find(mp3Bitrates.begin(), mp3Bitrates.end(), settings.mp3BitrateKbps) == mp3Bitrates.end())
        return juce::Result::fail("MP3 bitrate must be 64, 96, 128, 160, 192, 224, 256, or 320 kbps.");
    if (settings.mp3VbrQuality < 0 || settings.mp3VbrQuality > 9)
        return juce::Result::fail("MP3 VBR quality must be between 0 (best) and 9 (smallest).");
    if (settings.oggQuality < 0 || settings.oggQuality > 10)
        return juce::Result::fail("Ogg Vorbis quality must be between 0 and 10.");
    // JUCE 9's index zero skips FLAC's setter and actually selects default 5.
    if (settings.flacCompressionLevel < 1 || settings.flacCompressionLevel > 8)
        return juce::Result::fail("FLAC compression must be between 1 and 8; JUCE cannot select level 0.");
    if (!std::isfinite(settings.normalizePeakDbfs)
        || settings.normalizePeakDbfs < -24.0 || settings.normalizePeakDbfs > 0.0)
        return juce::Result::fail("The normalization target must be finite and between -24 and 0 dBFS.");
    return juce::Result::ok();
}

juce::Result AudioExport::write(const juce::AudioBuffer<float>& audio,
                               const juce::File& destination,
                               const AudioExportSettings& settings,
                               const juce::StringPairArray& metadata)
{
    if (const auto result = validate(settings); result.failed())
        return result;
    if (audio.getNumSamples() <= 0 || (audio.getNumChannels() != 1 && audio.getNumChannels() != 2))
        return juce::Result::fail("Export requires a non-empty mono or stereo audio buffer.");
    if (destination == juce::File() || destination.isDirectory())
        return juce::Result::fail("Choose an audio file, not a directory, as the export destination.");
    if (settings.format == AudioExportFormat::aiff
        && static_cast<juce::int64>(audio.getNumSamples()) * settings.channels * (settings.bitDepth / 8)
            > std::numeric_limits<int>::max() - 16 * 1024 * 1024)
        return juce::Result::fail("This AIFF export exceeds the writer's 2 GiB container limit.");
    size_t metadataBytes = 0;
    for (int index = 0; index < metadata.size(); ++index)
    {
        metadataBytes += metadata.getAllKeys()[index].getNumBytesAsUTF8()
            + metadata.getAllValues()[index].getNumBytesAsUTF8() + 16;
        if (metadataBytes > 1024U * 1024U)
            return juce::Result::fail("Audio export metadata must be smaller than 1 MiB.");
    }
    try
    {
        juce::AudioBuffer<float> converted(settings.channels, audio.getNumSamples());
        double peak = 0.0;
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto left = static_cast<double>(audio.getSample(0, sample));
            const auto right = static_cast<double>(
                audio.getSample(audio.getNumChannels() == 1 ? 0 : 1, sample));
            if (!std::isfinite(left) || !std::isfinite(right))
                return juce::Result::fail("Audio export cannot encode NaN or infinite input samples.");
            if (settings.channels == 1)
            {
                const auto value = (left + right) * 0.5;
                converted.setSample(0, sample, static_cast<float>(value));
                peak = std::max(peak, std::abs(value));
            }
            else
            {
                converted.setSample(0, sample, static_cast<float>(left));
                converted.setSample(1, sample, static_cast<float>(right));
                peak = std::max({ peak, std::abs(left), std::abs(right) });
            }
        }
        const auto gain = settings.normalizePeak && peak > 0.0
            ? std::pow(10.0, settings.normalizePeakDbfs / 20.0) / peak : 1.0;
        const auto floatingPoint = settings.format == AudioExportFormat::wav && settings.bitDepth == 32;
        for (int channel = 0; channel < converted.getNumChannels(); ++channel)
            for (int sample = 0; sample < converted.getNumSamples(); ++sample)
            {
                auto value = static_cast<double>(converted.getSample(channel, sample)) * gain;
                if (!floatingPoint)
                    value = juce::jlimit(-1.0, 1.0, value);
                converted.setSample(channel, sample, static_cast<float>(value));
            }
        if (settings.dither == AudioExportDither::tpdf)
            applyDither(converted, settings.bitDepth);
        if (const auto result = destination.getParentDirectory().createDirectory(); result.failed())
            return juce::Result::fail("Could not create the export directory: " + result.getErrorMessage());
        juce::TemporaryFile staged(destination, juce::TemporaryFile::useHiddenFile);
        const auto encoded = settings.format == AudioExportFormat::mp3
            ? writeMp3(converted, staged.getFile(), settings, metadata)
            : writeNative(converted, staged.getFile(), settings, metadata);
        if (encoded.failed())
            return encoded;
        if (settings.format == AudioExportFormat::flac)
        {
            if (const auto result = addFlacMetadata(staged.getFile(), metadata); result.failed())
                return result;
        }
        else if (settings.format == AudioExportFormat::aiff)
        {
            if (const auto result = addAiffMetadata(staged.getFile(), metadata); result.failed())
                return result;
        }
        if (!staged.overwriteTargetFileWithTemporary())
            return juce::Result::fail("The audio encoded successfully, but could not replace "
                                     + destination.getFullPathName() + ".");
        return juce::Result::ok();
    }
    catch (const std::bad_alloc&)
    {
        return juce::Result::fail("There is not enough memory to prepare the audio export.");
    }
}
}

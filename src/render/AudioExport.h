#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <vector>

namespace studio
{
enum class AudioExportFormat { wav, aiff, flac, oggVorbis, mp3 };
enum class AudioExportDither { none, tpdf };
enum class Mp3BitrateMode { constant, variable };

struct AudioExportSettings
{
    AudioExportFormat format = AudioExportFormat::wav;
    double sampleRate = 48000.0;
    int bitDepth = 24; // WAV 32 is IEEE float; lossy codecs accept internal precision 32.
    int channels = 2;
    AudioExportDither dither = AudioExportDither::none;
    int mp3BitrateKbps = 320;
    Mp3BitrateMode mp3BitrateMode = Mp3BitrateMode::constant;
    int mp3VbrQuality = 2; // 0 (best) through 9 (smallest).
    int oggQuality = 6; // JUCE quality option indices 0 through 10.
    int flacCompressionLevel = 5; // 1 through 8; JUCE cannot select actual level 0.
    bool normalizePeak = false;
    double normalizePeakDbfs = -1.0;

    juce::var toVar() const;
};

class AudioExport
{
public:
    static juce::String formatName(AudioExportFormat);
    static juce::String extension(AudioExportFormat);
    static std::vector<double> sampleRates(AudioExportFormat);
    static std::vector<int> bitDepths(AudioExportFormat);
    static juce::String description(const AudioExportSettings&);
    static juce::Result validate(const AudioExportSettings&);
    // Input is already at settings.sampleRate; the input buffer is never modified.
    // Canonical metadata keys: title, artist, album, date, genre, copyright,
    // comment, trackNumber (JUCE's Ogg writer cannot store copyright).
    // JUCE-native WAV/AIFF/Ogg keys also pass through.
    static juce::Result write(
        const juce::AudioBuffer<float>& audio,
        const juce::File& destination,
        const AudioExportSettings& settings,
        const juce::StringPairArray& metadata = {});
};
}

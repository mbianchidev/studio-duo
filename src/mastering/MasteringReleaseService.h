#pragma once

#include "MasteringEngine.h"
#include "model/ProjectModel.h"
#include "render/AudioExport.h"

namespace studio
{
using MasteringExportFormat = AudioExportFormat;
using MasteringDither = AudioExportDither;

struct MasteringDistributionPreset
{
    juce::String id;
    juce::String name;
    std::optional<double> targetLoudnessLufs;
    std::optional<double> maximumTruePeakDbtp;
};

struct MasteringExportSettings : AudioExportSettings
{
    juce::String distributionPresetId;

    [[nodiscard]] juce::var toVar() const;
};

class MasteringReleaseService
{
public:
    static std::vector<MasteringDistributionPreset>
        distributionPresets();
    static std::optional<RenderReport> exportMaster(
        const MasteringAlbum& album,
        const juce::File& destination,
        const MasteringExportSettings& settings,
        const juce::File& signingDirectory,
        juce::String& error);
    static std::optional<RenderReport> exportDdp(
        const MasteringAlbum& album,
        const juce::File& destinationDirectory,
        const juce::File& encoderExecutable,
        const juce::File& signingDirectory,
        juce::String& error);
    static bool verifyReport(
        const RenderReport& report,
        const juce::File& trustedSigningDirectory,
        juce::String& error);
};
}

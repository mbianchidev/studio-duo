#pragma once

#include "StartupCoordinator.h"

#include <array>

namespace studio
{
struct StartupDeviceSummary
{
    bool audioEnabled = false;
    bool midiAvailable = false;
    bool retrospectiveCapture = false;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int audioInputs = 0;
    int audioOutputs = 0;
    int enabledMidiInputs = 0;
    int enabledMidiOutputs = 0;
    int unavailableMidiDevices = 0;
};

struct StartupPluginSummary
{
    int ready = 0;
    int blocked = 0;
    int bundled = 0;
    int failedValidations = 0;
    juce::int64 scanCrashes = 0;
    juce::int64 runtimeCrashes = 0;
    juce::int64 timeouts = 0;
    std::array<int, 8> failures {};
};

[[nodiscard]] juce::var startupDiagnosticReport(
    const StartupCoordinator& startup,
    const StartupDeviceSummary& devices,
    const StartupPluginSummary& plugins,
    bool safePlugins,
    bool defaultAudio,
    double firstPaintMs);
[[nodiscard]] juce::Result writeStartupDiagnostic(
    const juce::File& destination, const juce::var& report);

class StartupDiagnosticStore
{
public:
    explicit StartupDiagnosticStore(juce::File directory);
    juce::Result persist(const juce::var& report);
    [[nodiscard]] juce::File reportFile() const;

private:
    juce::File directory;
    juce::File currentReport;
};
}

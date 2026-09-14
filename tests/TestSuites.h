#pragma once

#include <juce_core/juce_core.h>
#include <optional>

std::optional<int> runAudioDeviceProbeFixture(const juce::StringArray& arguments);
void audioDeviceProbeTests();
#if JUCE_WINDOWS
std::optional<int> runWindowsCrashHandlerFixture(const juce::StringArray& arguments);
void windowsCrashHandlerTests();
#endif

void routingModelTests();
void routingEngineTests();
void routingUiModelTests();
void pluginFormatTests();
void pluginSearchPathTests();
void pluginRecoveryTests();
void automationTests();
void deviceTests();
void loggingTests();
void reampSnapshotTests();
void renderEngineTests();
void pluginCompatibilityTests();
void projectMigrationTests();
void windowSizingTests();
void transportTests();
void updateTests();

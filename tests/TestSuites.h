#pragma once

#include <juce_core/juce_core.h>
#include <optional>

std::optional<int> runAudioDeviceProbeFixture(const juce::StringArray& arguments);
std::optional<int> runProjectSaveFixture(const juce::StringArray& arguments);
std::optional<int> runProjectExchangeFixture(const juce::StringArray& arguments);
std::optional<int> runLongSessionQualification(const juce::StringArray& arguments);
std::optional<int> runVendorPluginQualification(const juce::StringArray& arguments);
std::optional<int> runMasteringMeasurement(const juce::StringArray& arguments);
std::optional<int> runMasteringDeliveryFixture(const juce::StringArray& arguments);
void projectHardeningTests();
void saveInterruptionTests();
void projectExchangeTests();
void keyboardAccessibilityTests();
void performanceHardeningTests();
void longSessionQualificationTests();
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
void pluginRackTests();
void pluginRackEngineTests();
void pluginRackUiTests();
void automationTests();
void deviceTests();
void bundledDeviceTests();
void loggingTests();
void reampSnapshotTests();
void renderEngineTests();
void audioExportTests();
void audioExportOptionsTests();
void pluginCompatibilityTests();
void projectMigrationTests();
void projectTemplateTests();
void windowSizingTests();
void transportTests();
void transportEditingTests();
void arrangementWorkflowTests();
void workspaceCommandTests();
void sectionTransportTests();
void transportSettingsTests();
void updateTests();
void midiTests();
void controlSurfaceTests();
void dawProjectTests();
void studioOneProjectTests();
void masteringTests();
void uiIconTests();
void timelineMarkerTests();
void mixerPanelTests();
void infoPanelTests();
void reentrancySafeTimerTests();
void startupTests();

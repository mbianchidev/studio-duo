#include "TestHarness.h"
#include "TestSuites.h"

#include "plugin_host/PluginCompatibilityValidator.h"
#include "plugin_host/ScreamForgeValidation.h"
#include "plugin_host/ClapPluginFormat.h"

void pluginCompatibilityTests()
{
    studio::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions,
                               STUDIO_DUO_CLAP_FIXTURE_PATH);
    expect(descriptions.size() == 1,
           "Compatibility fixture can be discovered.");
    if (descriptions.isEmpty())
        return;

    const auto report = studio::PluginCompatibilityValidator::validate(
        *descriptions[0]);
    expect(report.status == "pass"
               && std::all_of(
                   report.checks.cbegin(),
                   report.checks.cend(),
                   [](const auto& check)
                   {
                       return check.status == "pass"
                           || check.status == "skip";
                   }),
           "Public-standard compatibility validation passes the CLAP fixture.");
    juce::String error;
    const auto restored =
        studio::PluginValidationReport::fromVar(report.toVar(), error);
    expect(restored.has_value()
               && restored->pluginIdentifier == report.pluginIdentifier,
           "Compatibility reports are stable and serializable.");

    juce::PluginDescription screamForge;
    screamForge.name = "Scream Forge";
    screamForge.manufacturerName = "Studio Duo";
    screamForge.pluginFormatName = "VST3";
    expect(studio::ScreamForgeValidation::matches(screamForge),
           "Scream Forge selection uses public plugin metadata.");
    const auto unavailable =
        studio::ScreamForgeValidation::validateInstalled({});
    expect(unavailable.status == "not-installed",
           "Absent Scream Forge builds report not-installed explicitly.");
}

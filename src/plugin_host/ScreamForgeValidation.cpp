#include "ScreamForgeValidation.h"

namespace studio
{
juce::var ScreamForgeValidationResult::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("schemaVersion", 1);
    object->setProperty("target", "Scream Forge");
    object->setProperty("status", status);
    juce::Array<juce::var> reportValues;
    for (const auto& report : reports)
        reportValues.add(report.toVar());
    object->setProperty("reports", juce::var(reportValues));
    return juce::var(object.release());
}

bool ScreamForgeValidation::matches(
    const juce::PluginDescription& description)
{
    return description.name.containsIgnoreCase("Scream Forge")
        || (description.manufacturerName.containsIgnoreCase("Studio Duo")
            && description.category.containsIgnoreCase("Pitch"));
}

ScreamForgeValidationResult ScreamForgeValidation::validateInstalled(
    const std::vector<juce::PluginDescription>& descriptions)
{
    ScreamForgeValidationResult result;
    for (const auto& description : descriptions)
        if (matches(description)
            && (description.pluginFormatName == "VST3"
                || description.pluginFormatName == "AudioUnit"))
            result.reports.push_back(
                PluginCompatibilityValidator::validate(description));
    if (result.reports.empty())
    {
        result.status = "not-installed";
        return result;
    }
    result.status = std::all_of(
        result.reports.cbegin(),
        result.reports.cend(),
        [](const auto& report)
        {
            return report.status == "pass";
        })
        ? "pass"
        : "fail";
    return result;
}
}

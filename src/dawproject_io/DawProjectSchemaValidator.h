#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace studio
{
struct DawProjectValidationIssue
{
    juce::String objectPath;
    juce::String message;
};

struct DawProjectValidationResult
{
    bool valid = false;
    std::vector<DawProjectValidationIssue> issues;

    [[nodiscard]] juce::String summary() const;
};

class DawProjectSchemaValidator
{
public:
    static DawProjectValidationResult validateProjectXml(
        const juce::String& xml);
    static DawProjectValidationResult validateMetadataXml(
        const juce::String& xml);
    static juce::String projectSchemaSha256();
    static juce::String metadataSchemaSha256();
};
}

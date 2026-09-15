#pragma once

#include <juce_core/juce_core.h>

#include <map>

namespace studio
{
class DawProjectIdMapper
{
public:
    [[nodiscard]] juce::String externalId(
        const juce::String& kind,
        const juce::String& internalId);
    [[nodiscard]] juce::String importedId(
        const juce::String& kind,
        const juce::String& externalId,
        juce::String& error);
    bool bindImportedId(const juce::String& kind,
                        const juce::String& externalId,
                        const juce::String& internalId,
                        juce::String& error);
    [[nodiscard]] juce::String internalId(
        const juce::String& kind,
        const juce::String& externalId) const;

private:
    static juce::String key(const juce::String& kind,
                            const juce::String& id);
    static juce::String hash(const juce::String& kind,
                             const juce::String& id);
    static juce::String safeKind(const juce::String& kind);

    std::map<juce::String, juce::String> externalByInternal;
    std::map<juce::String, juce::String> internalByExternal;
};
}

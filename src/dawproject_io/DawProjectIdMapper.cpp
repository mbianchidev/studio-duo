#include "DawProjectIdMapper.h"

#include <juce_cryptography/juce_cryptography.h>

namespace studio
{
juce::String DawProjectIdMapper::externalId(
    const juce::String& kind,
    const juce::String& internalId)
{
    const auto mappingKey = key(kind, internalId);
    if (const auto existing = externalByInternal.find(mappingKey);
        existing != externalByInternal.cend())
    {
        return existing->second;
    }
    const auto value =
        "sd-" + safeKind(kind) + "-" + hash(kind, internalId).substring(0, 24);
    externalByInternal.emplace(mappingKey, value);
    return value;
}

juce::String DawProjectIdMapper::importedId(
    const juce::String& kind,
    const juce::String& externalId,
    juce::String& error)
{
    if (const auto existing = internalByExternal.find(key(kind, externalId));
        existing != internalByExternal.cend())
    {
        return existing->second;
    }
    const auto digest = hash(kind, externalId);
    const auto generated =
        digest.substring(0, 8) + "-"
        + digest.substring(8, 12) + "-"
        + digest.substring(12, 16) + "-"
        + digest.substring(16, 20) + "-"
        + digest.substring(20, 32);
    if (!bindImportedId(kind, externalId, generated, error))
        return {};
    return generated;
}

bool DawProjectIdMapper::bindImportedId(
    const juce::String& kind,
    const juce::String& externalId,
    const juce::String& internalId,
    juce::String& error)
{
    if (kind.trim().isEmpty()
        || externalId.trim().isEmpty()
        || internalId.trim().isEmpty())
    {
        error = "DAWproject ID mappings require a kind and non-empty IDs.";
        return false;
    }
    const auto mappingKey = key(kind, externalId);
    if (const auto existing = internalByExternal.find(mappingKey);
        existing != internalByExternal.cend())
    {
        if (existing->second == internalId)
            return true;
        error = "DAWproject ID '" + externalId
            + "' is already bound to another internal object.";
        return false;
    }
    internalByExternal.emplace(mappingKey, internalId);
    return true;
}

juce::String DawProjectIdMapper::internalId(
    const juce::String& kind,
    const juce::String& externalId) const
{
    if (const auto existing = internalByExternal.find(key(kind, externalId));
        existing != internalByExternal.cend())
    {
        return existing->second;
    }
    return {};
}

juce::String DawProjectIdMapper::key(const juce::String& kind,
                                     const juce::String& id)
{
    return kind.trim().toLowerCase() + "\x1f" + id;
}

juce::String DawProjectIdMapper::hash(const juce::String& kind,
                                      const juce::String& id)
{
    const auto value = kind.trim().toLowerCase() + "\n" + id;
    const auto utf8 = value.toStdString();
    return juce::SHA256(
               utf8.data(),
               utf8.size())
        .toHexString()
        .toLowerCase();
}

juce::String DawProjectIdMapper::safeKind(const juce::String& kind)
{
    auto result = kind.trim().toLowerCase().retainCharacters(
        "abcdefghijklmnopqrstuvwxyz0123456789-_");
    if (result.isEmpty())
        result = "object";
    if (!juce::CharacterFunctions::isLetter(result[0])
        && result[0] != '_')
    {
        result = "object-" + result;
    }
    return result;
}
}

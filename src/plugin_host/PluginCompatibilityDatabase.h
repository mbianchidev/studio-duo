#pragma once

#include "model/ProjectModel.h"

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

namespace studio
{
enum class PluginFailureKind
{
    none,
    scanCrash,
    runtimeCrash,
    timeout,
    protocolMismatch,
    unsupportedLayout,
    corruptState,
    missing
};

struct PluginCompatibilityRecord
{
    juce::String pluginIdentifier;
    juce::String name;
    juce::String format;
    juce::String vendor;
    juce::String version;
    juce::String architecture;
    PluginBridgeMode preferredMode = PluginBridgeMode::sandboxed;
    PluginFailureKind lastFailure = PluginFailureKind::none;
    juce::String lastMessage;
    juce::String updatedAt;
    int scanCrashCount = 0;
    int runtimeCrashCount = 0;
    int timeoutCount = 0;
    bool araCapable = false;
};

class PluginCompatibilityDatabase
{
public:
    explicit PluginCompatibilityDatabase(juce::File databaseFile);

    bool load(juce::String& error);
    bool save(juce::String& error) const;
    void noteFailure(const PluginCompatibilityRecord& identity,
                     PluginFailureKind failure,
                     juce::String message);
    void noteReady(const PluginCompatibilityRecord& identity,
                   PluginBridgeMode mode);
    [[nodiscard]] std::optional<PluginCompatibilityRecord> find(
        const juce::String& pluginIdentifier) const;
    [[nodiscard]] const std::vector<PluginCompatibilityRecord>& records() const;

private:
    PluginCompatibilityRecord& findOrAdd(
        const PluginCompatibilityRecord& identity);

    juce::File file;
    std::vector<PluginCompatibilityRecord> entries;
};

juce::String pluginFailureKindToString(PluginFailureKind value);
std::optional<PluginFailureKind> pluginFailureKindFromString(
    const juce::String& value);
}

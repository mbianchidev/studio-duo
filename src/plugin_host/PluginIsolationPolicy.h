#pragma once

#include "model/ProjectModel.h"

namespace studio
{
inline bool requiresReducedIsolationConsent(
    const PluginInsert& insert, PluginBridgeMode requestedMode) noexcept
{
    return !insert.bundledDevice && !insert.isContainer()
        && requestedMode != PluginBridgeMode::sandboxed
        && requestedMode != insert.bridgeMode;
}

inline juce::String reducedIsolationWarning(PluginBridgeMode mode)
{
    if (mode == PluginBridgeMode::sandboxed)
        return {};
    return (mode == PluginBridgeMode::araCompatibility
                ? juce::String("ARA 2 requires synchronous access to the project document. ")
                : juce::String("Trusted in-process mode bypasses the plugin sandbox. "))
        + "This third-party plugin will run inside Studio Duo. A plugin crash or hang can "
          "terminate or freeze the application; sandbox crash containment does not apply. "
          "A recovery point must be written before activation. Cancel to keep the current mode.";
}
}

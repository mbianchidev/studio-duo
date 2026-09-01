#include "PluginStateStore.h"

#include <juce_cryptography/juce_cryptography.h>

namespace studio
{
std::optional<PluginStateReference> PluginStateStore::store(
    const juce::File& package,
    const juce::MemoryBlock& state,
    juce::String& error)
{
    if (state.isEmpty())
    {
        error = "Plugin state cannot be empty.";
        return std::nullopt;
    }

    const auto hash = juce::SHA256(state).toHexString();
    const auto directory = package.getChildFile("plugin-state");
    if (!directory.createDirectory())
    {
        error = "Could not create the plugin-state directory.";
        return std::nullopt;
    }
    const auto file = directory.getChildFile(hash + ".bin");
    if (!file.existsAsFile())
    {
        const auto temporary = directory.getNonexistentChildFile(
            hash + ".tmp-",
            {},
            true);
        auto stream = temporary.createOutputStream();
        if (stream == nullptr
            || !stream->write(state.getData(), state.getSize()))
        {
            temporary.deleteFile();
            error = "Could not write plugin state.";
            return std::nullopt;
        }
        stream->flush();
        if (stream->getStatus().failed())
        {
            error = stream->getStatus().getErrorMessage();
            stream.reset();
            temporary.deleteFile();
            return std::nullopt;
        }
        stream.reset();
        if (!temporary.moveFileTo(file))
        {
            temporary.deleteFile();
            error = "Could not publish plugin state.";
            return std::nullopt;
        }
    }
    return PluginStateReference {
        "plugin-state/" + file.getFileName(),
        hash
    };
}

bool PluginStateStore::load(const juce::File& package,
                            const PluginStateReference& reference,
                            juce::MemoryBlock& state,
                            juce::String& error)
{
    if (reference.relativePath.isEmpty()
        || reference.hash.isEmpty()
        || reference.relativePath.contains("..")
        || juce::File::isAbsolutePath(reference.relativePath))
    {
        error = "Plugin state reference is invalid.";
        return false;
    }
    const auto file = package.getChildFile(reference.relativePath);
    if (!file.isAChildOf(package) || !file.existsAsFile())
    {
        error = "Plugin state file is missing.";
        return false;
    }
    if (juce::SHA256(file).toHexString() != reference.hash)
    {
        error = "Plugin state hash does not match its reference.";
        return false;
    }
    if (!file.loadFileAsData(state))
    {
        error = "Could not read plugin state.";
        return false;
    }
    return true;
}
}

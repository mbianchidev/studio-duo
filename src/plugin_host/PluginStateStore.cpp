#include "PluginStateStore.h"

#include <juce_cryptography/juce_cryptography.h>

#include <limits>

namespace studio
{
namespace
{
constexpr auto stateMagic = static_cast<juce::int64>(
    0x5354504c55475331ULL);
constexpr auto stateEnvelopeVersion = 1;

juce::MemoryBlock enveloped(const juce::MemoryBlock& state)
{
    juce::MemoryBlock result;
    juce::MemoryOutputStream output(result, true);
    output.writeInt64(stateMagic);
    output.writeInt(stateEnvelopeVersion);
    output.writeInt64(static_cast<juce::int64>(state.getSize()));
    output.write(state.getData(), state.getSize());
    output.flush();
    return result;
}
}

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
    const auto reference = PluginStateReference {
        "plugin-state/" + file.getFileName(),
        hash
    };
    if (file.existsAsFile())
    {
        juce::MemoryBlock existing;
        juce::String existingError;
        if (load(package, reference, existing, existingError)
            && existing == state)
            return reference;
    }

    const auto encoded = enveloped(state);
    const auto temporary = directory.getNonexistentChildFile(
        hash + ".tmp-",
        {},
        true);
    auto stream = temporary.createOutputStream();
    if (stream == nullptr
        || !stream->write(encoded.getData(), encoded.getSize()))
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
    const auto published = file.existsAsFile()
        ? temporary.replaceFileIn(file)
        : temporary.moveFileTo(file);
    if (!published)
    {
        temporary.deleteFile();
        error = "Could not publish plugin state.";
        return std::nullopt;
    }
    juce::MemoryBlock verified;
    if (!load(package, reference, verified, error)
        || verified != state)
        return std::nullopt;
    return reference;
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
    juce::MemoryBlock encoded;
    if (!file.loadFileAsData(encoded))
    {
        error = "Could not read plugin state.";
        return false;
    }
    state = encoded;
    if (encoded.getSize() >= sizeof(juce::int64) * 2 + sizeof(int))
    {
        juce::MemoryInputStream input(encoded, false);
        if (input.readInt64() == stateMagic)
        {
            const auto version = input.readInt();
            const auto payloadSize = input.readInt64();
            if (version != stateEnvelopeVersion
                || payloadSize <= 0
                || payloadSize
                    != static_cast<juce::int64>(
                        input.getNumBytesRemaining())
                || payloadSize > std::numeric_limits<int>::max())
            {
                error = "Plugin state envelope is truncated or unsupported.";
                state.reset();
                return false;
            }
            state.setSize(
                static_cast<std::size_t>(payloadSize),
                false);
            if (input.read(
                    state.getData(),
                    static_cast<int>(payloadSize))
                != payloadSize)
            {
                error = "Plugin state payload is truncated.";
                state.reset();
                return false;
            }
        }
    }
    if (juce::SHA256(state).toHexString() != reference.hash)
    {
        error = "Plugin state hash does not match its reference.";
        state.reset();
        return false;
    }
    return true;
}

bool PluginStateStore::materialize(
    const juce::File& sourcePackage,
    const juce::File& destinationPackage,
    const PluginStateReference& reference,
    juce::String& error)
{
    if (reference.relativePath.isEmpty() && reference.hash.isEmpty())
        return true;

    juce::MemoryBlock state;
    juce::String destinationError;
    if (load(destinationPackage,
             reference,
             state,
             destinationError))
        return true;

    state.reset();
    if (!load(sourcePackage, reference, state, error))
        return false;
    const auto stored = store(destinationPackage, state, error);
    if (!stored.has_value()
        || stored->relativePath != reference.relativePath
        || stored->hash != reference.hash)
    {
        if (error.isEmpty())
            error = "Copied plugin state did not preserve its reference.";
        return false;
    }
    return true;
}
}

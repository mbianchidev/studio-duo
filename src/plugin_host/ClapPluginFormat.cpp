#include "ClapPluginFormat.h"

#include "ClapPluginInstance.h"

#include <clap/clap.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace studio
{
namespace
{
juce::String pluginFile(const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.upToFirstOccurrenceOf("|", false, false);
}

juce::File executableFor(const juce::File& plugin)
{
#if JUCE_MAC
    if (plugin.isDirectory())
    {
        const auto executableDirectory = plugin.getChildFile("Contents")
                                             .getChildFile("MacOS");
        const auto expected = executableDirectory.getChildFile(
            plugin.getFileNameWithoutExtension());
        if (expected.existsAsFile())
            return expected;
        juce::Array<juce::File> executables;
        executableDirectory.findChildFiles(
            executables,
            juce::File::findFiles,
            false);
        if (!executables.isEmpty())
            return executables.getFirst();
    }
#endif
    return plugin;
}

struct ScannedModule
{
    explicit ScannedModule(const juce::File& pluginFile)
        : bundle(pluginFile)
    {
        const auto executable = executableFor(bundle);
        if (!executable.existsAsFile()
            || !library.open(executable.getFullPathName()))
            return;
        entry = reinterpret_cast<const clap_plugin_entry_t*>(
            library.getFunction("clap_entry"));
        if (entry == nullptr
            || !clap_version_is_compatible(entry->clap_version)
            || !entry->init(bundle.getFullPathName().toRawUTF8()))
        {
            entry = nullptr;
            library.close();
            return;
        }
        initialized = true;
        factory = static_cast<const clap_plugin_factory_t*>(
            entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    }

    ~ScannedModule()
    {
        if (initialized && entry != nullptr)
            entry->deinit();
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return factory != nullptr;
    }

    juce::File bundle;
    juce::DynamicLibrary library;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    bool initialized = false;
};

bool featureListContains(const char* const* features, const char* feature)
{
    if (features == nullptr)
        return false;
    for (auto current = features; *current != nullptr; ++current)
        if (std::strcmp(*current, feature) == 0)
            return true;
    return false;
}
}

juce::String ClapPluginFormat::getName() const
{
    return "CLAP";
}

void ClapPluginFormat::findAllTypesForFile(
    juce::OwnedArray<juce::PluginDescription>& results,
    const juce::String& fileOrIdentifier)
{
    const auto* messages =
        juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && !messages->isThisTheMessageThread())
    {
        juce::MessageManager::callSync(
            [this, &results, fileOrIdentifier]
            {
                findAllTypesForFile(results, fileOrIdentifier);
            });
        return;
    }
    const juce::File file(pluginFile(fileOrIdentifier));
    ScannedModule module(file);
    if (!module.valid())
        return;

    const auto count = module.factory->get_plugin_count(module.factory);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto* descriptor = module.factory->get_plugin_descriptor(
            module.factory,
            index);
        if (descriptor == nullptr
            || descriptor->id == nullptr
            || descriptor->name == nullptr)
            continue;

        auto description = std::make_unique<juce::PluginDescription>();
        description->name = juce::String::fromUTF8(descriptor->name);
        description->descriptiveName = description->name;
        description->manufacturerName = descriptor->vendor != nullptr
            ? juce::String::fromUTF8(descriptor->vendor)
            : juce::String();
        description->version = descriptor->version != nullptr
            ? juce::String::fromUTF8(descriptor->version)
            : juce::String();
        description->category =
            featureListContains(descriptor->features, CLAP_PLUGIN_FEATURE_INSTRUMENT)
            ? "Instrument"
            : "Effect";
        description->pluginFormatName = getName();
        description->fileOrIdentifier = file.getFullPathName()
            + "|"
            + juce::String::fromUTF8(descriptor->id);
        description->uniqueId = description->fileOrIdentifier.hashCode();
        description->deprecatedUid = description->uniqueId;
        description->isInstrument =
            featureListContains(descriptor->features,
                                CLAP_PLUGIN_FEATURE_INSTRUMENT);
        description->numInputChannels = description->isInstrument ? 0 : 2;
        description->numOutputChannels = 2;
        description->lastFileModTime = file.getLastModificationTime();
        description->lastInfoUpdateTime = juce::Time::getCurrentTime();
        results.add(std::move(description));
    }
}

bool ClapPluginFormat::fileMightContainThisPluginType(
    const juce::String& fileOrIdentifier)
{
    return juce::File(pluginFile(fileOrIdentifier)).hasFileExtension("clap");
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier(
    const juce::String& fileOrIdentifier)
{
    return juce::File(pluginFile(fileOrIdentifier)).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning(
    const juce::PluginDescription& description)
{
    const juce::File file(pluginFile(description.fileOrIdentifier));
    return !file.exists()
        || file.getLastModificationTime() != description.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist(
    const juce::PluginDescription& description)
{
    return juce::File(pluginFile(description.fileOrIdentifier)).exists();
}

bool ClapPluginFormat::canScanForPlugins() const
{
    return true;
}

bool ClapPluginFormat::isTrivialToScan() const
{
    return false;
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins(
    const juce::FileSearchPath& directoriesToSearch,
    bool recursive,
    bool)
{
    juce::StringArray result;
    for (int index = 0; index < directoriesToSearch.getNumPaths(); ++index)
    {
        juce::Array<juce::File> files;
        directoriesToSearch[index].findChildFiles(
            files,
            juce::File::findFilesAndDirectories,
            recursive,
            "*.clap");
        for (const auto& file : files)
            if (file.hasFileExtension("clap"))
                result.addIfNotAlreadyThere(file.getFullPathName());
    }
    return result;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    juce::FileSearchPath paths;
#if JUCE_MAC
    paths.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                  .getChildFile("Library/Audio/Plug-Ins/CLAP"));
    paths.add(juce::File("/Library/Audio/Plug-Ins/CLAP"));
#elif JUCE_WINDOWS
    if (const auto* common = std::getenv("COMMONPROGRAMFILES"))
        paths.add(juce::File(juce::String::fromUTF8(common) + "\\CLAP"));
    if (const auto* local = std::getenv("LOCALAPPDATA"))
        paths.add(juce::File(juce::String::fromUTF8(local)
                             + "\\Programs\\Common\\CLAP"));
#else
    paths.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                  .getChildFile(".clap"));
    paths.add(juce::File("/usr/lib/clap"));
#endif
    if (const auto* clapPath = std::getenv("CLAP_PATH"))
    {
        juce::StringArray environmentPaths;
#if JUCE_WINDOWS
        environmentPaths.addTokens(juce::String::fromUTF8(clapPath), ";", "");
#else
        environmentPaths.addTokens(juce::String::fromUTF8(clapPath), ":", "");
#endif
        for (const auto& path : environmentPaths)
            paths.add(juce::File(path));
    }
    return paths;
}

bool ClapPluginFormat::requiresUnblockedMessageThreadDuringCreation(
    const juce::PluginDescription&) const
{
    return false;
}

void ClapPluginFormat::createPluginInstance(
    const juce::PluginDescription& description,
    double sampleRate,
    int blockSize,
    PluginCreationCallback callback)
{
    juce::String error;
    auto instance = ClapPluginInstance::create(
        description,
        sampleRate,
        blockSize,
        error);
    callback(std::move(instance), error);
}
}

#include "PluginSearchPaths.h"

namespace studio
{
namespace
{
juce::String normalizePathText(juce::String path,
                               PluginSearchPathStyle style)
{
    path = path.trim();
    if (path.isEmpty())
        return {};

    const auto separator =
        style == PluginSearchPathStyle::windows ? '\\' : '/';
    if (style == PluginSearchPathStyle::windows)
        path = path.replaceCharacter('/', separator);

    const auto preserveLeadingPair =
        style == PluginSearchPathStyle::windows
        && path.startsWith("\\\\");
    juce::String normalized;
    auto previousWasSeparator = false;
    for (int index = 0; index < path.length(); ++index)
    {
        const auto character = path[index];
        const auto isSeparator = character == separator;
        if (!isSeparator
            || !previousWasSeparator
            || (preserveLeadingPair && normalized.length() == 1))
        {
            normalized += character;
        }
        previousWasSeparator = isSeparator;
    }

    const auto isDriveRoot =
        style == PluginSearchPathStyle::windows
        && normalized.length() == 3
        && normalized[1] == ':'
        && normalized[2] == separator;
    const auto isUncPrefix =
        style == PluginSearchPathStyle::windows
        && normalized == "\\\\";
    while (normalized.length() > 1
           && normalized.endsWithChar(separator)
           && !isDriveRoot
           && !isUncPrefix)
    {
        normalized = normalized.dropLastCharacters(1);
    }
    return normalized;
}

juce::String comparisonKey(const juce::String& path,
                           PluginSearchPathStyle style)
{
    auto key = normalizePathText(path, style);
    if (style == PluginSearchPathStyle::windows)
        key = key.toLowerCase();
    return key;
}

int indexOfPath(const juce::StringArray& paths,
                const juce::String& path,
                PluginSearchPathStyle style)
{
    const auto target = comparisonKey(path, style);
    for (int index = 0; index < paths.size(); ++index)
        if (comparisonKey(paths[index], style) == target)
            return index;
    return -1;
}
}

PluginSearchPaths::PluginSearchPaths(juce::File settingsFile)
    : file(std::move(settingsFile))
{
}

bool PluginSearchPaths::load(juce::String& error)
{
    writable = true;
    folders.clear();
    if (!file.existsAsFile())
        return true;

    const auto value = juce::JSON::parse(file.loadFileAsString());
    const auto* root = value.getDynamicObject();
    if (root == nullptr
        || static_cast<int>(root->getProperty("schemaVersion")) != 1
        || !root->getProperty("customVst3Folders").isArray())
    {
        writable = false;
        error = "VST3 search-folder settings are corrupt or unsupported.";
        return false;
    }

    juce::StringArray loadedFolders;
    for (const auto& folderValue :
         *root->getProperty("customVst3Folders").getArray())
    {
        if (!folderValue.isString())
        {
            writable = false;
            error = "VST3 search-folder settings contain an invalid path.";
            folders.clear();
            return false;
        }

        const auto path = folderValue.toString().trim();
        if (path.isNotEmpty())
            loadedFolders.add(juce::File(path).getFullPathName());
    }

    folders = normalizeAndDeduplicate(loadedFolders, nativePathStyle());
    return true;
}

juce::Result PluginSearchPaths::addCustomFolder(const juce::File& folder)
{
    if (!folder.isDirectory())
        return juce::Result::fail(
            "The selected VST3 search path is not an existing folder: "
            + folder.getFullPathName());
    if (!folder.hasReadAccess())
        return juce::Result::fail(
            "The selected VST3 search folder is not readable: "
            + folder.getFullPathName());

    const auto normalized =
        normalizePathText(folder.getFullPathName(), nativePathStyle());
    if (normalized.isEmpty())
        return juce::Result::fail("The selected VST3 search folder is invalid.");
    if (indexOfPath(folders, normalized, nativePathStyle()) >= 0)
        return juce::Result::ok();

    const auto previous = folders;
    folders.add(normalized);
    folders = normalizeAndDeduplicate(folders, nativePathStyle());
    const auto result = save();
    if (result.failed())
        folders = previous;
    return result;
}

juce::Result PluginSearchPaths::removeCustomFolder(const juce::File& folder)
{
    const auto index = indexOfPath(
        folders,
        folder.getFullPathName(),
        nativePathStyle());
    if (index < 0)
        return juce::Result::fail(
            "The VST3 search folder is not configured: "
            + folder.getFullPathName());

    const auto previous = folders;
    folders.remove(index);
    const auto result = save();
    if (result.failed())
        folders = previous;
    return result;
}

juce::StringArray PluginSearchPaths::customFolders() const
{
    return folders;
}

PluginSearchPlan PluginSearchPaths::createScanPlan(
    const juce::FileSearchPath& defaultFolders) const
{
    PluginSearchPlan plan;
    juce::StringArray availableFolders;
    for (int index = 0; index < defaultFolders.getNumPaths(); ++index)
        availableFolders.add(defaultFolders[index].getFullPathName());

    for (const auto& path : folders)
    {
        const juce::File folder(path);
        if (!folder.exists())
        {
            plan.warnings.add("missing: " + path);
            continue;
        }
        if (!folder.isDirectory())
        {
            plan.warnings.add("not a folder: " + path);
            continue;
        }
        if (!folder.hasReadAccess())
        {
            plan.warnings.add("not readable: " + path);
            continue;
        }
        availableFolders.add(path);
    }

    for (const auto& path :
         normalizeAndDeduplicate(availableFolders, nativePathStyle()))
    {
        plan.folders.add(juce::File(path));
    }
    plan.folders.removeRedundantPaths();
    return plan;
}

juce::StringArray PluginSearchPaths::normalizeAndDeduplicate(
    const juce::StringArray& paths,
    PluginSearchPathStyle style)
{
    juce::StringArray result;
    juce::StringArray keys;
    for (const auto& path : paths)
    {
        const auto normalized = normalizePathText(path, style);
        if (normalized.isEmpty())
            continue;

        const auto key = comparisonKey(normalized, style);
        if (keys.contains(key))
            continue;
        keys.add(key);
        result.add(normalized);
    }
    return result;
}

PluginSearchPathStyle PluginSearchPaths::nativePathStyle() noexcept
{
#if JUCE_WINDOWS
    return PluginSearchPathStyle::windows;
#else
    return PluginSearchPathStyle::posix;
#endif
}

juce::Result PluginSearchPaths::save() const
{
    if (!writable)
    {
        return juce::Result::fail(
            "VST3 search-folder settings were not overwritten after a failed load.");
    }
    if (!file.getParentDirectory().createDirectory())
    {
        return juce::Result::fail(
            "Could not create the VST3 search-folder settings directory.");
    }

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schemaVersion", 1);
    juce::Array<juce::var> folderValues;
    for (const auto& folder : folders)
        folderValues.add(folder);
    root->setProperty("customVst3Folders", juce::var(folderValues));

    const auto temporary = file.getSiblingFile(
        file.getFileName() + ".tmp-" + juce::Uuid().toString());
    if (!temporary.replaceWithText(
            juce::JSON::toString(juce::var(root.release()), true),
            false,
            false,
            "\n"))
    {
        return juce::Result::fail(
            "Could not write the VST3 search-folder settings.");
    }

    const auto replaced = file.existsAsFile()
        ? temporary.replaceFileIn(file)
        : temporary.moveFileTo(file);
    if (!replaced)
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not publish the VST3 search-folder settings.");
    }
    return juce::Result::ok();
}
}

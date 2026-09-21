#include "StudioPreferences.h"

#include <algorithm>

namespace studio
{
namespace
{
juce::var paletteToVar(
    const StudioThemePalette& palette)
{
    auto object = std::make_unique<juce::DynamicObject>();
    const auto setColour =
        [&object](const char* name, std::uint32_t colour)
        {
            object->setProperty(
                name,
                juce::Colour(colour).toString());
        };
    setColour("window", palette.window);
    setColour("panel", palette.panel);
    setColour("raised", palette.raised);
    setColour("transport", palette.transport);
    setColour(
        "transportRaised",
        palette.transportRaised);
    setColour("border", palette.border);
    setColour("text", palette.text);
    setColour(
        "secondaryText",
        palette.secondaryText);
    setColour("accent", palette.orange);
    setColour("amber", palette.amber);
    setColour("green", palette.green);
    setColour("violet", palette.violet);
    return juce::var(object.release());
}

std::optional<StudioThemePalette> paletteFromVar(
    const juce::var& value)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
        return std::nullopt;
    const auto colour =
        [object](const char* name)
            -> std::optional<std::uint32_t>
        {
            const auto property =
                object->getProperty(name);
            if (!property.isString())
                return std::nullopt;
            const auto text =
                property.toString();
            if (text.length() != 8
                || !text.containsOnly(
                    "0123456789abcdefABCDEF"))
                return std::nullopt;
            return juce::Colour::fromString(text)
                .getARGB();
        };
    const auto window = colour("window");
    const auto panel = colour("panel");
    const auto raised = colour("raised");
    const auto transport = colour("transport");
    const auto transportRaised =
        colour("transportRaised");
    const auto border = colour("border");
    const auto text = colour("text");
    const auto secondaryText =
        colour("secondaryText");
    const auto accent = colour("accent");
    const auto amber = colour("amber");
    const auto green = colour("green");
    const auto violet = colour("violet");
    if (!window
        || !panel
        || !raised
        || !transport
        || !transportRaised
        || !border
        || !text
        || !secondaryText
        || !accent
        || !amber
        || !green
        || !violet)
        return std::nullopt;
    return StudioThemePalette {
        *window,
        *panel,
        *raised,
        *transport,
        *transportRaised,
        *border,
        *text,
        *secondaryText,
        *accent,
        *amber,
        *green,
        *violet
    };
}

bool sameFile(const juce::File& left,
              const juce::File& right)
{
    return left == right;
}
}

StudioPreferences::StudioPreferences(
    juce::File file)
    : settingsFile(
          file == juce::File()
              ? defaultSettingsFile()
              : std::move(file))
{
    load();
}

bool StudioPreferences::autosaveEnabled() const noexcept
{
    return autosave;
}

bool StudioPreferences::scanPluginsAtStartup() const noexcept
{
    return scanAtStartup;
}

const juce::String&
StudioPreferences::themePresetId() const noexcept
{
    return selectedThemePreset;
}

StudioThemePalette
StudioPreferences::themePalette() const
{
    if (selectedThemePreset == "custom")
        return customTheme;
    if (const auto preset =
            studioThemePaletteForPreset(
                selectedThemePreset))
        return *preset;
    return studioThemePresets().front().palette;
}

const std::vector<RecentProject>&
StudioPreferences::recentProjects() const noexcept
{
    return recent;
}

const juce::String&
StudioPreferences::status() const noexcept
{
    return statusMessage;
}

juce::Result StudioPreferences::setAutosaveEnabled(
    bool enabled)
{
    const auto previous = autosave;
    autosave = enabled;
    const auto result = save();
    if (result.failed())
        autosave = previous;
    else
        statusMessage = "Preferences saved.";
    return result;
}

juce::Result
StudioPreferences::setScanPluginsAtStartup(
    bool enabled)
{
    const auto previous = scanAtStartup;
    scanAtStartup = enabled;
    const auto result = save();
    if (result.failed())
        scanAtStartup = previous;
    else
        statusMessage = "Preferences saved.";
    return result;
}

juce::Result StudioPreferences::setThemePreset(
    const juce::String& presetId)
{
    if (presetId != "custom"
        && !studioThemePaletteForPreset(presetId))
        return juce::Result::fail(
            "The selected theme preset is unavailable.");
    const auto previous = selectedThemePreset;
    selectedThemePreset = presetId;
    const auto result = save();
    if (result.failed())
        selectedThemePreset = previous;
    else
        statusMessage = "Theme preference saved.";
    return result;
}

juce::Result
StudioPreferences::setCustomThemePalette(
    const StudioThemePalette& palette)
{
    juce::String error;
    if (!palette.isAccessible(error))
        return juce::Result::fail(error);
    const auto previousPreset =
        selectedThemePreset;
    const auto previousPalette = customTheme;
    selectedThemePreset = "custom";
    customTheme = palette;
    const auto result = save();
    if (result.failed())
    {
        selectedThemePreset = previousPreset;
        customTheme = previousPalette;
    }
    else
    {
        statusMessage = "Custom theme saved.";
    }
    return result;
}

juce::Result StudioPreferences::resetTheme()
{
    return setThemePreset("studio-gray");
}

juce::Result StudioPreferences::recordRecentProject(
    const juce::File& package,
    const juce::String& projectName,
    juce::Time editedAt)
{
    if (package == juce::File())
        return juce::Result::fail(
            "A recent project requires a package path.");
    const auto previous = recent;
    recent.erase(
        std::remove_if(
            recent.begin(),
            recent.end(),
            [&package](const auto& entry)
            {
                return sameFile(entry.file, package);
            }),
        recent.end());
    recent.push_back({
        package,
        projectName.isNotEmpty()
            ? projectName
            : package.getFileNameWithoutExtension(),
        editedAt
    });
    std::stable_sort(
        recent.begin(),
        recent.end(),
        [](const auto& left, const auto& right)
        {
            return left.lastEdited
                > right.lastEdited;
        });
    constexpr std::size_t maximumRecentProjects = 12;
    if (recent.size() > maximumRecentProjects)
        recent.resize(maximumRecentProjects);
    const auto result = save();
    if (result.failed())
        recent = previous;
    else
        statusMessage = "Recent projects updated.";
    return result;
}

juce::Result StudioPreferences::removeRecentProject(
    const juce::File& package)
{
    const auto previous = recent;
    recent.erase(
        std::remove_if(
            recent.begin(),
            recent.end(),
            [&package](const auto& entry)
            {
                return sameFile(entry.file, package);
            }),
        recent.end());
    const auto result = save();
    if (result.failed())
        recent = previous;
    else
        statusMessage = "Recent project removed.";
    return result;
}

juce::File StudioPreferences::defaultSettingsFile()
{
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Studio Duo")
        .getChildFile("preferences.json");
}

void StudioPreferences::load()
{
    statusMessage.clear();
    if (!settingsFile.existsAsFile())
        return;
    const auto parsed = juce::JSON::parse(
        settingsFile.loadFileAsString());
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr
        || !object->getProperty(
                "autosaveEnabled")
                .isBool()
        || (object->hasProperty(
                "scanPluginsAtStartup")
            && !object->getProperty(
                    "scanPluginsAtStartup")
                    .isBool()))
    {
        statusMessage =
            "Preferences could not be read; defaults are active.";
        juce::Logger::writeToLog(statusMessage);
        return;
    }
    autosave = static_cast<bool>(
        object->getProperty("autosaveEnabled"));
    if (object->hasProperty(
            "scanPluginsAtStartup"))
    {
        scanAtStartup = static_cast<bool>(
            object->getProperty(
                "scanPluginsAtStartup"));
    }

    auto hadInvalidOptionalData = false;
    if (object->hasProperty("themePreset"))
    {
        const auto preset =
            object->getProperty(
                "themePreset")
                .toString();
        if (preset == "custom"
            || studioThemePaletteForPreset(preset))
        {
            selectedThemePreset = preset;
        }
        else
        {
            hadInvalidOptionalData = true;
        }
    }
    if (object->hasProperty("customTheme"))
    {
        const auto parsedPalette = paletteFromVar(
            object->getProperty("customTheme"));
        juce::String error;
        if (parsedPalette
            && parsedPalette->isAccessible(error))
        {
            customTheme = *parsedPalette;
        }
        else
        {
            hadInvalidOptionalData = true;
            if (selectedThemePreset == "custom")
                selectedThemePreset =
                    "studio-gray";
        }
    }
    if (const auto recentValue =
            object->getProperty("recentProjects");
        recentValue.isArray())
    {
        for (const auto& item :
             *recentValue.getArray())
        {
            const auto* entry =
                item.getDynamicObject();
            if (entry == nullptr)
            {
                hadInvalidOptionalData = true;
                continue;
            }
            const auto path =
                entry->getProperty("path").toString();
            const auto name =
                entry->getProperty("name").toString();
            const auto edited =
                entry->getProperty("lastEditedMs");
            if (path.isEmpty()
                || (!edited.isInt()
                    && !edited.isInt64()
                    && !edited.isDouble()))
            {
                hadInvalidOptionalData = true;
                continue;
            }
            recent.push_back({
                juce::File(path),
                name.isNotEmpty()
                    ? name
                    : juce::File(path)
                          .getFileNameWithoutExtension(),
                juce::Time(
                    static_cast<juce::int64>(
                        edited))
            });
        }
        std::stable_sort(
            recent.begin(),
            recent.end(),
            [](const auto& left,
               const auto& right)
            {
                return left.lastEdited
                    > right.lastEdited;
            });
    }
    if (hadInvalidOptionalData)
    {
        statusMessage =
            "Some preference values were invalid; safe defaults are active.";
        juce::Logger::writeToLog(statusMessage);
    }
}

juce::Result StudioPreferences::save()
{
    const auto directory =
        settingsFile.getParentDirectory();
    if (!directory.createDirectory())
        return juce::Result::fail(
            "Could not create the preferences directory.");

    auto object =
        std::make_unique<juce::DynamicObject>();
    object->setProperty("schemaVersion", 2);
    object->setProperty(
        "autosaveEnabled",
        autosave);
    object->setProperty(
        "scanPluginsAtStartup",
        scanAtStartup);
    object->setProperty(
        "themePreset",
        selectedThemePreset);
    object->setProperty(
        "customTheme",
        paletteToVar(customTheme));
    juce::Array<juce::var> recentValues;
    for (const auto& entry : recent)
    {
        auto recentObject =
            std::make_unique<juce::DynamicObject>();
        recentObject->setProperty(
            "path",
            entry.file.getFullPathName());
        recentObject->setProperty(
            "name",
            entry.name);
        recentObject->setProperty(
            "lastEditedMs",
            entry.lastEdited.toMilliseconds());
        recentValues.add(
            juce::var(recentObject.release()));
    }
    object->setProperty(
        "recentProjects",
        juce::var(recentValues));

    const auto temporary =
        settingsFile.getSiblingFile(
            settingsFile.getFileName()
            + ".tmp");
    if (!temporary.replaceWithText(
            juce::JSON::toString(
                juce::var(object.release()),
                true)))
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not write the preferences file.");
    }
    if (!temporary.replaceFileIn(settingsFile))
    {
        temporary.deleteFile();
        return juce::Result::fail(
            "Could not publish the preferences file.");
    }
    return juce::Result::ok();
}
}

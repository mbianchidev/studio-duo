#include "StudioPreferences.h"

namespace studio
{
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

const juce::String& StudioPreferences::status() const noexcept
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
        || !object->getProperty("autosaveEnabled").isBool())
    {
        statusMessage =
            "Preferences could not be read; defaults are active.";
        juce::Logger::writeToLog(statusMessage);
        return;
    }
    autosave =
        static_cast<bool>(
            object->getProperty("autosaveEnabled"));
}

juce::Result StudioPreferences::save()
{
    const auto directory =
        settingsFile.getParentDirectory();
    if (!directory.createDirectory())
        return juce::Result::fail(
            "Could not create the preferences directory.");

    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("schemaVersion", 1);
    object->setProperty("autosaveEnabled", autosave);
    const auto temporary =
        settingsFile.getSiblingFile(
            settingsFile.getFileName() + ".tmp");
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

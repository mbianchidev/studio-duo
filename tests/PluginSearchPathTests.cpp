#include "TestHarness.h"
#include "TestSuites.h"

#include "plugin_host/PluginFormats.h"
#include "plugin_host/PluginSearchPaths.h"

void pluginSearchPathTests()
{
    juce::StringArray windowsPaths;
    windowsPaths.add("C:\\Program Files\\Common Files\\VST3\\");
    windowsPaths.add("c:/program files/common files/vst3");
    windowsPaths.add("D:\\Audio\\VST3");
    const auto normalizedWindows =
        studio::PluginSearchPaths::normalizeAndDeduplicate(
            windowsPaths,
            studio::PluginSearchPathStyle::windows);
    expect(
        normalizedWindows.size() == 2
            && normalizedWindows[0]
                == "C:\\Program Files\\Common Files\\VST3"
            && normalizedWindows[1] == "D:\\Audio\\VST3",
        "Windows VST3 paths normalize separators and deduplicate without case sensitivity.");

    juce::StringArray macPaths;
    macPaths.add("/Library/Audio/Plug-Ins/VST3/");
    macPaths.add("/Library/Audio/Plug-Ins/VST3");
    macPaths.add("/library/Audio/Plug-Ins/VST3");
    macPaths.add("/Users/test/Library/Audio/Plug-Ins/VST3");
    const auto normalizedMac =
        studio::PluginSearchPaths::normalizeAndDeduplicate(
            macPaths,
            studio::PluginSearchPathStyle::posix);
    expect(
        normalizedMac.size() == 3
            && normalizedMac[0] == "/Library/Audio/Plug-Ins/VST3"
            && normalizedMac[1] == "/library/Audio/Plug-Ins/VST3"
            && normalizedMac[2]
                == "/Users/test/Library/Audio/Plug-Ins/VST3",
        "macOS VST3 paths normalize trailing separators while preserving case-sensitive filesystems.");

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile(
                              "StudioDuoPluginSearchPaths",
                              juce::String(),
                              false);
    auto result = root.createDirectory();
    auto resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());

    const auto firstFolder = root.getChildFile("First VST3");
    const auto secondFolder = root.getChildFile("Second VST3");
    result = firstFolder.createDirectory();
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    result = secondFolder.createDirectory();
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    const auto settingsFile = root.getChildFile("search-paths.json");
    studio::PluginSearchPaths settings(settingsFile);
    juce::String error;
    expect(settings.load(error), error.toRawUTF8());
    result = settings.addCustomFolder(firstFolder);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    result = settings.addCustomFolder(
        juce::File(firstFolder.getFullPathName() + juce::File::getSeparatorString()));
    resultMessage = result.getErrorMessage();
    expect(result.wasOk() && settings.customFolders().size() == 1,
           resultMessage.toRawUTF8());
    result = settings.addCustomFolder(secondFolder);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());

    studio::PluginSearchPaths restored(settingsFile);
    error.clear();
    expect(restored.load(error)
               && restored.customFolders().size() == 2,
           "Custom VST3 folders survive application restart.");
    result = restored.removeCustomFolder(firstFolder);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    studio::PluginSearchPaths afterRemoval(settingsFile);
    error.clear();
    expect(afterRemoval.load(error)
               && afterRemoval.customFolders().size() == 1
               && afterRemoval.customFolders()[0]
                    == secondFolder.getFullPathName(),
           "Removed VST3 folders stay removed after application restart.");

    const auto validationFile = root.getChildFile("validation-paths.json");
    const auto validFolder = root.getChildFile("Valid VST3");
    const auto missingFolder = root.getChildFile("Missing VST3");
    const auto invalidFolder = root.getChildFile("Invalid VST3");
    result = validFolder.createDirectory();
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    result = missingFolder.createDirectory();
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    result = invalidFolder.createDirectory();
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    studio::PluginSearchPaths validationSettings(validationFile);
    error.clear();
    expect(validationSettings.load(error), error.toRawUTF8());
    result = validationSettings.addCustomFolder(validFolder);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    result = validationSettings.addCustomFolder(missingFolder);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    result = validationSettings.addCustomFolder(invalidFolder);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    expect(missingFolder.deleteRecursively(),
           "Missing-folder fixture is removed.");
    expect(invalidFolder.deleteRecursively(),
           "Invalid-folder fixture directory is removed.");
    expect(invalidFolder.replaceWithText("not a directory"),
           "Invalid-folder fixture is replaced with a file.");

    const auto defaultFolder = root.getChildFile("Default VST3");
    result = defaultFolder.createDirectory();
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    juce::FileSearchPath defaults;
    defaults.add(defaultFolder);
    const auto plan = validationSettings.createScanPlan(defaults);
    expect(
        plan.folders.getNumPaths() == 2
            && plan.warnings.size() == 2
            && plan.warnings.joinIntoString(" ").contains("missing:")
            && plan.warnings.joinIntoString(" ").contains("not a folder:"),
        "Unavailable custom VST3 folders are reported and skipped without blocking valid folders.");

    juce::AudioPluginFormatManager formatManager;
    studio::PluginFormats::addSupportedFormats(formatManager);
    juce::AudioPluginFormat* vst3Format = nullptr;
    for (auto* format : formatManager.getFormats())
        if (format->getName() == "VST3")
            vst3Format = format;
    const juce::File fixture(STUDIO_DUO_ARA_FIXTURE_PATH);
    const auto discoveryRoot = fixture.getParentDirectory();
    const auto discoveryFile = root.getChildFile("discovery-paths.json");
    studio::PluginSearchPaths discoverySettings(discoveryFile);
    error.clear();
    expect(discoverySettings.load(error), error.toRawUTF8());
    result = discoverySettings.addCustomFolder(discoveryRoot);
    resultMessage = result.getErrorMessage();
    expect(result.wasOk(), resultMessage.toRawUTF8());
    const auto discoveryPlan = discoverySettings.createScanPlan({});
    const auto identifiers =
        vst3Format != nullptr
            ? vst3Format->searchPathsForPlugins(
                  discoveryPlan.folders,
                  true,
                  false)
            : juce::StringArray();
    auto foundFixture = false;
    for (const auto& identifier : identifiers)
        foundFixture = foundFixture || juce::File(identifier) == fixture;
    expect(
        vst3Format != nullptr && foundFixture,
        "A VST3 outside default locations is discovered through its configured custom folder.");

    expect(root.deleteRecursively(),
           "Plugin search-path test directory is removed.");
}

#include "TestHarness.h"
#include "TestSuites.h"

#include "project_io/ProjectFile.h"

void projectMigrationTests()
{
    auto project = studio::Project::createDefault();
    studio::AutomationLane lane;
    lane.name = "Volume";
    lane.target.trackId = project.tracks.front().id;
    lane.points.push_back({
        juce::Uuid().toString(),
        0.0,
        0.5
    });
    project.automationLanes.push_back(lane);

    const auto package = juce::File::getSpecialLocation(
                             juce::File::tempDirectory)
                             .getNonexistentChildFile(
                                 "StudioDuoMigration",
                                 ".studioduo",
                                 false);
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "Version 3 project package can be saved.");
    const auto manifest = juce::JSON::parse(
        package.getChildFile("manifest.json").loadFileAsString());
    const auto* manifestObject = manifest.getDynamicObject();
    expect(manifestObject != nullptr
               && manifestObject->getProperty("requiredCapabilities").isArray()
               && manifestObject->getProperty("activeAutomation").toString()
                      .isNotEmpty(),
           "Version 3 manifest declares capabilities and automation generation.");

    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value()
               && loaded->automationLanes.size() == 1,
           "Version 3 automation generation loads.");

    auto mismatchedManifest = manifest.clone();
    mismatchedManifest.getDynamicObject()->setProperty("formatVersion", 2);
    package.getChildFile("manifest.json").replaceWithText(
        juce::JSON::toString(mismatchedManifest, true));
    error.clear();
    expect(!studio::ProjectFile::load(package, error).has_value()
               && error.containsIgnoreCase("versions"),
           "Manifest and session version mismatches are rejected.");

    expect(studio::ProjectFile::save(project, package).wasOk(),
           "Project can be saved after a rejected manifest.");
    const auto currentManifest = juce::JSON::parse(
        package.getChildFile("manifest.json").loadFileAsString());
    const auto automationPath =
        currentManifest.getDynamicObject()
            ->getProperty("activeAutomation")
            .toString();
    package.getChildFile(automationPath).deleteFile();
    error.clear();
    expect(!studio::ProjectFile::load(package, error).has_value()
               && error.containsIgnoreCase("automation"),
           "Missing automation generations are reported.");
    package.deleteRecursively();
}

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
           "Current project package can be saved.");
    const auto manifest = juce::JSON::parse(
        package.getChildFile("manifest.json").loadFileAsString());
    const auto* manifestObject = manifest.getDynamicObject();
    auto hasMidiCapability = false;
    auto hasBundledCompositionCapability = false;
    auto hasDawProjectCapability = false;
    auto hasSceneCapability = false;
    auto hasCompatibilityReportCapability = false;
    if (manifestObject != nullptr)
    {
        const auto required =
            manifestObject->getProperty("requiredCapabilities");
        if (required.isArray())
        {
            for (const auto& capability : *required.getArray())
            {
                hasMidiCapability = hasMidiCapability
                    || capability.toString() == "midiCompositionV1";
                hasBundledCompositionCapability =
                    hasBundledCompositionCapability
                    || capability.toString()
                        == "bundledCompositionDevicesV1";
                hasDawProjectCapability = hasDawProjectCapability
                    || capability.toString() == "dawprojectV1";
                hasSceneCapability = hasSceneCapability
                    || capability.toString() == "scenesV1";
                hasCompatibilityReportCapability =
                    hasCompatibilityReportCapability
                    || capability.toString()
                        == "compatibilityReportsV1";
            }
        }
    }
    expect(manifestObject != nullptr
               && manifestObject->getProperty("requiredCapabilities").isArray()
               && manifestObject->getProperty("activeAutomation").toString()
                      .isNotEmpty()
               && hasMidiCapability
               && hasBundledCompositionCapability
               && hasDawProjectCapability
               && hasSceneCapability
               && hasCompatibilityReportCapability,
           "Version 7 manifest declares MIDI, bundled-device, scene, report, DAWproject, and automation capabilities.");

    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value()
               && loaded->automationLanes.size() == 1,
           "Version 7 automation generation loads.");

    auto versionSix = project.toVar();
    auto* versionSixObject = versionSix.getDynamicObject();
    versionSixObject->setProperty("formatVersion", 6);
    versionSixObject->removeProperty("metadata");
    versionSixObject->removeProperty("scenes");
    versionSixObject->removeProperty("compatibilityReports");
    error.clear();
    const auto migratedVersionSix =
        studio::Project::fromVar(versionSix, error);
    expect(migratedVersionSix.has_value()
               && migratedVersionSix->scenes.empty()
               && migratedVersionSix->compatibilityReports.empty()
               && migratedVersionSix->metadata.artist.isEmpty(),
           "Version 6 projects migrate with empty metadata, scenes, and compatibility reports.");

    auto versionFive = project.toVar();
    versionFive.getDynamicObject()->setProperty("formatVersion", 5);
    for (auto& route :
         *versionFive.getDynamicObject()
              ->getProperty("routingConnections")
              .getArray())
    {
        route.getDynamicObject()->removeProperty("sourceInsertId");
        route.getDynamicObject()->removeProperty("sourceBusIndex");
    }
    error.clear();
    const auto migratedVersionFive =
        studio::Project::fromVar(versionFive, error);
    expect(migratedVersionFive.has_value()
               && migratedVersionFive->toVar()
                      .getDynamicObject()
                      ->getProperty("formatVersion")
                      == juce::var(
                          studio::Project::currentFormatVersion),
           "Version 5 projects migrate to version 7 with ordinary track-route sources.");

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

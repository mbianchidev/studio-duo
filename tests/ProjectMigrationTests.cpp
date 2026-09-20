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
    studio::AutomationLane channelPressure;
    channelPressure.name = "Channel pressure";
    channelPressure.target.type =
        studio::AutomationTargetType::midiChannelPressure;
    channelPressure.target.trackId = project.tracks.front().id;
    channelPressure.target.midiChannel = 4;
    channelPressure.points.push_back({
        juce::Uuid().toString(),
        0.5,
        0.75
    });
    project.automationLanes.push_back(channelPressure);

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
    auto hasMidiChannelPressureCapability = false;
    auto hasMasteringCapability = false;
    auto hasRenderReportV2Capability = false;
    auto hasSectionTransportCapability = false;
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
                hasMidiChannelPressureCapability =
                    hasMidiChannelPressureCapability
                    || capability.toString()
                        == "midiChannelPressureV1";
                hasMasteringCapability =
                    hasMasteringCapability
                    || capability.toString()
                        == "masteringAlbumV1";
                hasRenderReportV2Capability =
                    hasRenderReportV2Capability
                    || capability.toString()
                        == "renderReportsV2";
                hasSectionTransportCapability = hasSectionTransportCapability
                    || capability.toString() == "sectionTransportV1";
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
               && hasCompatibilityReportCapability
               && hasMidiChannelPressureCapability
               && hasMasteringCapability
               && hasRenderReportV2Capability
               && hasSectionTransportCapability,
           "Current manifests declare section transport and existing mastering, MIDI, device and interchange capabilities.");

    juce::String error;
    const auto loaded = studio::ProjectFile::load(package, error);
    expect(loaded.has_value()
               && loaded->automationLanes.size() == 2
               && loaded->automationLanes[1].target.type
                      == studio::AutomationTargetType::
                          midiChannelPressure
               && loaded->automationLanes[1].target.midiChannel == 4,
           "Version 8 automation generation preserves channel-scoped pressure.");

    auto legacyVersionSeven = project.toVar();
    auto* legacyObject = legacyVersionSeven.getDynamicObject();
    auto* legacyLanes =
        legacyObject->getProperty("automationLanes").getArray();
    legacyLanes->remove(1);
    legacyLanes->getReference(0)
        .getDynamicObject()
        ->getProperty("target")
        .getDynamicObject()
        ->removeProperty("midiChannel");
    error.clear();
    const auto loadedLegacyVersionSeven =
        studio::Project::fromVar(legacyVersionSeven, error);
    expect(loadedLegacyVersionSeven.has_value()
               && loadedLegacyVersionSeven->automationLanes.size()
                      == 1
               && loadedLegacyVersionSeven->automationLanes.front()
                      .target.midiChannel
                      == -1
               && loadedLegacyVersionSeven->toVar()
                      .getDynamicObject()
                      ->getProperty("formatVersion")
                      == juce::var(
                          studio::Project::currentFormatVersion),
           "Version 7 automation targets without MIDI channel metadata migrate to the current format.");

    auto legacyVersionEight = project.toVar();
    auto* legacyVersionEightObject =
        legacyVersionEight.getDynamicObject();
    legacyVersionEightObject->setProperty("formatVersion", 8);
    legacyVersionEightObject->removeProperty("mastering");
    error.clear();
    const auto loadedLegacyVersionEight =
        studio::Project::fromVar(legacyVersionEight, error);
    expect(loadedLegacyVersionEight.has_value()
               && loadedLegacyVersionEight->mastering.tracks.empty()
               && loadedLegacyVersionEight->mastering.references.empty(),
           "Version 8 projects migrate to the current format with an empty mastering album.");

    auto legacyNineProject = studio::Project::createDefault();
    legacyNineProject.sections.push_back({ "legacy-marker", "Legacy", 1.0 });
    legacyNineProject.tempoChanges = { { 1.0, 140.0, false } };
    legacyNineProject.loopStartSeconds = 1.25;
    legacyNineProject.loopEndSeconds = 6.75;
    auto legacyNine = legacyNineProject.toVar();
    legacyNine.getDynamicObject()->setProperty("formatVersion", 9);
    const auto loadedNine = studio::Project::fromVar(legacyNine, error);
    expect(loadedNine && loadedNine->timeSignatureNumerator == 4
               && loadedNine->timeSignatureDenominator == 4
               && loadedNine->sections.size() == 1
               && !loadedNine->sections.front().clickSettings
               && loadedNine->tempoChanges.front().sectionId.isEmpty()
               && loadedNine->tempoChanges == legacyNineProject.tempoChanges
               && loadedNine->loopStartSeconds == 1.25
               && loadedNine->loopEndSeconds == 6.75
               && static_cast<int>(loadedNine->toVar()["formatVersion"]) == 10,
           "Version 9 projects retain their loop/maps and 4/4 default without inventing section overrides.");

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
           "Version 5 projects migrate to version 8 with ordinary track-route sources.");

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

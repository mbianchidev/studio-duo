#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "project_io/ProjectFile.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

namespace
{
bool writeJson(const juce::File& file, const juce::var& value)
{
    return file.getParentDirectory().createDirectory().wasOk()
        && file.replaceWithText(juce::JSON::toString(value, true));
}

void manifestAndRecoveryGuards()
{
    release_fixture::Directory directory;
    const auto package = directory.file.getChildFile("guards.studioduo");
    auto project = studio::Project::createDefault();
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "The format-guard fixture can be saved.");
    const auto manifestFile = package.getChildFile("manifest.json");
    const auto original = juce::JSON::parse(manifestFile.loadFileAsString());
    const auto activeSession = package.getChildFile(original["activeSession"].toString());
    const auto activeAutomation = package.getChildFile(original["activeAutomation"].toString());
    const auto sessionBytes = activeSession.loadFileAsString();
    const auto automationBytes = activeAutomation.loadFileAsString();
    juce::String error;

    auto legacyCapabilities = original.clone();
    legacyCapabilities.getDynamicObject()->getProperty("requiredCapabilities").getArray()->add("renderReportsV1");
    expect(writeJson(manifestFile, legacyCapabilities), "The historical capability fixture can be written.");
    expect(studio::ProjectFile::load(package, error).has_value(),
           "Historical renderReportsV1 capability declarations remain readable.");
    expect(writeJson(manifestFile, original), "The current capability declaration can be restored.");

    for (const auto& version : { juce::var(studio::Project::currentFormatVersion + 1),
                                juce::var(12.5),
                                juce::var("12") })
    {
        auto manifest = original.clone();
        manifest.getDynamicObject()->setProperty("formatVersion", version);
        expect(writeJson(manifestFile, manifest), "The unsupported-version fixture can be written.");
        error.clear();
        expect(!studio::ProjectFile::loadForOpen(package, error) && error.isNotEmpty(),
               "Recovery cannot turn an unsupported format into a successful open.");
        expect(studio::ProjectFile::save(project, package).failed()
                   && activeSession.loadFileAsString() == sessionBytes
                   && activeAutomation.loadFileAsString() == automationBytes
                   && manifestFile.loadFileAsString() == juce::JSON::toString(manifest, true),
               "Saving refuses to overwrite an unsupported package.");
    }

    auto manifest = original.clone();
    manifest.getDynamicObject()->setProperty("projectId", "different-project");
    expect(writeJson(manifestFile, manifest), "The identity-mismatch fixture can be written.");
    error.clear();
    expect(!studio::ProjectFile::load(package, error) && error.containsIgnoreCase("identity"),
           "The manifest and session must describe the same project identity.");

    manifest = original.clone();
    manifest.getDynamicObject()->removeProperty("activeAutomation");
    expect(writeJson(manifestFile, manifest), "The missing-automation fixture can be written.");
    error.clear();
    expect(!studio::ProjectFile::load(package, error) && error.containsIgnoreCase("automation"),
           "Current packages cannot silently lose their independent automation generation.");
    error.clear();
    const auto recovered = studio::ProjectFile::loadForOpen(package, error);
    expect(recovered && recovered->recovered && recovered->warning.isNotEmpty(),
           "A damaged automation reference can recover with a visible warning.");

    expect(writeJson(manifestFile, original), "The original manifest can be restored.");
    for (const auto& generation : { juce::var(-1), juce::var(1.5),
                                    juce::var(std::numeric_limits<int>::max()) })
    {
        manifest = original.clone();
        manifest.getDynamicObject()->setProperty("generation", generation);
        expect(writeJson(manifestFile, manifest), "The invalid-generation fixture can be written.");
        expect(studio::ProjectFile::save(project, package).failed()
                   && activeSession.loadFileAsString() == sessionBytes
                   && activeAutomation.loadFileAsString() == automationBytes,
               "Invalid or exhausted generation counters never overwrite immutable data.");
    }
    expect(writeJson(manifestFile, original), "The valid generation can be restored.");

    auto automation = juce::JSON::parse(automationBytes);
    automation.getDynamicObject()->setProperty("schemaVersion", 1.5);
    expect(writeJson(activeAutomation, automation), "The fractional automation schema fixture can be written.");
    error.clear();
    expect(!studio::ProjectFile::load(package, error),
           "An automation schema version cannot be accepted by integer truncation.");
    expect(activeAutomation.replaceWithText(automationBytes),
           "The original automation generation can be restored.");

    project.name = "Unsaved synthetic edit";
    expect(studio::ProjectFile::writeRecoveryPoint(project, package).wasOk(),
           "The unsaved recovery fixture can be written.");
    const auto recoveryFile = package.getChildFile("recovery/latest.json");
    const auto recoveryBytes = recoveryFile.loadFileAsString();
    auto invalid = project;
    invalid.tempo = 0.0;
    expect(studio::ProjectFile::writeRecoveryPoint(invalid, package).failed()
               && recoveryFile.loadFileAsString() == recoveryBytes,
           "An invalid project cannot replace the only complete recovery point.");
    expect(recoveryFile.replaceWithText(recoveryBytes), "The valid recovery fixture can be retained.");
    auto recovery = juce::JSON::parse(recoveryFile.loadFileAsString());
    recovery.getDynamicObject()->setProperty("schemaVersion", 2);
    expect(writeJson(recoveryFile, recovery), "The future recovery schema fixture can be written.");
    error.clear();
    const auto opened = studio::ProjectFile::loadForOpen(package, error);
    expect(opened && !opened->recovered && opened->project.name != project.name
               && opened->warning.containsIgnoreCase("recovery"),
           "An unsupported recovery envelope is reported and cannot replace valid saved data.");
}

void historicalMigrationFixtures()
{
    const auto fixtures = juce::File(STUDIO_DUO_SOURCE_DIR).getChildFile("tests/fixtures");
    for (const auto* name : { "project-v1.json", "project-v2.json" })
    {
        const auto file = fixtures.getChildFile(name);
        const auto original = file.loadFileAsString();
        expect(original.isNotEmpty(), "The committed historical migration fixture exists.");
        const auto value = juce::JSON::parse(original);
        juce::String error;
        const auto first = studio::Project::fromVar(value, error);
        const auto second = studio::Project::fromVar(value, error);
        expect(first && second
                   && release_fixture::snapshot(*first) == release_fixture::snapshot(*second)
                   && juce::JSON::toString(value, true) == juce::JSON::toString(juce::JSON::parse(original), true)
                   && file.loadFileAsString() == original,
               "Historical migration is deterministic, repeatable, and does not mutate the only source copy.");
        if (!first)
        {
            expect(false, error.toRawUTF8());
            continue;
        }
        expect(std::abs(first->tempo - 137.0) < 0.000001
                   && std::abs(first->tracks.front().volumeDecibels + 6.0f) < 0.000001f
                   && std::abs(first->tracks.front().pan + 0.25f) < 0.000001f
                   && first->loopStartSeconds == 1.25 && first->loopEndSeconds == 6.75
                   && first->markers.size() == 1 && first->sections.size() == 1,
               "Historical mixer edits, loop bounds, and marker/section identities are preserved.");
        expect(first->routingConnections.front().destination.trackId
                   == (juce::String(name) == "project-v1.json" ? "synthetic-master" : "synthetic-bus"),
               "Version 1 direct-master and version 2 bus-routing semantics remain distinct.");
    }

    auto project = studio::Project::createDefault();
    studio::PluginInsert insert;
    insert.id = "synthetic-migration-plugin";
    insert.name = "Synthetic migration processor";
    insert.pluginIdentifier = "synthetic.migration.processor";
    insert.format = "VST3";
    project.tracks.front().inserts = { insert };
    for (int version = 3; version <= studio::Project::currentFormatVersion; ++version)
    {
        auto value = project.toVar();
        auto* object = value.getDynamicObject();
        object->setProperty("formatVersion", version);
        if (version < 5)
        {
            for (auto& track : *object->getProperty("tracks").getArray())
                track.getDynamicObject()->removeProperty("midiClips");
            for (const auto* property : { "drumMaps", "midiPatterns", "midiRoutingTemplates" })
                object->removeProperty(property);
        }
        if (version < 7)
            for (const auto* property : { "metadata", "scenes", "compatibilityReports" })
                object->removeProperty(property);
        if (version < 9)
            object->removeProperty("mastering");
        if (version < 11)
            object->removeProperty("markers");
        if (version < 12)
        {
            auto* legacyInsert = object->getProperty("tracks").getArray()->getReference(0)
                                     .getDynamicObject()->getProperty("inserts").getArray()
                                     ->getReference(0).getDynamicObject();
            for (const auto* property : { "kind", "containerId", "gainDecibels" })
                legacyInsert->removeProperty(property);
        }
        const auto original = juce::JSON::toString(value, true);
        juce::String error;
        const auto first = studio::Project::fromVar(value, error);
        const auto second = studio::Project::fromVar(value, error);
        expect(first && second
                   && release_fixture::snapshot(*first) == release_fixture::snapshot(*second)
                   && juce::JSON::toString(value, true) == original
                   && first->tracks.front().inserts.front().kind == studio::PluginInsertKind::plugin
                   && first->tracks.front().inserts.front().containerId.isEmpty()
                   && first->tracks.front().inserts.front().gainDecibels == 0.0f,
               ("Format " + juce::String(version)
                + " migrates without changing source data or introducing rack processing: " + error).toRawUTF8());
    }

    for (const auto& version : { juce::var(0), juce::var(-1), juce::var(12.5),
                                juce::var("12"), juce::var(13) })
    {
        auto value = project.toVar();
        value.getDynamicObject()->setProperty("formatVersion", version);
        juce::String error;
        expect(!studio::Project::fromVar(value, error) && error.isNotEmpty(),
               "Invalid, fractional, textual, and future project versions fail explicitly.");
    }
}
}

std::optional<int> runProjectSaveFixture(const juce::StringArray& arguments)
{
    juce::String checkpoint;
    juce::String packagePath;
    for (const auto& argument : arguments)
    {
        if (argument.startsWith("--save-interruption="))
            checkpoint = argument.fromFirstOccurrenceOf("=", false, false);
        if (argument.startsWith("--save-package="))
            packagePath = argument.fromFirstOccurrenceOf("=", false, false);
    }
    if (checkpoint.isEmpty())
        return std::nullopt;
    if (!juce::File::isAbsolutePath(packagePath))
    {
        std::cerr << "The save-interruption worker requires an absolute fixture package path.\n";
        return 2;
    }
    const juce::File package(packagePath);
    juce::String error;
    auto project = studio::ProjectFile::load(package, error);
    if (!project)
    {
        std::cerr << "Could not open the save-interruption fixture: " << error << '\n';
        return 3;
    }
    project->name = "Interrupted synthetic edit";
    if (const auto result = studio::ProjectFile::writeRecoveryPoint(*project, package); result.failed())
    {
        std::cerr << result.getErrorMessage() << '\n';
        return 4;
    }
    studio::ProjectFile::setWriteCheckpointForTesting(
        [checkpoint, package](const juce::File& destination,
                              studio::ProjectFile::WriteCheckpoint stage)
        {
            const auto relative = destination.getRelativePathFrom(package).replaceCharacter('\\', '/');
            const auto document = relative.startsWith("session/") ? "session"
                : relative.startsWith("automation/") ? "automation"
                : relative == "manifest.json" ? "manifest" : "recovery";
            const auto name = juce::String(document)
                + (stage == studio::ProjectFile::WriteCheckpoint::temporaryFlushed ? "-temporary" : "-published");
            if (checkpoint == "hold" && name == "manifest-temporary")
            {
                if (!package.getSiblingFile("save-lock.ready").replaceWithText("ready"))
                    std::_Exit(75);
                juce::Thread::sleep(10000);
            }
            if (name == checkpoint)
                std::_Exit(74);
        });
    const auto result = studio::ProjectFile::save(*project, package);
    std::cerr << (result.failed() ? result.getErrorMessage() : "The requested save checkpoint was not reached.") << '\n';
    return 5;
}

void projectHardeningTests()
{
    manifestAndRecoveryGuards();
    historicalMigrationFixtures();
}

void saveInterruptionTests()
{
    constexpr std::array checkpoints {
        "session-temporary", "session-published",
        "automation-temporary", "automation-published",
        "manifest-temporary", "manifest-published",
        "recovery-temporary", "recovery-published"
    };
    for (const auto* checkpoint : checkpoints)
    {
        release_fixture::Directory directory;
        const auto package = directory.file.getChildFile("interruption.studioduo");
        auto project = studio::Project::createDefault();
        project.name = "Prior valid save";
        expect(studio::ProjectFile::save(project, package).wasOk(),
               "The save-interruption fixture starts with a valid generation.");
        const auto priorSession = package.getChildFile("session/generation-00000001.json").loadFileAsString();
        const auto priorAutomation = package.getChildFile("automation/generation-00000001.json").loadFileAsString();
        juce::ChildProcess child;
        const auto started = child.start(
            juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
              "--save-interruption=" + juce::String(checkpoint),
              "--save-package=" + package.getFullPathName() });
        const auto exited = started && child.waitForProcessToFinish(10000);
        if (started && !exited)
        {
            expect(child.kill(), "A timed-out save worker can be terminated.");
            child.waitForProcessToFinish(5000);
        }
        expect(exited && child.getExitCode() == 74,
               ("The real save process terminates abruptly at " + juce::String(checkpoint)
                + ": " + child.readAllProcessOutput()).toRawUTF8());
        if (!exited)
            continue;
        juce::String error;
        const auto saved = studio::ProjectFile::load(package, error);
        const auto committed = juce::String(checkpoint) == "manifest-published"
            || juce::String(checkpoint).startsWith("recovery-");
        expect(saved && saved->name == (committed ? "Interrupted synthetic edit" : "Prior valid save")
                   && package.getChildFile("session/generation-00000001.json").loadFileAsString() == priorSession
                   && package.getChildFile("automation/generation-00000001.json").loadFileAsString() == priorAutomation,
               "Abrupt process exit leaves the exact prior save or the complete new manifest-selected generation.");
        error.clear();
        auto opened = studio::ProjectFile::loadForOpen(package, error);
        expect(opened && opened->project.name == "Interrupted synthetic edit",
               ("The latest complete edit is recoverable after " + juce::String(checkpoint) + ": " + error).toRawUTF8());
        if (!opened)
            continue;
        std::map<juce::String, juce::String> immutableFiles;
        for (const auto* subdirectory : { "session", "automation" })
            for (const auto& file : package.getChildFile(subdirectory)
                                       .findChildFiles(juce::File::findFiles, false, "generation-*.json"))
                immutableFiles.emplace(file.getFullPathName(), file.loadFileAsString());
        opened->project.name = "Saved after interruption";
        expect(studio::ProjectFile::save(opened->project, package).wasOk(),
               "A recovered project can save after an interrupted transaction.");
        for (const auto& [path, bytes] : immutableFiles)
            expect(juce::File(path).loadFileAsString() == bytes,
                   "The next save never overwrites an orphaned immutable generation.");
    }

    release_fixture::Directory directory;
    const auto package = directory.file.getChildFile("concurrent.studioduo");
    const auto project = studio::Project::createDefault();
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "The simultaneous-save fixture can be created.");
    const auto manifestBytes = package.getChildFile("manifest.json").loadFileAsString();
    juce::ChildProcess child;
    const auto started = child.start(juce::StringArray {
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
        "--save-interruption=hold", "--save-package=" + package.getFullPathName()
    });
    const auto readyFile = package.getSiblingFile("save-lock.ready");
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (started && child.isRunning() && !readyFile.existsAsFile()
           && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::Thread::sleep(1);
    expect(started && readyFile.existsAsFile(),
           "A real save worker can pause with its transaction in progress.");
    if (readyFile.existsAsFile())
    {
        const auto result = studio::ProjectFile::save(project, package);
        expect(result.failed() && result.getErrorMessage().containsIgnoreCase("saving")
                   && package.getChildFile("manifest.json").loadFileAsString() == manifestBytes,
               "A simultaneous writer cannot alter a package owned by another save transaction.");
    }
    if (started && child.isRunning())
        expect(child.kill(), "The paused save worker can be terminated by its owner.");
    expect(!started || child.waitForProcessToFinish(5000),
           "The terminated save worker releases its native resources.");
    expect(studio::ProjectFile::save(project, package).wasOk(),
           "An abruptly terminated writer releases the package save lock.");
}

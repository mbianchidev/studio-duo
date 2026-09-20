#include "ProjectFile.h"

namespace studio
{
namespace
{
constexpr auto manifestName = "manifest.json";
constexpr auto reducedIsolationMarkerName = "in-process-active.json";
constexpr auto projectDirectoryToken = "${PROJECT_DIR}/";

struct ManifestState
{
    int generation = 0;
    juce::String projectId;
    juce::String savedAt;
};

struct RecoveryPoint
{
    Project project;
    int baseGeneration = 0;
    bool hasBaseGeneration = false;
    juce::String writtenAt;
};

bool transformPackagePaths(
    juce::var& value,
    const juce::File& packageDirectory,
    bool encode,
    bool pathBearing,
    juce::String& error)
{
    if (value.isArray())
    {
        for (auto& child : *value.getArray())
            if (!transformPackagePaths(
                    child,
                    packageDirectory,
                    encode,
                    pathBearing,
                    error))
                return false;
        return true;
    }

    if (auto* object = value.getDynamicObject())
    {
        auto& properties = object->getProperties();
        for (int index = 0; index < properties.size(); ++index)
        {
            const auto name = properties.getName(index);
            auto child = properties.getValueAt(index).clone();
            const auto childIsPath =
                name == juce::Identifier("sourceFile")
                || name == juce::Identifier("file")
                || name == juce::Identifier("renderFile");
            if (!transformPackagePaths(
                    child,
                    packageDirectory,
                    encode,
                    childIsPath,
                    error))
                return false;
            properties.set(name, child);
        }
        return true;
    }
    if (!value.isString() || !pathBearing)
        return true;

    const auto text = value.toString();
    if (encode)
    {
        if (!juce::File::isAbsolutePath(text))
            return true;
        const juce::File file(text);
        if (!file.isAChildOf(packageDirectory))
            return true;
        value = juce::String(projectDirectoryToken)
            + file.getRelativePathFrom(packageDirectory)
                  .replaceCharacter('\\', '/');
        return true;
    }
    if (!text.startsWith(projectDirectoryToken))
        return true;
    const auto relative =
        text.substring(juce::String(projectDirectoryToken).length());
    if (relative.isEmpty()
        || relative.contains("..")
        || juce::File::isAbsolutePath(relative))
    {
        error = "The project contains an unsafe package-relative path.";
        return false;
    }
    const auto resolved = packageDirectory.getChildFile(relative);
    if (!resolved.isAChildOf(packageDirectory))
    {
        error = "The project package-relative path escapes the package.";
        return false;
    }
    value = resolved.getFullPathName();
    return true;
}

bool resolvePackageRelativeToneRenders(
    Project& project,
    const juce::File& packageDirectory,
    juce::String& error)
{
    for (auto& snapshot : project.toneSnapshots)
    {
        if (snapshot.renderFile.isEmpty()
            || juce::File::isAbsolutePath(snapshot.renderFile))
            continue;
        if (snapshot.renderFile.contains(".."))
        {
            error = "A tone snapshot contains an unsafe render path.";
            return false;
        }
        const auto resolved =
            packageDirectory.getChildFile(snapshot.renderFile);
        if (!resolved.isAChildOf(packageDirectory))
        {
            error = "A tone snapshot render path escapes the project package.";
            return false;
        }
        snapshot.renderFile = resolved.getFullPathName();
    }
    return true;
}

int nextGeneration(const juce::File& manifest)
{
    if (!manifest.existsAsFile())
        return 1;

    const auto parsed = juce::JSON::parse(manifest.loadFileAsString());
    if (const auto* object = parsed.getDynamicObject())
        return juce::jmax(1, static_cast<int>(object->getProperty("generation")) + 1);

    return 1;
}

std::optional<ManifestState> readManifestState(
    const juce::File& packageDirectory,
    juce::String& error)
{
    const auto manifestFile = packageDirectory.getChildFile(manifestName);
    if (!manifestFile.existsAsFile())
    {
        error = "The selected package does not contain manifest.json.";
        return std::nullopt;
    }

    const auto parsed = juce::JSON::parse(manifestFile.loadFileAsString());
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr)
    {
        error = "The project manifest is not valid JSON.";
        return std::nullopt;
    }

    ManifestState state;
    state.generation = static_cast<int>(object->getProperty("generation"));
    state.projectId = object->getProperty("projectId").toString();
    state.savedAt = object->getProperty("savedAt").toString();
    return state;
}

std::optional<RecoveryPoint> readRecoveryPoint(
    const juce::File& packageDirectory,
    juce::String& error)
{
    const auto recoveryFile = packageDirectory
        .getChildFile("recovery")
        .getChildFile("latest.json");
    if (!recoveryFile.existsAsFile())
    {
        error = "The project does not contain a recovery point.";
        return std::nullopt;
    }

    const auto parsed = juce::JSON::parse(recoveryFile.loadFileAsString());
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr)
    {
        error = "The recovery point is not valid JSON.";
        return std::nullopt;
    }

    auto projectValue = object->getProperty("project").clone();
    juce::String projectError;
    if (!transformPackagePaths(
            projectValue,
            packageDirectory,
            false,
            false,
            projectError))
    {
        error = projectError;
        return std::nullopt;
    }
    auto project = Project::fromVar(projectValue, projectError);
    if (!project.has_value())
    {
        error = "The recovery point contains an invalid project: "
            + projectError;
        return std::nullopt;
    }
    if (!resolvePackageRelativeToneRenders(
            *project,
            packageDirectory,
            projectError))
    {
        error = projectError;
        return std::nullopt;
    }

    RecoveryPoint point;
    point.project = std::move(*project);
    point.hasBaseGeneration = object->hasProperty("baseGeneration");
    point.baseGeneration = static_cast<int>(
        object->getProperty("baseGeneration"));
    point.writtenAt = object->getProperty("writtenAt").toString();
    if (point.hasBaseGeneration && point.baseGeneration < 0)
    {
        error = "The recovery point contains an invalid base generation.";
        return std::nullopt;
    }
    return point;
}

bool sameProject(const Project& left, const Project& right)
{
    return juce::JSON::toString(left.toVar(), false)
        == juce::JSON::toString(right.toVar(), false);
}
}

juce::File ProjectFile::normalisePackagePath(const juce::File& requestedPath)
{
    if (requestedPath.hasFileExtension("studioduo"))
        return requestedPath;

    return requestedPath.getSiblingFile(requestedPath.getFileName() + ".studioduo");
}

juce::Result ProjectFile::save(const Project& project, const juce::File& requestedPackageDirectory)
{
    juce::String validationError;
    if (!Project::fromVar(project.toVar(), validationError).has_value())
        return juce::Result::fail(
            "Project validation failed: " + validationError);

    const auto packageDirectory = normalisePackagePath(requestedPackageDirectory);
    if (!packageDirectory.createDirectory())
        return juce::Result::fail("Could not create project package: " + packageDirectory.getFullPathName());

    const auto sessionDirectory = packageDirectory.getChildFile("session");
    const auto automationDirectory = packageDirectory.getChildFile("automation");
    const auto pluginStateDirectory = packageDirectory.getChildFile("plugin-state");
    const auto mediaDirectory = packageDirectory.getChildFile("media");
    const auto analysisDirectory = packageDirectory.getChildFile("analysis");
    const auto recoveryDirectory = packageDirectory.getChildFile("recovery");

    for (const auto& directory : { sessionDirectory,
                                   automationDirectory,
                                   pluginStateDirectory,
                                   mediaDirectory,
                                   analysisDirectory,
                                   recoveryDirectory })
        if (!directory.createDirectory())
            return juce::Result::fail("Could not create project directory: " + directory.getFullPathName());

    const auto manifestFile = packageDirectory.getChildFile(manifestName);
    const auto generation = nextGeneration(manifestFile);
    const auto generationName = "generation-" + juce::String(generation).paddedLeft('0', 8) + ".json";
    const auto sessionFile = sessionDirectory.getChildFile(generationName);
    const auto automationFile = automationDirectory.getChildFile(
        generationName);

    auto sessionValue = project.toVar();
    if (auto* session = sessionValue.getDynamicObject())
        session->removeProperty("automationLanes");
    if (!transformPackagePaths(
            sessionValue,
            packageDirectory,
            true,
            false,
            validationError))
        return juce::Result::fail(validationError);
    if (const auto result = writeJsonAtomically(sessionFile, sessionValue);
        result.failed())
        return result;
    auto automation = std::make_unique<juce::DynamicObject>();
    automation->setProperty("schemaVersion", 1);
    automation->setProperty("projectId", project.id);
    juce::Array<juce::var> laneValues;
    for (const auto& lane : project.automationLanes)
        laneValues.add(lane.toVar());
    automation->setProperty("lanes", juce::var(laneValues));
    if (const auto result = writeJsonAtomically(
            automationFile,
            juce::var(automation.release()));
        result.failed())
        return result;

    auto manifest = std::make_unique<juce::DynamicObject>();
    manifest->setProperty("formatVersion", Project::currentFormatVersion);
    manifest->setProperty("applicationVersion", STUDIO_DUO_VERSION);
    manifest->setProperty("projectId", project.id);
    manifest->setProperty("projectName", project.name);
    manifest->setProperty("generation", generation);
    manifest->setProperty("activeSession", "session/" + generationName);
    manifest->setProperty("activeAutomation",
                          "automation/" + generationName);
    manifest->setProperty(
        "requiredCapabilities",
        juce::var(juce::Array<juce::var> {
            "routingGraphV3",
            "automationV1",
            "sandboxedPluginStateV1",
            "clapHostV1",
            "araCompatibilityV1",
            "bundledDevicesV1",
            "bundledCompositionDevicesV1",
            "toneSnapshotsV1",
            "renderReportsV2",
            "midiCompositionV1",
            "midiChannelPressureV1",
            "scenesV1",
            "compatibilityReportsV1",
            "dawprojectV1",
            "masteringAlbumV1",
            "sectionTransportV1"
        }));
    manifest->setProperty("savedAt", juce::Time::getCurrentTime().toISO8601(true));

    if (const auto result = writeJsonAtomically(manifestFile, juce::var(manifest.release())); result.failed())
        return result;

    return writeRecoveryPoint(project, packageDirectory);
}

std::optional<Project> ProjectFile::load(const juce::File& requestedPackageDirectory, juce::String& error)
{
    const auto packageDirectory = normalisePackagePath(requestedPackageDirectory);
    const auto manifestFile = packageDirectory.getChildFile(manifestName);
    if (!manifestFile.existsAsFile())
    {
        error = "The selected package does not contain manifest.json.";
        return std::nullopt;
    }

    const auto manifestValue = juce::JSON::parse(manifestFile.loadFileAsString());
    const auto* manifest = manifestValue.getDynamicObject();
    if (manifest == nullptr)
    {
        error = "The project manifest is not valid JSON.";
        return std::nullopt;
    }

    const auto activeSession = manifest->getProperty("activeSession").toString();
    if (activeSession.isEmpty() || activeSession.contains("..") || juce::File::isAbsolutePath(activeSession))
    {
        error = "The project manifest contains an unsafe session path.";
        return std::nullopt;
    }

    const auto sessionFile = packageDirectory.getChildFile(activeSession);
    if (!sessionFile.isAChildOf(packageDirectory) || !sessionFile.existsAsFile())
    {
        error = "The active project session is missing.";
        return std::nullopt;
    }

    auto sessionValue = juce::JSON::parse(sessionFile.loadFileAsString());
    if (sessionValue.isVoid())
    {
        error = "The active project session is not valid JSON.";
        return std::nullopt;
    }
    const auto manifestVersion = static_cast<int>(
        manifest->getProperty("formatVersion"));
    const auto* sessionObject = sessionValue.getDynamicObject();
    if (manifestVersion < 1
        || manifestVersion > Project::currentFormatVersion
        || sessionObject == nullptr
        || static_cast<int>(
               sessionObject->getProperty("formatVersion"))
            != manifestVersion)
    {
        error = "The manifest and session format versions do not agree.";
        return std::nullopt;
    }
    if (manifestVersion >= 3
        && !manifest->getProperty("requiredCapabilities").isArray())
    {
        error = "The project manifest does not declare required capabilities.";
        return std::nullopt;
    }

    const auto activeAutomation =
        manifest->getProperty("activeAutomation").toString();
    if (activeAutomation.isNotEmpty())
    {
        if (activeAutomation.contains("..")
            || juce::File::isAbsolutePath(activeAutomation))
        {
            error = "The project manifest contains an unsafe automation path.";
            return std::nullopt;
        }
        const auto automationFile =
            packageDirectory.getChildFile(activeAutomation);
        if (!automationFile.isAChildOf(packageDirectory)
            || !automationFile.existsAsFile())
        {
            error = "The active project automation is missing.";
            return std::nullopt;
        }
        const auto automationValue = juce::JSON::parse(
            automationFile.loadFileAsString());
        const auto* automation = automationValue.getDynamicObject();
        auto* session = sessionValue.getDynamicObject();
        if (automation == nullptr
            || session == nullptr
            || static_cast<int>(
                   automation->getProperty("schemaVersion")) != 1
            || automation->getProperty("projectId").toString()
                != session->getProperty("id").toString()
            || !automation->getProperty("lanes").isArray())
        {
            error = "The active automation document is invalid.";
            return std::nullopt;
        }
        session->setProperty("automationLanes",
                             automation->getProperty("lanes"));
    }

    if (!transformPackagePaths(
            sessionValue,
            packageDirectory,
            false,
            false,
            error))
        return std::nullopt;
    auto project = Project::fromVar(sessionValue, error);
    if (!project.has_value())
        return std::nullopt;
    if (!resolvePackageRelativeToneRenders(
            *project,
            packageDirectory,
            error))
        return std::nullopt;
    return project;
}

std::optional<ProjectOpenResult> ProjectFile::loadForOpen(
    const juce::File& requestedPackageDirectory,
    juce::String& error)
{
    const auto packageDirectory =
        normalisePackagePath(requestedPackageDirectory);
    juce::String savedError;
    auto savedProject = load(packageDirectory, savedError);

    const auto recoveryFile = packageDirectory
        .getChildFile("recovery")
        .getChildFile("latest.json");
    if (!recoveryFile.existsAsFile())
    {
        if (!savedProject.has_value())
        {
            error = savedError;
            return std::nullopt;
        }
        return ProjectOpenResult {
            std::move(*savedProject),
            false,
            {}
        };
    }

    juce::String recoveryError;
    auto recoveryPoint = readRecoveryPoint(packageDirectory,
                                           recoveryError);
    if (!recoveryPoint.has_value())
    {
        if (!savedProject.has_value())
        {
            error = savedError
                + " Recovery failed: "
                + recoveryError;
            return std::nullopt;
        }
        return ProjectOpenResult {
            std::move(*savedProject),
            false,
            "The recovery point was ignored: " + recoveryError
        };
    }

    juce::String manifestError;
    const auto manifest = readManifestState(packageDirectory,
                                            manifestError);
    const auto expectedProjectId = savedProject.has_value()
        ? savedProject->id
        : manifest.has_value()
            ? manifest->projectId
            : juce::String();
    if (expectedProjectId.isNotEmpty()
        && recoveryPoint->project.id != expectedProjectId)
    {
        if (!savedProject.has_value())
        {
            error = savedError
                + " Recovery failed: the recovery point belongs to a different project ID.";
            return std::nullopt;
        }
        return ProjectOpenResult {
            std::move(*savedProject),
            false,
            "The recovery point was ignored because it belongs to a different project ID."
        };
    }

    if (!savedProject.has_value())
    {
        return ProjectOpenResult {
            std::move(recoveryPoint->project),
            true,
            "The saved project could not be opened: " + savedError
        };
    }

    if (sameProject(*savedProject, recoveryPoint->project))
        return ProjectOpenResult {
            std::move(*savedProject),
            false,
            {}
        };

    if (!manifest.has_value())
    {
        return ProjectOpenResult {
            std::move(recoveryPoint->project),
            true,
            "The saved project metadata could not be read: "
                + manifestError
        };
    }

    auto recoveryIsCurrent = true;
    if (recoveryPoint->hasBaseGeneration)
    {
        recoveryIsCurrent =
            recoveryPoint->baseGeneration == manifest->generation;
    }
    else if (recoveryPoint->writtenAt.isNotEmpty()
             && manifest->savedAt.isNotEmpty())
    {
        recoveryIsCurrent =
            recoveryPoint->writtenAt >= manifest->savedAt;
    }

    if (!recoveryIsCurrent)
    {
        return ProjectOpenResult {
            std::move(*savedProject),
            false,
            "A stale recovery point was ignored."
        };
    }

    return ProjectOpenResult {
        std::move(recoveryPoint->project),
        true,
        {}
    };
}

juce::Result ProjectFile::writeRecoveryPoint(const Project& project, const juce::File& requestedPackageDirectory)
{
    const auto packageDirectory =
        normalisePackagePath(requestedPackageDirectory);
    const auto recoveryDirectory =
        packageDirectory.getChildFile("recovery");
    if (!recoveryDirectory.createDirectory())
        return juce::Result::fail("Could not create the recovery directory.");

    auto baseGeneration = 0;
    juce::String manifestError;
    if (const auto manifest = readManifestState(packageDirectory,
                                                manifestError);
        manifest.has_value())
    {
        baseGeneration = manifest->generation;
    }

    auto recovery = std::make_unique<juce::DynamicObject>();
    recovery->setProperty("schemaVersion", 1);
    recovery->setProperty("baseGeneration", baseGeneration);
    recovery->setProperty("writtenAt", juce::Time::getCurrentTime().toISO8601(true));
    auto projectValue = project.toVar();
    juce::String pathError;
    if (!transformPackagePaths(
            projectValue,
            packageDirectory,
            true,
            false,
            pathError))
        return juce::Result::fail(pathError);
    recovery->setProperty("project", projectValue);
    return writeJsonAtomically(recoveryDirectory.getChildFile("latest.json"), juce::var(recovery.release()));
}

juce::Result ProjectFile::writeReducedIsolationMarker(
    const juce::File& requestedPackageDirectory,
    const std::vector<juce::String>& insertIds)
{
    const auto recoveryDirectory =
        normalisePackagePath(requestedPackageDirectory)
            .getChildFile("recovery");
    if (!recoveryDirectory.createDirectory())
        return juce::Result::fail(
            "Could not create the recovery directory.");

    juce::Array<juce::var> values;
    for (const auto& insertId : insertIds)
        if (insertId.isNotEmpty())
            values.add(insertId);
    auto marker = std::make_unique<juce::DynamicObject>();
    marker->setProperty("writtenAt",
                        juce::Time::getCurrentTime().toISO8601(true));
    marker->setProperty("insertIds", juce::var(values));
    return writeJsonAtomically(
        recoveryDirectory.getChildFile(reducedIsolationMarkerName),
        juce::var(marker.release()));
}

std::vector<juce::String> ProjectFile::reducedIsolationMarker(
    const juce::File& requestedPackageDirectory)
{
    const auto marker =
        normalisePackagePath(requestedPackageDirectory)
            .getChildFile("recovery")
            .getChildFile(reducedIsolationMarkerName);
    if (!marker.existsAsFile())
        return {};
    const auto parsed = juce::JSON::parse(marker.loadFileAsString());
    const auto* object = parsed.getDynamicObject();
    if (object == nullptr || !object->getProperty("insertIds").isArray())
        return {};
    std::vector<juce::String> result;
    for (const auto& value : *object->getProperty("insertIds").getArray())
        if (value.toString().isNotEmpty())
            result.push_back(value.toString());
    return result;
}

juce::Result ProjectFile::clearReducedIsolationMarker(
    const juce::File& requestedPackageDirectory)
{
    const auto marker =
        normalisePackagePath(requestedPackageDirectory)
            .getChildFile("recovery")
            .getChildFile(reducedIsolationMarkerName);
    if (!marker.existsAsFile() || marker.deleteFile())
        return juce::Result::ok();
    return juce::Result::fail(
        "Could not clear the reduced-isolation recovery marker.");
}

juce::Result ProjectFile::writeJsonAtomically(const juce::File& destination, const juce::var& value)
{
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create " + destination.getParentDirectory().getFullPathName());

    const auto temporary = destination.getSiblingFile(destination.getFileName()
                                                       + ".tmp-"
                                                       + juce::Uuid().toString());
    {
        auto stream = temporary.createOutputStream();
        if (stream == nullptr)
            return juce::Result::fail("Could not open a temporary project file for writing.");

        stream->writeText(juce::JSON::toString(value, true), false, false, "\n");
        stream->flush();
        if (stream->getStatus().failed())
        {
            temporary.deleteFile();
            return stream->getStatus();
        }
    }

    const auto replaced = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);

    if (!replaced)
    {
        temporary.deleteFile();
        return juce::Result::fail("Could not atomically replace " + destination.getFullPathName());
    }

    return juce::Result::ok();
}
}

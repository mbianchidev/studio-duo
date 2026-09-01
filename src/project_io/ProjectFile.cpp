#include "ProjectFile.h"

namespace studio
{
namespace
{
constexpr auto manifestName = "manifest.json";
constexpr auto reducedIsolationMarkerName = "in-process-active.json";

int nextGeneration(const juce::File& manifest)
{
    if (!manifest.existsAsFile())
        return 1;

    const auto parsed = juce::JSON::parse(manifest.loadFileAsString());
    if (const auto* object = parsed.getDynamicObject())
        return juce::jmax(1, static_cast<int>(object->getProperty("generation")) + 1);

    return 1;
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

    if (const auto result = writeJsonAtomically(sessionFile, project.toVar()); result.failed())
        return result;

    auto manifest = std::make_unique<juce::DynamicObject>();
    manifest->setProperty("formatVersion", Project::currentFormatVersion);
    manifest->setProperty("applicationVersion", STUDIO_DUO_VERSION);
    manifest->setProperty("projectId", project.id);
    manifest->setProperty("projectName", project.name);
    manifest->setProperty("generation", generation);
    manifest->setProperty("activeSession", "session/" + generationName);
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

    const auto sessionValue = juce::JSON::parse(sessionFile.loadFileAsString());
    if (sessionValue.isVoid())
    {
        error = "The active project session is not valid JSON.";
        return std::nullopt;
    }

    return Project::fromVar(sessionValue, error);
}

juce::Result ProjectFile::writeRecoveryPoint(const Project& project, const juce::File& requestedPackageDirectory)
{
    const auto recoveryDirectory = normalisePackagePath(requestedPackageDirectory).getChildFile("recovery");
    if (!recoveryDirectory.createDirectory())
        return juce::Result::fail("Could not create the recovery directory.");

    auto recovery = std::make_unique<juce::DynamicObject>();
    recovery->setProperty("writtenAt", juce::Time::getCurrentTime().toISO8601(true));
    recovery->setProperty("project", project.toVar());
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

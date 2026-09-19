#include "ProjectCollectionService.h"

#include "ProjectFile.h"
#include "plugin_host/PluginStateStore.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>

namespace studio
{
namespace
{
using ResourceCallback = std::function<void(
    const juce::String&,
    juce::File&,
    juce::String&)>;
using PluginStateCallback = std::function<void(
    const juce::String&,
    const juce::String&,
    const juce::String&)>;

void forEachResource(Project& project,
                     const ResourceCallback& callback)
{
    for (std::size_t trackIndex = 0;
         trackIndex < project.tracks.size();
         ++trackIndex)
    {
        auto& track = project.tracks[trackIndex];
        for (std::size_t clipIndex = 0;
             clipIndex < track.clips.size();
             ++clipIndex)
        {
            auto& clip = track.clips[clipIndex];
            callback(
                "tracks[" + juce::String(trackIndex)
                    + "].clips[" + juce::String(clipIndex) + "]",
                clip.sourceFile,
                clip.sourceHash);
        }
    }
    for (std::size_t sceneIndex = 0;
         sceneIndex < project.scenes.size();
         ++sceneIndex)
    {
        auto& scene = project.scenes[sceneIndex];
        for (std::size_t slotIndex = 0;
             slotIndex < scene.slots.size();
             ++slotIndex)
        {
            auto& slot = scene.slots[slotIndex];
            if (!slot.audioClip.has_value())
                continue;
            callback(
                "scenes[" + juce::String(sceneIndex)
                    + "].slots[" + juce::String(slotIndex)
                    + "].audioClip",
                slot.audioClip->sourceFile,
                slot.audioClip->sourceHash);
        }
    }
    for (std::size_t trackIndex = 0;
         trackIndex < project.mastering.tracks.size();
         ++trackIndex)
    {
        auto& track = project.mastering.tracks[trackIndex];
        for (std::size_t sourceIndex = 0;
             sourceIndex < track.sources.size();
             ++sourceIndex)
        {
            auto& source = track.sources[sourceIndex];
            callback(
                "mastering.tracks[" + juce::String(trackIndex)
                    + "].sources[" + juce::String(sourceIndex)
                    + "]",
                source.file,
                source.sourceHash);
        }
    }
    for (std::size_t index = 0;
         index < project.mastering.references.size();
         ++index)
    {
        auto& reference = project.mastering.references[index];
        callback(
            "mastering.references[" + juce::String(index) + "]",
            reference.file,
            reference.sourceHash);
    }
    for (std::size_t index = 0;
         index < project.toneSnapshots.size();
         ++index)
    {
        auto& snapshot = project.toneSnapshots[index];
        if (snapshot.renderFile.isEmpty())
            continue;
        juce::File file(snapshot.renderFile);
        callback(
            "toneSnapshots[" + juce::String(index) + "].renderFile",
            file,
            snapshot.renderHash);
        snapshot.renderFile = file.getFullPathName();
    }
}

void forEachPluginState(
    const Project& project,
    const PluginStateCallback& callback)
{
    for (std::size_t trackIndex = 0;
         trackIndex < project.tracks.size();
         ++trackIndex)
    {
        const auto& track = project.tracks[trackIndex];
        for (std::size_t insertIndex = 0;
             insertIndex < track.inserts.size();
             ++insertIndex)
        {
            const auto& insert = track.inserts[insertIndex];
            if (insert.stateFile.isNotEmpty())
                callback(
                    "tracks[" + juce::String(trackIndex)
                        + "].inserts[" + juce::String(insertIndex)
                        + "]",
                    insert.stateFile,
                    insert.stateHash);
        }
    }
    for (std::size_t snapshotIndex = 0;
         snapshotIndex < project.toneSnapshots.size();
         ++snapshotIndex)
    {
        const auto& snapshot = project.toneSnapshots[snapshotIndex];
        for (std::size_t insertIndex = 0;
             insertIndex < snapshot.inserts.size();
             ++insertIndex)
        {
            const auto& insert = snapshot.inserts[insertIndex];
            if (insert.stateFile.isNotEmpty())
                callback(
                    "toneSnapshots[" + juce::String(snapshotIndex)
                        + "].inserts[" + juce::String(insertIndex)
                        + "]",
                    insert.stateFile,
                    insert.stateHash);
        }
    }
    for (std::size_t snapshotIndex = 0;
         snapshotIndex < project.mixerSnapshots.size();
         ++snapshotIndex)
    {
        const auto& snapshot = project.mixerSnapshots[snapshotIndex];
        for (std::size_t trackIndex = 0;
             trackIndex < snapshot.tracks.size();
             ++trackIndex)
        {
            const auto& track = snapshot.tracks[trackIndex];
            for (std::size_t insertIndex = 0;
                 insertIndex < track.inserts.size();
                 ++insertIndex)
            {
                const auto& insert = track.inserts[insertIndex];
                if (insert.stateFile.isNotEmpty())
                    callback(
                        "mixerSnapshots["
                            + juce::String(snapshotIndex)
                            + "].tracks[" + juce::String(trackIndex)
                            + "].inserts[" + juce::String(insertIndex)
                            + "]",
                        insert.stateFile,
                        insert.stateHash);
            }
        }
    }
}

void addIssue(ProjectCollectionReport& report,
              const juce::String& objectPath,
              const juce::File& source,
              const juce::String& hash,
              const juce::String& message)
{
    report.issues.push_back({
        objectPath,
        source.getFullPathName(),
        hash,
        message
    });
}

bool collectPluginStates(
    const Project& project,
    const juce::File& sourcePackage,
    const juce::File& destinationPackage,
    ProjectCollectionReport& report)
{
    forEachPluginState(
        project,
        [&](const juce::String& objectPath,
            const juce::String& stateFile,
            const juce::String& stateHash)
        {
            const auto safePath = stateFile.isNotEmpty()
                && !stateFile.contains("..")
                && !juce::File::isAbsolutePath(stateFile);
            const auto source =
                sourcePackage.getChildFile(stateFile);
            if (!safePath
                || !sourcePackage.isDirectory()
                || !source.isAChildOf(sourcePackage)
                || !source.existsAsFile())
            {
                ++report.missingResources;
                addIssue(
                    report,
                    objectPath,
                    source,
                    stateHash,
                    "Plugin state is missing.");
                return;
            }
            juce::MemoryBlock state;
            juce::String stateError;
            const PluginStateReference reference {
                stateFile,
                stateHash
            };
            if (!PluginStateStore::load(
                    sourcePackage,
                    reference,
                    state,
                    stateError))
            {
                ++report.invalidResources;
                addIssue(
                    report,
                    objectPath,
                    source,
                    stateHash,
                    "Plugin state failed validation: " + stateError);
                return;
            }
            if (!PluginStateStore::materialize(
                    sourcePackage,
                    destinationPackage,
                    reference,
                    stateError))
            {
                ++report.invalidResources;
                addIssue(
                    report,
                    objectPath,
                    source,
                    stateHash,
                    "Plugin state could not be collected: " + stateError);
                return;
            }
            ++report.copiedResources;
        });
    return report.missingResources == 0
        && report.invalidResources == 0;
}

juce::var resourceManifest(const Project& project,
                           const juce::File& package)
{
    auto copy = project;
    juce::Array<juce::var> resources;
    const auto addEntry = [&resources, &package](
                              const juce::String& objectPath,
                              const juce::File& file,
                              const juce::String& referenceHash)
    {
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty("objectPath", objectPath);
        object->setProperty(
            "path",
            file.getRelativePathFrom(package)
                .replaceCharacter('\\', '/'));
        object->setProperty(
            "sha256",
            juce::SHA256(file).toHexString());
        object->setProperty("referenceHash", referenceHash);
        object->setProperty("size", file.getSize());
        resources.add(juce::var(object.release()));
    };
    forEachResource(
        copy,
        [&addEntry](
            const juce::String& objectPath,
            juce::File& file,
            juce::String& hash)
        {
            addEntry(objectPath, file, hash);
        });
    forEachPluginState(
        project,
        [&addEntry, &package](
            const juce::String& objectPath,
            const juce::String& stateFile,
            const juce::String& stateHash)
        {
            const auto file = package.getChildFile(stateFile);
            jassert(file.existsAsFile() && file.isAChildOf(package));
            addEntry(objectPath, file, stateHash);
        });
    auto manifest = std::make_unique<juce::DynamicObject>();
    manifest->setProperty("schemaVersion", 1);
    manifest->setProperty("projectId", project.id);
    manifest->setProperty(
        "createdAt",
        juce::Time::getCurrentTime().toISO8601(true));
    manifest->setProperty("resources", juce::var(resources));
    return juce::var(manifest.release());
}

bool writeReport(const ProjectCollectionReport& report,
                 const juce::File& file)
{
    return file.getParentDirectory().createDirectory()
        && file.replaceWithText(
            juce::JSON::toString(report.toVar(), true),
            false,
            false,
            "\n");
}
}

juce::var ProjectResourceIssue::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("objectPath", objectPath);
    object->setProperty("sourcePath", sourcePath);
    object->setProperty("expectedHash", expectedHash);
    object->setProperty("message", message);
    return juce::var(object.release());
}

juce::var ProjectCollectionReport::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("copiedResources", copiedResources);
    object->setProperty("missingResources", missingResources);
    object->setProperty("invalidResources", invalidResources);
    object->setProperty("repairedResources", repairedResources);
    juce::Array<juce::var> issueValues;
    for (const auto& issue : issues)
        issueValues.add(issue.toVar());
    object->setProperty("issues", juce::var(issueValues));
    return juce::var(object.release());
}

std::optional<ProjectCollectionReport>
ProjectCollectionService::savePortableCopy(
    const Project& project,
    const juce::File& sourcePackage,
    const juce::File& requestedDestinationPackage,
    juce::String& error)
{
    const auto destinationPackage =
        ProjectFile::normalisePackagePath(
            requestedDestinationPackage);
    if (destinationPackage.exists()
        && destinationPackage.getNumberOfChildFiles(
               juce::File::findFilesAndDirectories)
            > 0)
    {
        error = "Portable-copy destination must be empty.";
        return std::nullopt;
    }
    const auto stagingPackage =
        destinationPackage.getParentDirectory()
            .getNonexistentChildFile(
                destinationPackage.getFileNameWithoutExtension()
                    + "-staging",
                ".studioduo",
                false);
    if (!stagingPackage.createDirectory())
    {
        error = "Could not create the portable-copy staging package.";
        return std::nullopt;
    }
    const auto mediaDirectory =
        stagingPackage.getChildFile("media");
    if (!mediaDirectory.createDirectory())
    {
        error = "Could not create the portable media directory.";
        return std::nullopt;
    }

    auto portable = project;
    ProjectCollectionReport report;
    for (std::size_t index = 0;
         index < portable.toneSnapshots.size();
         ++index)
    {
        auto& snapshot = portable.toneSnapshots[index];
        if (snapshot.renderFile.isEmpty()
            || juce::File::isAbsolutePath(snapshot.renderFile))
            continue;
        const auto safePath = !snapshot.renderFile.contains("..")
            && sourcePackage.isDirectory();
        const auto resolved =
            sourcePackage.getChildFile(snapshot.renderFile);
        if (!safePath || !resolved.isAChildOf(sourcePackage))
        {
            ++report.missingResources;
            addIssue(
                report,
                "toneSnapshots[" + juce::String(index)
                    + "].renderFile",
                resolved,
                snapshot.renderHash,
                "Package-relative tone render cannot be resolved.");
            snapshot.renderFile.clear();
            continue;
        }
        snapshot.renderFile = resolved.getFullPathName();
    }
    forEachResource(
        portable,
        [&report, &mediaDirectory](
            const juce::String& objectPath,
            juce::File& file,
            juce::String& hash)
        {
            if (!file.existsAsFile())
            {
                ++report.missingResources;
                addIssue(
                    report,
                    objectPath,
                    file,
                    hash,
                    "Resource is missing.");
                return;
            }
            const auto actualHash =
                juce::SHA256(file).toHexString();
            if (hash.isNotEmpty() && hash != actualHash)
            {
                ++report.invalidResources;
                addIssue(
                    report,
                    objectPath,
                    file,
                    hash,
                    "Resource hash changed.");
                return;
            }
            const auto extension =
                file.getFileExtension().toLowerCase();
            const auto destination =
                mediaDirectory.getChildFile(
                    actualHash + extension);
            if (!destination.existsAsFile()
                && !file.copyFileTo(destination))
            {
                ++report.invalidResources;
                addIssue(
                    report,
                    objectPath,
                    file,
                    actualHash,
                    "Resource could not be copied.");
                return;
            }
            file = destination;
            hash = actualHash;
            ++report.copiedResources;
        });
    if (!collectPluginStates(
            portable,
            sourcePackage,
            stagingPackage,
            report))
    {
        stagingPackage.deleteRecursively();
        error = "Portable copy stopped because "
            + juce::String(report.missingResources)
            + " resource(s) are missing and "
            + juce::String(report.invalidResources)
            + " resource(s) failed hash validation.";
        return std::nullopt;
    }
    if (report.missingResources > 0 || report.invalidResources > 0)
    {
        stagingPackage.deleteRecursively();
        error = "Portable copy stopped because "
            + juce::String(report.missingResources)
            + " resource(s) are missing and "
            + juce::String(report.invalidResources)
            + " resource(s) failed hash validation.";
        return std::nullopt;
    }
    const auto saveResult = ProjectFile::save(
        portable,
        stagingPackage);
    if (saveResult.failed())
    {
        stagingPackage.deleteRecursively();
        error = saveResult.getErrorMessage();
        return std::nullopt;
    }
    if (!stagingPackage.getChildFile("portable-manifest.json")
             .replaceWithText(
                 juce::JSON::toString(
                     resourceManifest(portable, stagingPackage),
                     true),
                 false,
                 false,
                 "\n")
        || !writeReport(
            report,
            stagingPackage.getChildFile("analysis")
                .getChildFile("portable-copy-report.json")))
    {
        stagingPackage.deleteRecursively();
        error = "Could not write portable-copy verification files.";
        return std::nullopt;
    }
    juce::String validationError;
    const auto validation = validatePortableCopy(
        stagingPackage,
        validationError);
    if (!validation.has_value()
        || validation->invalidResources > 0)
    {
        stagingPackage.deleteRecursively();
        error = "Portable-copy validation failed: "
            + validationError;
        return std::nullopt;
    }
    if (destinationPackage.exists()
        && (destinationPackage.getNumberOfChildFiles(
                juce::File::findFilesAndDirectories)
                != 0
            || !destinationPackage.deleteFile()))
    {
        stagingPackage.deleteRecursively();
        error = "The portable-copy destination changed during collection and was not replaced.";
        return std::nullopt;
    }
    if (!stagingPackage.moveFileTo(destinationPackage))
    {
        stagingPackage.deleteRecursively();
        error = "Could not publish the verified portable copy.";
        return std::nullopt;
    }
    return report;
}

std::optional<ProjectCollectionReport>
ProjectCollectionService::validatePortableCopy(
    const juce::File& requestedPackage,
    juce::String& error)
{
    const auto package =
        ProjectFile::normalisePackagePath(requestedPackage);
    const auto manifestFile =
        package.getChildFile("portable-manifest.json");
    const auto manifest =
        juce::JSON::parse(manifestFile.loadFileAsString());
    const auto* object = manifest.getDynamicObject();
    if (!manifestFile.existsAsFile()
        || object == nullptr
        || static_cast<int>(
               object->getProperty("schemaVersion"))
            != 1
        || !object->getProperty("resources").isArray())
    {
        error = "Portable project manifest is missing or invalid.";
        return std::nullopt;
    }

    ProjectCollectionReport report;
    juce::String loadError;
    const auto loadedProject = ProjectFile::load(package, loadError);
    if (!loadedProject.has_value())
    {
        error = "Portable project cannot be opened: " + loadError;
        return std::nullopt;
    }
    if (object->getProperty("projectId").toString()
        != loadedProject->id)
    {
        ++report.invalidResources;
        report.issues.push_back({
            "project",
            manifestFile.getFullPathName(),
            loadedProject->id,
            "Portable manifest project ID does not match the project."
        });
    }
    std::map<juce::String, juce::String> expectedResources;
    auto loadedCopy = *loadedProject;
    forEachResource(
        loadedCopy,
        [&expectedResources, &package, &report](
            const juce::String& objectPath,
            juce::File& file,
            juce::String& hash)
        {
            if (!file.isAChildOf(package))
            {
                ++report.invalidResources;
                addIssue(
                    report,
                    objectPath,
                    file,
                    {},
                    "Portable project still references external media.");
                return;
            }
            expectedResources[
                objectPath + "|"
                    + file.getRelativePathFrom(package)
                          .replaceCharacter('\\', '/')] = hash;
        });
    forEachPluginState(
        *loadedProject,
        [&expectedResources](
            const juce::String& objectPath,
            const juce::String& stateFile,
            const juce::String& stateHash)
        {
            expectedResources[objectPath + "|" + stateFile] =
                stateHash;
        });
    std::map<juce::String, juce::String> declaredResources;
    for (const auto& resourceValue :
         *object->getProperty("resources").getArray())
    {
        const auto* resource = resourceValue.getDynamicObject();
        if (resource == nullptr)
        {
            ++report.invalidResources;
            continue;
        }
        const auto relative =
            resource->getProperty("path").toString();
        const auto expectedHash =
            resource->getProperty("sha256").toString();
        const auto referenceHash =
            resource->getProperty("referenceHash").toString();
        const auto objectPath =
            resource->getProperty("objectPath").toString();
        const auto file = package.getChildFile(relative);
        const auto identity = objectPath + "|" + relative;
        if (declaredResources.find(identity)
            != declaredResources.end())
        {
            ++report.invalidResources;
            addIssue(
                report,
                objectPath,
                file,
                expectedHash,
                "Portable manifest contains a duplicate resource entry.");
        }
        declaredResources[identity] = referenceHash;
        const auto validPath = relative.isNotEmpty()
            && !relative.contains("..")
            && !juce::File::isAbsolutePath(relative)
            && file.isAChildOf(package);
        if (!validPath
            || !file.existsAsFile()
            || file.getSize()
                != static_cast<juce::int64>(
                    resource->getProperty("size"))
            || juce::SHA256(file).toHexString()
                != expectedHash)
        {
            ++report.invalidResources;
            addIssue(
                report,
                objectPath,
                file,
                expectedHash,
                "Portable resource failed path, size, or hash validation.");
        }
    }
    for (const auto& [identity, expectedHash] : expectedResources)
    {
        const auto declared = declaredResources.find(identity);
        if (declared != declaredResources.end()
            && declared->second == expectedHash)
            continue;
        ++report.invalidResources;
        report.issues.push_back({
            identity.upToFirstOccurrenceOf("|", false, false),
            identity.fromFirstOccurrenceOf("|", false, false),
            expectedHash,
            declared == declaredResources.end()
                ? "Portable manifest omits a referenced resource."
                : "Portable manifest hash does not match the project reference."
        });
    }
    for (const auto& [identity, declaredHash] : declaredResources)
    {
        if (expectedResources.find(identity) != expectedResources.end())
            continue;
        ++report.invalidResources;
        report.issues.push_back({
            identity.upToFirstOccurrenceOf("|", false, false),
            identity.fromFirstOccurrenceOf("|", false, false),
            declaredHash,
            "Portable manifest contains an unreferenced resource."
        });
    }
    return report;
}

ProjectCollectionReport
ProjectCollectionService::repairMissingResources(
    Project& project,
    const std::vector<juce::File>& searchRoots,
    juce::String& error,
    const juce::File& projectPackage)
{
    ProjectCollectionReport report;
    if (projectPackage.isDirectory())
    {
        for (auto& snapshot : project.toneSnapshots)
        {
            if (snapshot.renderFile.isEmpty()
                || juce::File::isAbsolutePath(snapshot.renderFile)
                || snapshot.renderFile.contains(".."))
                continue;
            const auto resolved =
                projectPackage.getChildFile(snapshot.renderFile);
            if (resolved.isAChildOf(projectPackage))
                snapshot.renderFile = resolved.getFullPathName();
        }
    }
    juce::Array<juce::File> candidates;
    for (const auto& root : searchRoots)
    {
        if (!root.isDirectory())
            continue;
        candidates.addArray(root.findChildFiles(
            juce::File::findFiles,
            true));
    }
    std::unordered_map<std::string, juce::String> hashCache;
    forEachResource(
        project,
        [&report, &candidates, &hashCache](
            const juce::String& objectPath,
            juce::File& file,
            juce::String& hash)
        {
            if (file.existsAsFile()
                && (hash.isEmpty()
                    || juce::SHA256(file).toHexString() == hash))
                return;
            const auto originalName = file.getFileName();
            if (hash.isEmpty())
            {
                juce::Array<juce::File> nameMatches;
                for (const auto& candidate : candidates)
                    if (candidate.getFileName() == originalName)
                        nameMatches.add(candidate);
                if (nameMatches.size() == 1)
                {
                    file = nameMatches.getFirst();
                    hash = juce::SHA256(file).toHexString();
                    ++report.repairedResources;
                    return;
                }
                ++report.missingResources;
                addIssue(
                    report,
                    objectPath,
                    file,
                    hash,
                    nameMatches.isEmpty()
                        ? "No matching resource was found."
                        : "Several same-named resources were found; choose a narrower repair folder.");
                return;
            }
            for (const auto& candidate : candidates)
            {
                auto candidateHash = juce::String();
                const auto key =
                    candidate.getFullPathName().toStdString();
                if (const auto cached = hashCache.find(key);
                    cached != hashCache.end())
                {
                    candidateHash = cached->second;
                }
                else
                {
                    candidateHash =
                        juce::SHA256(candidate).toHexString();
                    hashCache.emplace(key, candidateHash);
                }
                if (candidateHash == hash)
                {
                    file = candidate;
                    hash = candidateHash;
                    ++report.repairedResources;
                    return;
                }
            }
            ++report.missingResources;
            addIssue(
                report,
                objectPath,
                file,
                hash,
                "No matching resource was found.");
        });
    if (report.missingResources > 0)
        error = juce::String(report.missingResources)
            + " resource(s) remain missing.";
    else
        error.clear();
    return report;
}
}

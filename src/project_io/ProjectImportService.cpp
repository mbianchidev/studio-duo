#include "ProjectImportService.h"

#include "dawproject_io/DawProjectIO.h"
#include "studio_one_io/StudioOneProjectIO.h"

#include <new>

namespace studio
{
ProjectSourceFormat ProjectImportService::sourceFormat(const juce::File& source)
{
    if (source.hasFileExtension("studioduo"))
        return ProjectSourceFormat::studioDuo;
    if (source.hasFileExtension("song"))
        return ProjectSourceFormat::studioOneSong;
    if (source.hasFileExtension("dawproject"))
        return ProjectSourceFormat::dawProject;
    return ProjectSourceFormat::unsupported;
}

bool ProjectImportService::supportsProjectSource(const juce::File& source)
{
    return sourceFormat(source) != ProjectSourceFormat::unsupported;
}

ProjectImportResult ProjectImportService::importProject(
    const juce::File& source,
    const juce::File& destination,
    bool allowPartialImport)
{
    juce::String error;
    juce::String code = "import.source-format";
    try
    {
        switch (sourceFormat(source))
        {
            case ProjectSourceFormat::studioOneSong:
                return StudioOneProjectIO::importSong(source, destination, allowPartialImport);
            case ProjectSourceFormat::dawProject:
                return DawProjectIO::importProject(source, destination);
            case ProjectSourceFormat::studioDuo:
            case ProjectSourceFormat::unsupported:
                break;
        }
    }
    catch (const std::bad_alloc&)
    {
        code = "import.memory";
        error = "Project import ran out of memory. The current project and original source files were not replaced.";
    }
    ProjectImportResult result;
    result.result = juce::Result::fail(
        error.isNotEmpty() ? error
            : juce::String(
                "Choose a Studio One .song or .dawproject export to import. "
                "Open .studioduo packages directly; Studio One .project mastering albums are not song files."));
    result.report.format = "Project import";
    result.report.operation = "import";
    result.report.source = source.getFullPathName();
    result.report.destination = destination.getFullPathName();
    result.report.createdAt = juce::Time::getCurrentTime().toISO8601(true);
    result.report.issues.push_back({
        CompatibilitySeverity::error, code, "/",
        result.result.getErrorMessage()
    });
    return result;
}
}

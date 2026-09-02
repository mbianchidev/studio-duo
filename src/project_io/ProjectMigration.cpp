#include "ProjectMigration.h"

#include "model/ProjectModel.h"

namespace studio
{
namespace
{
int formatVersion(const juce::DynamicObject& object)
{
    const auto value = object.getProperty("formatVersion");
    return value.isInt() || value.isInt64() || value.isDouble()
        ? static_cast<int>(value)
        : 0;
}

void addEmptyArray(juce::DynamicObject& object, const juce::Identifier& name)
{
    if (object.getProperty(name).isVoid())
        object.setProperty(name, juce::var(juce::Array<juce::var> {}));
}

bool addVersionThreeRouting(juce::DynamicObject& project, juce::String& error)
{
    const auto tracksValue = project.getProperty("tracks");
    if (!tracksValue.isArray())
    {
        error = "Project tracks must be a JSON array before migration.";
        return false;
    }

    juce::String masterId;
    for (const auto& trackValue : *tracksValue.getArray())
    {
        const auto* track = trackValue.getDynamicObject();
        if (track != nullptr
            && track->getProperty("type").toString() == "master"
            && track->getProperty("parentTrackId").toString().isEmpty())
        {
            masterId = track->getProperty("id").toString();
            break;
        }
    }
    if (masterId.isEmpty())
    {
        error = "Project migration could not find the master track.";
        return false;
    }

    juce::Array<juce::var> routes;
    for (const auto& trackValue : *tracksValue.getArray())
    {
        const auto* track = trackValue.getDynamicObject();
        if (track == nullptr)
            continue;

        const auto trackId = track->getProperty("id").toString();
        const auto type = track->getProperty("type").toString();
        if (trackId.isEmpty()
            || track->getProperty("parentTrackId").toString().isNotEmpty()
            || type == "master"
            || type == "folder"
            || type == "vca"
            || type == "controlRoom")
            continue;

        RoutingConnection connection;
        connection.name = "Main output";
        connection.kind = RouteKind::mainOutput;
        connection.sourceTrackId = trackId;
        connection.destination.type = RouteEndpointType::track;
        const auto legacyOutput = track->getProperty("outputTrackId").toString();
        connection.destination.trackId = legacyOutput.isNotEmpty()
            ? legacyOutput
            : masterId;
        routes.add(connection.toVar());
    }

    project.setProperty("routingConnections", juce::var(routes));
    return true;
}
}

std::optional<juce::var> ProjectMigration::migrateToCurrent(
    const juce::var& value,
    juce::String& error)
{
    auto migrated = value.clone();
    auto* object = migrated.getDynamicObject();
    if (object == nullptr)
    {
        error = "Project must be a JSON object.";
        return std::nullopt;
    }

    const auto version = formatVersion(*object);
    if (version < 1 || version > Project::currentFormatVersion)
    {
        error = "Unsupported Studio Duo project format version "
            + juce::String(version)
            + ".";
        return std::nullopt;
    }

    if (version < 3
        && !addVersionThreeRouting(*object, error))
        return std::nullopt;

    addEmptyArray(*object, "routingConnections");
    addEmptyArray(*object, "automationLanes");
    addEmptyArray(*object, "toneSnapshots");
    addEmptyArray(*object, "mixerSnapshots");
    addEmptyArray(*object, "renderReports");
    object->setProperty("formatVersion", Project::currentFormatVersion);
    return migrated;
}
}

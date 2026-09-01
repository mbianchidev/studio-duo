#include "PluginCompatibilityDatabase.h"

#include <algorithm>

namespace studio
{
namespace
{
int integerProperty(const juce::DynamicObject& object,
                    const juce::Identifier& name)
{
    const auto value = object.getProperty(name);
    return value.isInt() || value.isInt64() || value.isDouble()
        ? static_cast<int>(value)
        : 0;
}

bool booleanProperty(const juce::DynamicObject& object,
                     const juce::Identifier& name)
{
    const auto value = object.getProperty(name);
    return value.isBool() && static_cast<bool>(value);
}
}

PluginCompatibilityDatabase::PluginCompatibilityDatabase(
    juce::File databaseFile)
    : file(std::move(databaseFile))
{
}

bool PluginCompatibilityDatabase::load(juce::String& error)
{
    entries.clear();
    if (!file.existsAsFile())
        return true;
    const auto value = juce::JSON::parse(file.loadFileAsString());
    const auto* root = value.getDynamicObject();
    if (root == nullptr
        || integerProperty(*root, "schemaVersion") != 1
        || !root->getProperty("records").isArray())
    {
        error = "Plugin compatibility database is corrupt or unsupported.";
        return false;
    }

    for (const auto& recordValue : *root->getProperty("records").getArray())
    {
        const auto* object = recordValue.getDynamicObject();
        if (object == nullptr)
        {
            error = "Plugin compatibility record must be an object.";
            return false;
        }
        const auto failure = pluginFailureKindFromString(
            object->getProperty("lastFailure").toString());
        const auto mode = pluginBridgeModeFromString(
            object->getProperty("preferredMode").toString());
        if (!failure.has_value() || !mode.has_value())
        {
            error = "Plugin compatibility record contains an unsupported value.";
            return false;
        }
        PluginCompatibilityRecord record;
        record.pluginIdentifier =
            object->getProperty("pluginIdentifier").toString();
        record.name = object->getProperty("name").toString();
        record.format = object->getProperty("format").toString();
        record.vendor = object->getProperty("vendor").toString();
        record.version = object->getProperty("version").toString();
        record.architecture =
            object->getProperty("architecture").toString();
        record.preferredMode = *mode;
        record.lastFailure = *failure;
        record.lastMessage = object->getProperty("lastMessage").toString();
        record.updatedAt = object->getProperty("updatedAt").toString();
        record.scanCrashCount = integerProperty(*object, "scanCrashCount");
        record.runtimeCrashCount =
            integerProperty(*object, "runtimeCrashCount");
        record.timeoutCount = integerProperty(*object, "timeoutCount");
        record.araCapable = booleanProperty(*object, "araCapable");
        if (record.pluginIdentifier.isEmpty())
        {
            error = "Plugin compatibility record has no identifier.";
            return false;
        }
        entries.push_back(std::move(record));
    }
    return true;
}

bool PluginCompatibilityDatabase::save(juce::String& error) const
{
    if (!file.getParentDirectory().createDirectory())
    {
        error = "Could not create the plugin compatibility directory.";
        return false;
    }

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schemaVersion", 1);
    juce::Array<juce::var> recordsValue;
    for (const auto& record : entries)
    {
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty("pluginIdentifier", record.pluginIdentifier);
        object->setProperty("name", record.name);
        object->setProperty("format", record.format);
        object->setProperty("vendor", record.vendor);
        object->setProperty("version", record.version);
        object->setProperty("architecture", record.architecture);
        object->setProperty(
            "preferredMode",
            pluginBridgeModeToString(record.preferredMode));
        object->setProperty(
            "lastFailure",
            pluginFailureKindToString(record.lastFailure));
        object->setProperty("lastMessage", record.lastMessage);
        object->setProperty("updatedAt", record.updatedAt);
        object->setProperty("scanCrashCount", record.scanCrashCount);
        object->setProperty(
            "runtimeCrashCount",
            record.runtimeCrashCount);
        object->setProperty("timeoutCount", record.timeoutCount);
        object->setProperty("araCapable", record.araCapable);
        recordsValue.add(juce::var(object.release()));
    }
    root->setProperty("records", juce::var(recordsValue));

    const auto temporary = file.getSiblingFile(
        file.getFileName()
            + ".tmp-"
            + juce::Uuid().toString());
    if (!temporary.replaceWithText(
            juce::JSON::toString(juce::var(root.release()), true),
            false,
            false,
            "\n"))
    {
        error = "Could not write the plugin compatibility database.";
        return false;
    }
    const auto replaced = file.existsAsFile()
        ? temporary.replaceFileIn(file)
        : temporary.moveFileTo(file);
    if (!replaced)
    {
        temporary.deleteFile();
        error = "Could not publish the plugin compatibility database.";
        return false;
    }
    return true;
}

void PluginCompatibilityDatabase::noteFailure(
    const PluginCompatibilityRecord& identity,
    PluginFailureKind failure,
    juce::String message)
{
    auto& record = findOrAdd(identity);
    record.lastFailure = failure;
    record.lastMessage = std::move(message);
    record.updatedAt = juce::Time::getCurrentTime().toISO8601(true);
    if (failure == PluginFailureKind::scanCrash)
        ++record.scanCrashCount;
    else if (failure == PluginFailureKind::runtimeCrash)
        ++record.runtimeCrashCount;
    else if (failure == PluginFailureKind::timeout)
        ++record.timeoutCount;
}

void PluginCompatibilityDatabase::noteReady(
    const PluginCompatibilityRecord& identity,
    PluginBridgeMode mode)
{
    auto& record = findOrAdd(identity);
    record.preferredMode = mode;
    record.lastFailure = PluginFailureKind::none;
    record.lastMessage = "Ready";
    record.updatedAt = juce::Time::getCurrentTime().toISO8601(true);
}

std::optional<PluginCompatibilityRecord>
PluginCompatibilityDatabase::find(
    const juce::String& pluginIdentifier) const
{
    const auto iterator = std::find_if(
        entries.cbegin(),
        entries.cend(),
        [&pluginIdentifier](const auto& record)
        {
            return record.pluginIdentifier == pluginIdentifier;
        });
    return iterator == entries.cend()
        ? std::optional<PluginCompatibilityRecord> {}
        : *iterator;
}

const std::vector<PluginCompatibilityRecord>&
PluginCompatibilityDatabase::records() const
{
    return entries;
}

PluginCompatibilityRecord& PluginCompatibilityDatabase::findOrAdd(
    const PluginCompatibilityRecord& identity)
{
    const auto iterator = std::find_if(
        entries.begin(),
        entries.end(),
        [&identity](const auto& record)
        {
            return record.pluginIdentifier == identity.pluginIdentifier;
        });
    if (iterator != entries.end())
    {
        iterator->name = identity.name;
        iterator->format = identity.format;
        iterator->vendor = identity.vendor;
        iterator->version = identity.version;
        iterator->architecture = identity.architecture;
        iterator->araCapable = identity.araCapable;
        return *iterator;
    }
    entries.push_back(identity);
    return entries.back();
}

juce::String pluginFailureKindToString(PluginFailureKind value)
{
    switch (value)
    {
        case PluginFailureKind::none: return "none";
        case PluginFailureKind::scanCrash: return "scanCrash";
        case PluginFailureKind::runtimeCrash: return "runtimeCrash";
        case PluginFailureKind::timeout: return "timeout";
        case PluginFailureKind::protocolMismatch: return "protocolMismatch";
        case PluginFailureKind::unsupportedLayout: return "unsupportedLayout";
        case PluginFailureKind::corruptState: return "corruptState";
        case PluginFailureKind::missing: return "missing";
    }
    return "none";
}

std::optional<PluginFailureKind> pluginFailureKindFromString(
    const juce::String& value)
{
    if (value == "none") return PluginFailureKind::none;
    if (value == "scanCrash") return PluginFailureKind::scanCrash;
    if (value == "runtimeCrash") return PluginFailureKind::runtimeCrash;
    if (value == "timeout") return PluginFailureKind::timeout;
    if (value == "protocolMismatch") return PluginFailureKind::protocolMismatch;
    if (value == "unsupportedLayout") return PluginFailureKind::unsupportedLayout;
    if (value == "corruptState") return PluginFailureKind::corruptState;
    if (value == "missing") return PluginFailureKind::missing;
    return std::nullopt;
}
}

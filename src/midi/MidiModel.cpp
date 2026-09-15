#include "MidiModel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace studio
{
namespace
{
const juce::DynamicObject* requireObject(const juce::var& value,
                                         juce::String& error,
                                         const juce::String& context)
{
    if (const auto* object = value.getDynamicObject())
        return object;
    error = context + " must be a JSON object.";
    return nullptr;
}

double numberProperty(const juce::DynamicObject& object,
                      const juce::Identifier& name,
                      double fallback)
{
    const auto value = object.getProperty(name);
    return value.isDouble() || value.isInt() || value.isInt64()
        ? static_cast<double>(value)
        : fallback;
}

int integerProperty(const juce::DynamicObject& object,
                    const juce::Identifier& name,
                    int fallback)
{
    const auto value = object.getProperty(name);
    return value.isInt() || value.isInt64()
        ? static_cast<int>(value)
        : fallback;
}

bool booleanProperty(const juce::DynamicObject& object,
                     const juce::Identifier& name,
                     bool fallback)
{
    const auto value = object.getProperty(name);
    return value.isBool() ? static_cast<bool>(value) : fallback;
}

bool validId(const juce::String& value)
{
    return value.isNotEmpty();
}

template <typename Item>
bool uniqueIds(const std::vector<Item>& items)
{
    std::vector<juce::String> ids;
    ids.reserve(items.size());
    for (const auto& item : items)
    {
        if (!validId(item.id)
            || std::find(ids.cbegin(), ids.cend(), item.id) != ids.cend())
            return false;
        ids.push_back(item.id);
    }
    return true;
}

juce::Array<juce::var> integerArray(const std::vector<int>& values)
{
    juce::Array<juce::var> result;
    result.ensureStorageAllocated(static_cast<int>(values.size()));
    for (const auto value : values)
        result.add(value);
    return result;
}

bool readIntegerArray(const juce::var& value,
                      std::vector<int>& destination,
                      int minimum,
                      int maximum,
                      juce::String& error,
                      const juce::String& context)
{
    if (!value.isArray())
    {
        error = context + " must be an array.";
        return false;
    }
    for (const auto& item : *value.getArray())
    {
        if (!item.isInt() && !item.isInt64())
        {
            error = context + " must contain integers.";
            return false;
        }
        const auto parsed = static_cast<int>(item);
        if (parsed < minimum || parsed > maximum)
        {
            error = context + " contains an out-of-range value.";
            return false;
        }
        destination.push_back(parsed);
    }
    return true;
}

const DrumMapEntry* findByName(const DrumMap& map,
                               const juce::String& name)
{
    const auto iterator = std::find_if(
        map.entries.cbegin(),
        map.entries.cend(),
        [&name](const auto& entry)
        {
            return entry.name.equalsIgnoreCase(name);
        });
    return iterator == map.entries.cend() ? nullptr : &*iterator;
}

MidiPatternEvent patternEvent(const DrumMap& map,
                              const juce::String& name,
                              double offset,
                              int velocity)
{
    MidiPatternEvent event;
    event.offsetBeats = offset;
    event.velocity = velocity;
    if (const auto* entry = findByName(map, name))
    {
        event.drumMapEntryId = entry->id;
        event.pitch = entry->noteNumber;
    }
    return event;
}
}

double MidiNote::actualStartBeats() const noexcept
{
    return startBeats + timingOffsetBeats;
}

double MidiNote::endBeats() const noexcept
{
    return actualStartBeats() + durationBeats;
}

double MidiClip::endBeats() const noexcept
{
    return startBeats + durationBeats;
}

juce::var MidiExpressionPoint::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("type", midiExpressionTypeToString(type));
    object->setProperty("offsetBeats", offsetBeats);
    object->setProperty("value", value);
    object->setProperty("controller", controller);
    return juce::var(object.release());
}

std::optional<MidiExpressionPoint> MidiExpressionPoint::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "MIDI expression point");
    if (object == nullptr)
        return std::nullopt;

    MidiExpressionPoint point;
    point.id = object->getProperty("id").toString();
    const auto type = midiExpressionTypeFromString(
        object->getProperty("type").toString());
    if (!type.has_value())
    {
        error = "MIDI expression point contains an unsupported type.";
        return std::nullopt;
    }
    point.type = *type;
    point.offsetBeats = numberProperty(*object, "offsetBeats", 0.0);
    point.value = numberProperty(*object, "value", 0.0);
    point.controller = integerProperty(*object, "controller", -1);
    const auto minimum = point.type == MidiExpressionType::pitchBend
        ? -1.0
        : 0.0;
    if (!validId(point.id)
        || !std::isfinite(point.offsetBeats)
        || !std::isfinite(point.value)
        || point.offsetBeats < 0.0
        || point.value < minimum
        || point.value > 1.0
        || (point.type == MidiExpressionType::controller
            && (point.controller < 0 || point.controller > 127))
        || (point.type != MidiExpressionType::controller
            && point.controller != -1))
    {
        error = "MIDI expression point contains an invalid ID, position, or value.";
        return std::nullopt;
    }
    return point;
}

juce::var MidiNote::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("pitch", pitch);
    object->setProperty("channel", channel);
    object->setProperty("startBeats", startBeats);
    object->setProperty("timingOffsetBeats", timingOffsetBeats);
    object->setProperty("durationBeats", durationBeats);
    object->setProperty("velocity", velocity);
    object->setProperty("releaseVelocity", releaseVelocity);
    object->setProperty("probability", probability);
    object->setProperty("drumMapEntryId", drumMapEntryId);
    object->setProperty("articulation", articulation);
    object->setProperty("chokeGroup", chokeGroup);
    object->setProperty("cymbalState", cymbalStateToString(cymbalState));
    object->setProperty("footControlValue", footControlValue);
    object->setProperty("roundRobinHint", roundRobinHint);
    juce::Array<juce::var> expressionValues;
    expressionValues.ensureStorageAllocated(
        static_cast<int>(expressions.size()));
    for (const auto& expression : expressions)
        expressionValues.add(expression.toVar());
    object->setProperty("expressions", juce::var(expressionValues));
    return juce::var(object.release());
}

std::optional<MidiNote> MidiNote::fromVar(const juce::var& value,
                                          juce::String& error)
{
    const auto* object = requireObject(value, error, "MIDI note");
    if (object == nullptr)
        return std::nullopt;

    MidiNote note;
    note.id = object->getProperty("id").toString();
    note.pitch = integerProperty(*object, "pitch", 60);
    note.channel = integerProperty(*object, "channel", 1);
    note.startBeats = numberProperty(*object, "startBeats", 0.0);
    note.timingOffsetBeats = numberProperty(
        *object,
        "timingOffsetBeats",
        0.0);
    note.durationBeats = numberProperty(*object, "durationBeats", 0.25);
    note.velocity = integerProperty(*object, "velocity", 100);
    note.releaseVelocity = integerProperty(
        *object,
        "releaseVelocity",
        64);
    note.probability = numberProperty(*object, "probability", 1.0);
    note.drumMapEntryId =
        object->getProperty("drumMapEntryId").toString();
    note.articulation = object->getProperty("articulation").toString();
    note.chokeGroup = object->getProperty("chokeGroup").toString();
    const auto cymbal = cymbalStateFromString(
        object->getProperty("cymbalState").toString());
    if (!cymbal.has_value())
    {
        error = "MIDI note contains an unsupported cymbal state.";
        return std::nullopt;
    }
    note.cymbalState = *cymbal;
    note.footControlValue = integerProperty(
        *object,
        "footControlValue",
        -1);
    note.roundRobinHint = integerProperty(*object, "roundRobinHint", -1);
    const auto expressionValues = object->getProperty("expressions");
    if (!expressionValues.isVoid())
    {
        if (!expressionValues.isArray())
        {
            error = "MIDI note expressions must be an array.";
            return std::nullopt;
        }
        for (const auto& expressionValue : *expressionValues.getArray())
        {
            auto expression = MidiExpressionPoint::fromVar(
                expressionValue,
                error);
            if (!expression.has_value())
                return std::nullopt;
            note.expressions.push_back(std::move(*expression));
        }
    }

    if (!validId(note.id)
        || note.pitch < 0
        || note.pitch > 127
        || note.channel < 1
        || note.channel > 16
        || !std::isfinite(note.startBeats)
        || !std::isfinite(note.timingOffsetBeats)
        || !std::isfinite(note.durationBeats)
        || !std::isfinite(note.probability)
        || note.startBeats < 0.0
        || note.actualStartBeats() < 0.0
        || note.durationBeats <= 0.0
        || note.velocity < 1
        || note.velocity > 127
        || note.releaseVelocity < 0
        || note.releaseVelocity > 127
        || note.probability < 0.0
        || note.probability > 1.0
        || note.footControlValue < -1
        || note.footControlValue > 127
        || note.roundRobinHint < -1)
    {
        error = "MIDI note contains an invalid ID, pitch, channel, timing, or value.";
        return std::nullopt;
    }
    if (!uniqueIds(note.expressions)
        || std::any_of(
            note.expressions.cbegin(),
            note.expressions.cend(),
            [&note](const auto& expression)
            {
                return expression.offsetBeats
                    > note.durationBeats + 0.0000001;
            }))
    {
        error = "MIDI note expressions require unique IDs inside the note duration.";
        return std::nullopt;
    }
    return note;
}

juce::var MidiClip::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("startBeats", startBeats);
    object->setProperty("durationBeats", durationBeats);
    object->setProperty("editorMode", midiEditorModeToString(editorMode));
    object->setProperty("drumMapId", drumMapId);
    object->setProperty(
        "humanizeSeed",
        juce::String(static_cast<juce::uint64>(humanizeSeed)));
    object->setProperty("humanizeTimingTicks", humanizeTimingTicks);
    object->setProperty("humanizeVelocity", humanizeVelocity);
    object->setProperty("muted", muted);
    juce::Array<juce::var> noteValues;
    noteValues.ensureStorageAllocated(static_cast<int>(notes.size()));
    for (const auto& note : notes)
        noteValues.add(note.toVar());
    object->setProperty("notes", juce::var(noteValues));
    return juce::var(object.release());
}

std::optional<MidiClip> MidiClip::fromVar(const juce::var& value,
                                          juce::String& error)
{
    const auto* object = requireObject(value, error, "MIDI clip");
    if (object == nullptr)
        return std::nullopt;

    MidiClip clip;
    clip.id = object->getProperty("id").toString();
    clip.name = object->getProperty("name").toString().trim();
    clip.startBeats = numberProperty(*object, "startBeats", 0.0);
    clip.durationBeats = numberProperty(*object, "durationBeats", 4.0);
    const auto mode = midiEditorModeFromString(
        object->getProperty("editorMode").toString());
    if (!mode.has_value())
    {
        error = "MIDI clip contains an unsupported editor mode.";
        return std::nullopt;
    }
    clip.editorMode = *mode;
    clip.drumMapId = object->getProperty("drumMapId").toString();
    const auto seedText = object->getProperty("humanizeSeed").toString();
    if (seedText.isNotEmpty())
    {
        const auto text = seedText.toStdString();
        std::uint64_t parsed = 0;
        const auto conversion = std::from_chars(
            text.data(),
            text.data() + text.size(),
            parsed);
        if (conversion.ec != std::errc {}
            || conversion.ptr != text.data() + text.size())
        {
            error = "MIDI clip humanization seed is invalid.";
            return std::nullopt;
        }
        clip.humanizeSeed = parsed;
    }
    clip.humanizeTimingTicks = integerProperty(
        *object,
        "humanizeTimingTicks",
        0);
    clip.humanizeVelocity = integerProperty(
        *object,
        "humanizeVelocity",
        0);
    clip.muted = booleanProperty(*object, "muted", false);
    const auto noteValues = object->getProperty("notes");
    if (!noteValues.isArray())
    {
        error = "MIDI clip notes must be an array.";
        return std::nullopt;
    }
    for (const auto& noteValue : *noteValues.getArray())
    {
        auto note = MidiNote::fromVar(noteValue, error);
        if (!note.has_value())
            return std::nullopt;
        clip.notes.push_back(std::move(*note));
    }

    if (!validId(clip.id)
        || clip.name.isEmpty()
        || !std::isfinite(clip.startBeats)
        || !std::isfinite(clip.durationBeats)
        || clip.startBeats < 0.0
        || clip.durationBeats <= 0.0
        || clip.humanizeTimingTicks < 0
        || clip.humanizeTimingTicks > 960
        || clip.humanizeVelocity < 0
        || clip.humanizeVelocity > 127
        || !uniqueIds(clip.notes)
        || std::any_of(
            clip.notes.cbegin(),
            clip.notes.cend(),
            [&clip](const auto& note)
            {
                return note.endBeats()
                    > clip.durationBeats + 0.0000001;
            }))
    {
        error = "MIDI clip contains invalid IDs, timing, or humanization settings.";
        return std::nullopt;
    }
    return clip;
}

juce::var DrumMapEntry::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("noteNumber", noteNumber);
    object->setProperty("name", name);
    object->setProperty("articulation", articulation);
    object->setProperty("chokeGroup", chokeGroup);
    object->setProperty("cymbalState", cymbalStateToString(cymbalState));
    object->setProperty("footControlCC", footControlCC);
    object->setProperty(
        "roundRobinNotes",
        juce::var(integerArray(roundRobinNotes)));
    object->setProperty("outputGroup", outputGroup);
    return juce::var(object.release());
}

std::optional<DrumMapEntry> DrumMapEntry::fromVar(const juce::var& value,
                                                  juce::String& error)
{
    const auto* object = requireObject(value, error, "Drum-map entry");
    if (object == nullptr)
        return std::nullopt;
    DrumMapEntry entry;
    entry.id = object->getProperty("id").toString();
    entry.noteNumber = integerProperty(*object, "noteNumber", 36);
    entry.name = object->getProperty("name").toString().trim();
    entry.articulation =
        object->getProperty("articulation").toString().trim();
    entry.chokeGroup = object->getProperty("chokeGroup").toString().trim();
    const auto state = cymbalStateFromString(
        object->getProperty("cymbalState").toString());
    if (!state.has_value())
    {
        error = "Drum-map entry contains an unsupported cymbal state.";
        return std::nullopt;
    }
    entry.cymbalState = *state;
    entry.footControlCC = integerProperty(*object, "footControlCC", -1);
    const auto roundRobin = object->getProperty("roundRobinNotes");
    if (!roundRobin.isVoid()
        && !readIntegerArray(
            roundRobin,
            entry.roundRobinNotes,
            0,
            127,
            error,
            "Drum-map round-robin notes"))
        return std::nullopt;
    entry.outputGroup =
        object->getProperty("outputGroup").toString().trim();
    if (!validId(entry.id)
        || entry.noteNumber < 0
        || entry.noteNumber > 127
        || entry.name.isEmpty()
        || entry.articulation.isEmpty()
        || entry.footControlCC < -1
        || entry.footControlCC > 127
        || entry.outputGroup.isEmpty())
    {
        error = "Drum-map entries require valid IDs, pitches, names, and controls.";
        return std::nullopt;
    }
    return entry;
}

const DrumMapEntry* DrumMap::entryForPitch(int pitch) const noexcept
{
    const auto iterator = std::find_if(
        entries.cbegin(),
        entries.cend(),
        [pitch](const auto& entry)
        {
            return entry.noteNumber == pitch
                || std::find(
                       entry.roundRobinNotes.cbegin(),
                       entry.roundRobinNotes.cend(),
                       pitch)
                    != entry.roundRobinNotes.cend();
        });
    return iterator == entries.cend() ? nullptr : &*iterator;
}

const DrumMapEntry* DrumMap::entryForId(
    const juce::String& entryId) const noexcept
{
    const auto iterator = std::find_if(
        entries.cbegin(),
        entries.cend(),
        [&entryId](const auto& entry) { return entry.id == entryId; });
    return iterator == entries.cend() ? nullptr : &*iterator;
}

juce::var DrumMap::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("source", source);
    juce::Array<juce::var> values;
    values.ensureStorageAllocated(static_cast<int>(entries.size()));
    for (const auto& entry : entries)
        values.add(entry.toVar());
    object->setProperty("entries", juce::var(values));
    return juce::var(object.release());
}

std::optional<DrumMap> DrumMap::fromVar(const juce::var& value,
                                        juce::String& error)
{
    const auto* object = requireObject(value, error, "Drum map");
    if (object == nullptr)
        return std::nullopt;
    DrumMap map;
    map.id = object->getProperty("id").toString();
    map.name = object->getProperty("name").toString().trim();
    map.source = object->getProperty("source").toString();
    const auto values = object->getProperty("entries");
    if (!values.isArray())
    {
        error = "Drum-map entries must be an array.";
        return std::nullopt;
    }
    for (const auto& entryValue : *values.getArray())
    {
        auto entry = DrumMapEntry::fromVar(entryValue, error);
        if (!entry.has_value())
            return std::nullopt;
        map.entries.push_back(std::move(*entry));
    }
    std::vector<int> pitches;
    for (const auto& entry : map.entries)
    {
        const auto addPitch = [&pitches](int pitch)
        {
            if (std::find(pitches.cbegin(), pitches.cend(), pitch)
                != pitches.cend())
                return false;
            pitches.push_back(pitch);
            return true;
        };
        if (!addPitch(entry.noteNumber)
            || std::any_of(
                entry.roundRobinNotes.cbegin(),
                entry.roundRobinNotes.cend(),
                [&addPitch](int pitch) { return !addPitch(pitch); }))
        {
            error = "Drum-map base and round-robin pitches must be unique.";
            return std::nullopt;
        }
    }
    if (!validId(map.id)
        || map.name.isEmpty()
        || map.entries.empty()
        || !uniqueIds(map.entries))
    {
        error = "Drum maps require an ID, name, and uniquely identified entries.";
        return std::nullopt;
    }
    return map;
}

juce::var MidiPatternEvent::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("drumMapEntryId", drumMapEntryId);
    object->setProperty("pitch", pitch);
    object->setProperty("offsetBeats", offsetBeats);
    object->setProperty("durationBeats", durationBeats);
    object->setProperty("velocity", velocity);
    object->setProperty("probability", probability);
    return juce::var(object.release());
}

std::optional<MidiPatternEvent> MidiPatternEvent::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "MIDI pattern event");
    if (object == nullptr)
        return std::nullopt;
    MidiPatternEvent event;
    event.id = object->getProperty("id").toString();
    event.drumMapEntryId =
        object->getProperty("drumMapEntryId").toString();
    event.pitch = integerProperty(*object, "pitch", 36);
    event.offsetBeats = numberProperty(*object, "offsetBeats", 0.0);
    event.durationBeats = numberProperty(*object, "durationBeats", 0.125);
    event.velocity = integerProperty(*object, "velocity", 100);
    event.probability = numberProperty(*object, "probability", 1.0);
    if (!validId(event.id)
        || event.pitch < 0
        || event.pitch > 127
        || !std::isfinite(event.offsetBeats)
        || !std::isfinite(event.durationBeats)
        || !std::isfinite(event.probability)
        || event.offsetBeats < 0.0
        || event.durationBeats <= 0.0
        || event.velocity < 1
        || event.velocity > 127
        || event.probability < 0.0
        || event.probability > 1.0)
    {
        error = "MIDI pattern event contains an invalid ID, pitch, timing, or value.";
        return std::nullopt;
    }
    return event;
}

juce::var MidiPatternAlias::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("lengthBeats", lengthBeats);
    juce::Array<juce::var> values;
    values.ensureStorageAllocated(static_cast<int>(events.size()));
    for (const auto& event : events)
        values.add(event.toVar());
    object->setProperty("events", juce::var(values));
    return juce::var(object.release());
}

std::optional<MidiPatternAlias> MidiPatternAlias::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(value, error, "MIDI pattern alias");
    if (object == nullptr)
        return std::nullopt;
    MidiPatternAlias pattern;
    pattern.id = object->getProperty("id").toString();
    pattern.name = object->getProperty("name").toString().trim();
    pattern.lengthBeats = numberProperty(*object, "lengthBeats", 4.0);
    const auto values = object->getProperty("events");
    if (!values.isArray())
    {
        error = "MIDI pattern events must be an array.";
        return std::nullopt;
    }
    for (const auto& eventValue : *values.getArray())
    {
        auto event = MidiPatternEvent::fromVar(eventValue, error);
        if (!event.has_value())
            return std::nullopt;
        pattern.events.push_back(std::move(*event));
    }
    if (!validId(pattern.id)
        || pattern.name.isEmpty()
        || !std::isfinite(pattern.lengthBeats)
        || pattern.lengthBeats <= 0.0
        || pattern.events.empty()
        || !uniqueIds(pattern.events)
        || std::any_of(
            pattern.events.cbegin(),
            pattern.events.cend(),
            [&pattern](const auto& event)
            {
                return event.offsetBeats + event.durationBeats
                    > pattern.lengthBeats + 0.0000001;
            }))
    {
        error = "MIDI patterns require valid IDs, timing, and ordinary note events.";
        return std::nullopt;
    }
    return pattern;
}

juce::var MidiRoutingTemplateOutput::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("destinationTrackId", destinationTrackId);
    object->setProperty("midiChannel", midiChannel);
    object->setProperty("pitches", juce::var(integerArray(pitches)));
    return juce::var(object.release());
}

std::optional<MidiRoutingTemplateOutput>
MidiRoutingTemplateOutput::fromVar(const juce::var& value,
                                   juce::String& error)
{
    const auto* object = requireObject(
        value,
        error,
        "MIDI routing-template output");
    if (object == nullptr)
        return std::nullopt;
    MidiRoutingTemplateOutput output;
    output.id = object->getProperty("id").toString();
    output.name = object->getProperty("name").toString().trim();
    output.destinationTrackId =
        object->getProperty("destinationTrackId").toString();
    output.midiChannel = integerProperty(*object, "midiChannel", 1);
    if (!readIntegerArray(
            object->getProperty("pitches"),
            output.pitches,
            0,
            127,
            error,
            "MIDI routing-template pitches"))
        return std::nullopt;
    if (!validId(output.id)
        || output.name.isEmpty()
        || output.midiChannel < 1
        || output.midiChannel > 16
        || output.pitches.empty())
    {
        error = "MIDI routing-template outputs require IDs, names, channels, and pitches.";
        return std::nullopt;
    }
    std::sort(output.pitches.begin(), output.pitches.end());
    if (std::adjacent_find(output.pitches.cbegin(), output.pitches.cend())
        != output.pitches.cend())
    {
        error = "MIDI routing-template output pitches must be unique.";
        return std::nullopt;
    }
    return output;
}

juce::var MidiRoutingTemplate::toVar() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("id", id);
    object->setProperty("name", name);
    juce::Array<juce::var> values;
    values.ensureStorageAllocated(static_cast<int>(outputs.size()));
    for (const auto& output : outputs)
        values.add(output.toVar());
    object->setProperty("outputs", juce::var(values));
    return juce::var(object.release());
}

std::optional<MidiRoutingTemplate> MidiRoutingTemplate::fromVar(
    const juce::var& value,
    juce::String& error)
{
    const auto* object = requireObject(
        value,
        error,
        "MIDI routing template");
    if (object == nullptr)
        return std::nullopt;
    MidiRoutingTemplate result;
    result.id = object->getProperty("id").toString();
    result.name = object->getProperty("name").toString().trim();
    const auto values = object->getProperty("outputs");
    if (!values.isArray())
    {
        error = "MIDI routing-template outputs must be an array.";
        return std::nullopt;
    }
    for (const auto& outputValue : *values.getArray())
    {
        auto output = MidiRoutingTemplateOutput::fromVar(
            outputValue,
            error);
        if (!output.has_value())
            return std::nullopt;
        result.outputs.push_back(std::move(*output));
    }
    std::vector<int> assignedPitches;
    for (const auto& output : result.outputs)
    {
        for (const auto pitch : output.pitches)
        {
            if (std::find(
                    assignedPitches.cbegin(),
                    assignedPitches.cend(),
                    pitch)
                != assignedPitches.cend())
            {
                error = "A MIDI routing-template pitch can belong to only one output.";
                return std::nullopt;
            }
            assignedPitches.push_back(pitch);
        }
    }
    if (!validId(result.id)
        || result.name.isEmpty()
        || result.outputs.empty()
        || !uniqueIds(result.outputs))
    {
        error = "MIDI routing templates require an ID, name, and unique outputs.";
        return std::nullopt;
    }
    return result;
}

juce::String midiExpressionTypeToString(MidiExpressionType type)
{
    switch (type)
    {
        case MidiExpressionType::pitchBend: return "pitchBend";
        case MidiExpressionType::pressure: return "pressure";
        case MidiExpressionType::timbre: return "timbre";
        case MidiExpressionType::controller: return "controller";
    }
    return "pressure";
}

std::optional<MidiExpressionType> midiExpressionTypeFromString(
    const juce::String& value)
{
    if (value == "pitchBend") return MidiExpressionType::pitchBend;
    if (value == "pressure") return MidiExpressionType::pressure;
    if (value == "timbre") return MidiExpressionType::timbre;
    if (value == "controller") return MidiExpressionType::controller;
    return std::nullopt;
}

juce::String midiEditorModeToString(MidiEditorMode mode)
{
    switch (mode)
    {
        case MidiEditorMode::pianoRoll: return "pianoRoll";
        case MidiEditorMode::drums: return "drums";
    }
    return "pianoRoll";
}

std::optional<MidiEditorMode> midiEditorModeFromString(
    const juce::String& value)
{
    if (value == "pianoRoll") return MidiEditorMode::pianoRoll;
    if (value == "drums") return MidiEditorMode::drums;
    return std::nullopt;
}

juce::String cymbalStateToString(CymbalState state)
{
    switch (state)
    {
        case CymbalState::none: return "none";
        case CymbalState::edge: return "edge";
        case CymbalState::bow: return "bow";
        case CymbalState::bell: return "bell";
        case CymbalState::choke: return "choke";
        case CymbalState::open: return "open";
        case CymbalState::closed: return "closed";
        case CymbalState::pedal: return "pedal";
    }
    return "none";
}

std::optional<CymbalState> cymbalStateFromString(
    const juce::String& value)
{
    if (value.isEmpty() || value == "none") return CymbalState::none;
    if (value == "edge") return CymbalState::edge;
    if (value == "bow") return CymbalState::bow;
    if (value == "bell") return CymbalState::bell;
    if (value == "choke") return CymbalState::choke;
    if (value == "open") return CymbalState::open;
    if (value == "closed") return CymbalState::closed;
    if (value == "pedal") return CymbalState::pedal;
    return std::nullopt;
}

DrumMap createDefaultMetalDrumMap()
{
    DrumMap map;
    map.id = "studio-duo-metal-map-v1";
    map.name = "Studio Duo Metal";
    map.source = "Studio Duo default";
    const auto add = [&map](int pitch,
                            const juce::String& name,
                            const juce::String& articulation,
                            const juce::String& choke,
                            CymbalState state,
                            int footControl,
                            std::vector<int> roundRobin,
                            const juce::String& output)
    {
        DrumMapEntry entry;
        entry.id = "studio-duo-metal-note-" + juce::String(pitch);
        entry.noteNumber = pitch;
        entry.name = name;
        entry.articulation = articulation;
        entry.chokeGroup = choke;
        entry.cymbalState = state;
        entry.footControlCC = footControl;
        entry.roundRobinNotes = std::move(roundRobin);
        entry.outputGroup = output;
        map.entries.push_back(std::move(entry));
    };
    add(36, "Kick L", "center", {}, CymbalState::none, -1, { 35 }, "Kick");
    add(37, "Kick R", "center", {}, CymbalState::none, -1, {}, "Kick");
    add(38, "Snare", "center", {}, CymbalState::none, -1, { 40 }, "Snare");
    add(39, "Snare rim", "rimshot", {}, CymbalState::none, -1, {}, "Snare");
    add(41, "Floor tom", "center", {}, CymbalState::none, -1, { 43 }, "Toms");
    add(45, "Mid tom", "center", {}, CymbalState::none, -1, { 47 }, "Toms");
    add(48, "High tom", "center", {}, CymbalState::none, -1, { 50 }, "Toms");
    add(42, "Hi-hat closed", "tip", "hihat", CymbalState::closed, 4, {}, "Cymbals");
    add(44, "Hi-hat pedal", "foot", "hihat", CymbalState::pedal, 4, {}, "Cymbals");
    add(46, "Hi-hat open", "edge", "hihat", CymbalState::open, 4, {}, "Cymbals");
    add(49, "Crash 1", "edge", "crash1", CymbalState::edge, -1, { 55 }, "Cymbals");
    add(57, "Crash 1 choke", "choke", "crash1", CymbalState::choke, -1, {}, "Cymbals");
    add(52, "China", "edge", "china", CymbalState::edge, -1, { 58 }, "Cymbals");
    add(59, "China choke", "choke", "china", CymbalState::choke, -1, {}, "Cymbals");
    add(51, "Ride", "bow", "ride", CymbalState::bow, -1, { 53 }, "Cymbals");
    add(56, "Ride bell", "bell", "ride", CymbalState::bell, -1, {}, "Cymbals");
    add(60, "Ride choke", "choke", "ride", CymbalState::choke, -1, {}, "Cymbals");
    return map;
}

std::vector<MidiPatternAlias> createDefaultMetalPatterns(
    const DrumMap& map)
{
    MidiPatternAlias blast;
    blast.id = "studio-duo-pattern-blast-16-v1";
    blast.name = "16th blast";
    blast.lengthBeats = 4.0;
    for (int step = 0; step < 16; ++step)
    {
        const auto beat = static_cast<double>(step) * 0.25;
        blast.events.push_back(patternEvent(map, "Hi-hat closed", beat, 96));
        blast.events.push_back(patternEvent(map, "Snare", beat, step % 4 == 0 ? 118 : 104));
        if (step % 2 == 0)
            blast.events.push_back(patternEvent(map, "Kick L", beat, 112));
    }

    MidiPatternAlias doubleKick;
    doubleKick.id = "studio-duo-pattern-double-kick-16-v1";
    doubleKick.name = "16th double kick";
    doubleKick.lengthBeats = 4.0;
    for (int step = 0; step < 16; ++step)
        doubleKick.events.push_back(
            patternEvent(
                map,
                step % 2 == 0 ? "Kick L" : "Kick R",
                static_cast<double>(step) * 0.25,
                step % 4 == 0 ? 118 : 104));

    MidiPatternAlias gravity;
    gravity.id = "studio-duo-pattern-gravity-blast-v1";
    gravity.name = "Gravity blast";
    gravity.lengthBeats = 4.0;
    for (int step = 0; step < 32; ++step)
    {
        gravity.events.push_back(
            patternEvent(
                map,
                step % 2 == 0 ? "Snare" : "Snare rim",
                static_cast<double>(step) * 0.125,
                step % 2 == 0 ? 112 : 82));
        if (step % 4 == 0)
            gravity.events.push_back(
                patternEvent(
                    map,
                    "Kick L",
                    static_cast<double>(step) * 0.125,
                    110));
    }
    const auto assignEventIds = [](MidiPatternAlias& pattern)
    {
        for (std::size_t index = 0; index < pattern.events.size(); ++index)
        {
            pattern.events[index].id = pattern.id
                + "-event-"
                + juce::String(static_cast<int>(index + 1));
        }
    };
    assignEventIds(blast);
    assignEventIds(doubleKick);
    assignEventIds(gravity);
    return { std::move(blast), std::move(doubleKick), std::move(gravity) };
}

MidiRoutingTemplate createDefaultMetalRoutingTemplate(const DrumMap& map)
{
    MidiRoutingTemplate routing;
    routing.id = "studio-duo-routing-metal-multi-v1";
    routing.name = "Metal kit multi-output";
    const auto addOutput = [&routing, &map](const juce::String& name,
                                            int channel,
                                            const juce::String& group)
    {
        MidiRoutingTemplateOutput output;
        output.id = routing.id
            + "-output-"
            + juce::String(channel);
        output.name = name;
        output.midiChannel = channel;
        for (const auto& entry : map.entries)
        {
            if (entry.outputGroup != group)
                continue;
            output.pitches.push_back(entry.noteNumber);
            output.pitches.insert(
                output.pitches.end(),
                entry.roundRobinNotes.cbegin(),
                entry.roundRobinNotes.cend());
        }
        std::sort(output.pitches.begin(), output.pitches.end());
        output.pitches.erase(
            std::unique(output.pitches.begin(), output.pitches.end()),
            output.pitches.end());
        routing.outputs.push_back(std::move(output));
    };
    addOutput("Kick MIDI", 1, "Kick");
    addOutput("Snare MIDI", 2, "Snare");
    addOutput("Toms MIDI", 3, "Toms");
    addOutput("Cymbals MIDI", 4, "Cymbals");
    return routing;
}
}

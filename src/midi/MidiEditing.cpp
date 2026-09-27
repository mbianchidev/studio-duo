#include "MidiEditing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace studio
{
namespace
{
constexpr auto ticksPerBeat = 960.0;
constexpr auto openHiHatFootControl = 0;
constexpr auto closedHiHatFootControl = 127;
constexpr auto pedalHiHatFootControl =
    closedHiHatFootControl - 32;

std::uint64_t fnv1a(const juce::String& value) noexcept
{
    auto hash = std::uint64_t { 14695981039346656037ull };
    const auto utf8 = value.toRawUTF8();
    for (auto index = std::size_t { 0 }; utf8[index] != '\0'; ++index)
    {
        hash ^= static_cast<std::uint8_t>(utf8[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

std::uint32_t boundedValue(std::uint64_t value,
                           std::uint32_t bound) noexcept
{
    if (bound == 0)
        return 0;
    return static_cast<std::uint32_t>(
        (static_cast<unsigned long long>(
             static_cast<std::uint32_t>(value))
         * static_cast<unsigned long long>(bound))
        >> 32u);
}

const DrumMapEntry* namedEntry(const DrumMap* map,
                               const juce::String& name,
                               int fallbackPitch)
{
    if (map == nullptr)
        return nullptr;
    const auto named = std::find_if(
        map->entries.cbegin(),
        map->entries.cend(),
        [&name](const auto& entry)
        {
            return entry.name.equalsIgnoreCase(name);
        });
    if (named != map->entries.cend())
        return &*named;
    return map->entryForPitch(fallbackPitch);
}

void applyDrumMetadata(MidiNote& note,
                       const DrumMapEntry* entry,
                       std::size_t roundRobinIndex)
{
    if (entry == nullptr)
        return;
    note.drumMapEntryId = entry->id;
    note.articulation = entry->articulation;
    note.chokeGroup = entry->chokeGroup;
    note.cymbalState = entry->cymbalState;
    if (entry->footControlCC >= 0)
    {
        if (entry->cymbalState == CymbalState::open)
            note.footControlValue = openHiHatFootControl;
        else if (entry->cymbalState == CymbalState::closed)
            note.footControlValue = closedHiHatFootControl;
        else if (entry->cymbalState == CymbalState::pedal)
            note.footControlValue = pedalHiHatFootControl;
    }
    if (!entry->roundRobinNotes.empty())
    {
        const auto variants = entry->roundRobinNotes.size() + 1;
        const auto selected = roundRobinIndex % variants;
        note.roundRobinHint = static_cast<int>(selected);
        if (selected > 0)
            note.pitch = entry->roundRobinNotes[selected - 1];
    }
}

MidiNote toolNote(const MidiEntryRequest& request,
                  const DrumMapEntry* entry,
                  int fallbackPitch,
                  double start,
                  int velocity,
                  std::size_t roundRobinIndex)
{
    MidiNote note;
    note.pitch = entry != nullptr ? entry->noteNumber : fallbackPitch;
    note.startBeats = std::max(0.0, start);
    note.durationBeats = std::max(1.0 / 128.0,
                                  request.noteDurationBeats);
    note.velocity = juce::jlimit(1, 127, velocity);
    applyDrumMetadata(note, entry, roundRobinIndex);
    return note;
}

std::vector<std::size_t> selectedIndices(
    const MidiClip& clip,
    const std::vector<juce::String>& noteIds)
{
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < clip.notes.size(); ++index)
        if (std::find(
                noteIds.cbegin(),
                noteIds.cend(),
                clip.notes[index].id)
            != noteIds.cend())
            result.push_back(index);
    return result;
}

MidiExpressionPoint expressionPoint(
    MidiExpressionType type,
    double offset,
    double normalized)
{
    MidiExpressionPoint point;
    point.type = type;
    point.offsetBeats = std::max(0.0, offset);
    point.value = type == MidiExpressionType::pitchBend
        ? juce::jlimit(-1.0, 1.0, normalized * 2.0 - 1.0)
        : juce::jlimit(0.0, 1.0, normalized);
    point.controller = type == MidiExpressionType::controller ? 11 : -1;
    return point;
}

void addRecordedExpression(MidiNote& note,
                           MidiExpressionType type,
                           int controller,
                           double offsetBeats,
                           double value)
{
    MidiExpressionPoint point;
    point.type = type;
    point.controller = controller;
    point.offsetBeats = std::max(0.0, offsetBeats);
    point.value = type == MidiExpressionType::pitchBend
        ? juce::jlimit(-1.0, 1.0, value)
        : juce::jlimit(0.0, 1.0, value);
    note.expressions.push_back(std::move(point));
}
}

std::uint64_t deterministicMidiValue(std::uint64_t seed,
                                     const juce::String& stableId,
                                     std::uint64_t stream) noexcept
{
    return splitMix64(seed ^ fnv1a(stableId)
                      ^ splitMix64(stream + 0x632be59bd9b4e019ull));
}

bool midiNoteShouldPlay(const MidiClip& clip,
                        const MidiNote& note) noexcept
{
    if (note.probability >= 1.0)
        return true;
    if (note.probability <= 0.0)
        return false;
    constexpr auto scale = std::uint32_t { 1000000 };
    const auto threshold = static_cast<std::uint32_t>(
        std::llround(note.probability * static_cast<double>(scale)));
    return boundedValue(
               deterministicMidiValue(
                   clip.humanizeSeed,
                   note.id,
                   7),
               scale)
        < threshold;
}

MidiClip humanizeMidiClip(const MidiClip& source,
                          const MidiHumanizeSettings& settings)
{
    auto result = source;
    result.humanizeSeed = settings.seed;
    result.humanizeTimingTicks = juce::jlimit(
        0,
        960,
        settings.maximumTimingTicks);
    result.humanizeVelocity = juce::jlimit(
        0,
        127,
        settings.maximumVelocityChange);
    for (auto& note : result.notes)
    {
        const auto timingRange = static_cast<std::uint32_t>(
            result.humanizeTimingTicks * 2 + 1);
        const auto velocityRange = static_cast<std::uint32_t>(
            result.humanizeVelocity * 2 + 1);
        const auto timing = static_cast<int>(
                                boundedValue(
                                    deterministicMidiValue(
                                        settings.seed,
                                        note.id,
                                        0),
                                    timingRange))
            - result.humanizeTimingTicks;
        const auto velocity = static_cast<int>(
                                  boundedValue(
                                      deterministicMidiValue(
                                          settings.seed,
                                          note.id,
                                          1),
                                      velocityRange))
            - result.humanizeVelocity;
        note.timingOffsetBeats =
            static_cast<double>(timing) / ticksPerBeat;
        if (note.actualStartBeats() < 0.0)
            note.timingOffsetBeats = -note.startBeats;
        if (note.endBeats() > result.durationBeats)
            note.timingOffsetBeats = result.durationBeats
                - note.durationBeats
                - note.startBeats;
        note.velocity = juce::jlimit(1, 127, note.velocity + velocity);
    }
    sortMidiNotes(result);
    return result;
}

std::vector<MidiNote> expandMidiPattern(
    const MidiPatternAlias& pattern,
    const DrumMap* drumMap,
    double startBeats,
    int repetitions)
{
    std::vector<MidiNote> result;
    repetitions = std::max(1, repetitions);
    result.reserve(
        pattern.events.size() * static_cast<std::size_t>(repetitions));
    for (int repetition = 0; repetition < repetitions; ++repetition)
    {
        for (std::size_t index = 0; index < pattern.events.size(); ++index)
        {
            const auto& event = pattern.events[index];
            MidiNote note;
            note.pitch = event.pitch;
            note.startBeats = std::max(
                0.0,
                startBeats
                    + pattern.lengthBeats
                        * static_cast<double>(repetition)
                    + event.offsetBeats);
            note.durationBeats = event.durationBeats;
            note.velocity = event.velocity;
            note.probability = event.probability;
            const auto* entry = drumMap != nullptr
                ? drumMap->entryForId(event.drumMapEntryId)
                : nullptr;
            if (entry == nullptr && drumMap != nullptr)
                entry = drumMap->entryForPitch(event.pitch);
            applyDrumMetadata(
                note,
                entry,
                static_cast<std::size_t>(repetition)
                    * pattern.events.size()
                    + index);
            result.push_back(std::move(note));
        }
    }
    return result;
}

std::vector<MidiNote> generateMidiEntryTool(
    MidiEntryTool tool,
    const MidiEntryRequest& request,
    const DrumMap* drumMap)
{
    std::vector<MidiNote> result;
    const auto start = std::max(0.0, request.startBeats);
    const auto length = std::max(request.stepBeats, request.lengthBeats);
    const auto step = std::max(1.0 / 128.0, request.stepBeats);
    const auto end = start + length;
    const auto kickLeft = namedEntry(drumMap, "Kick L", 36);
    const auto kickRight = namedEntry(drumMap, "Kick R", 37);
    const auto snare = namedEntry(drumMap, "Snare", 38);
    const auto snareRim = namedEntry(drumMap, "Snare rim", 39);
    const auto cymbal = namedEntry(drumMap, "Hi-hat closed", 42);

    switch (tool)
    {
        case MidiEntryTool::flam:
        {
            const auto entry = drumMap != nullptr
                ? drumMap->entryForPitch(request.pitch)
                : nullptr;
            for (auto beat = start; beat < end - 0.0000001; beat += step)
            {
                const auto graceStart =
                    beat - request.flamSpacingBeats >= start
                    ? beat - request.flamSpacingBeats
                    : beat;
                const auto mainStart =
                    beat - request.flamSpacingBeats >= start
                    ? beat
                    : beat + request.flamSpacingBeats;
                result.push_back(toolNote(
                    request,
                    entry,
                    request.pitch,
                    graceStart,
                    request.velocity - 24,
                    result.size()));
                result.push_back(toolNote(
                    request,
                    entry,
                    request.pitch,
                    mainStart,
                    request.velocity,
                    result.size()));
            }
            break;
        }
        case MidiEntryTool::roll:
        {
            const auto entry = drumMap != nullptr
                ? drumMap->entryForPitch(request.pitch)
                : nullptr;
            for (auto beat = start; beat < end - 0.0000001; beat += step)
                result.push_back(toolNote(
                    request,
                    entry,
                    request.pitch,
                    beat,
                    request.velocity
                        - (static_cast<int>(result.size()) % 2) * 10,
                    result.size()));
            break;
        }
        case MidiEntryTool::gravityBlast:
        {
            const auto gravityStep = step * 0.5;
            auto index = 0;
            for (auto beat = start;
                 beat < end - 0.0000001;
                 beat += gravityStep, ++index)
            {
                const auto* entry = index % 2 == 0 ? snare : snareRim;
                result.push_back(toolNote(
                    request,
                    entry,
                    index % 2 == 0 ? 38 : 39,
                    beat,
                    request.velocity - (index % 2 == 0 ? 0 : 26),
                    result.size()));
                if (index % 4 == 0)
                    result.push_back(toolNote(
                        request,
                        kickLeft,
                        36,
                        beat,
                        request.velocity,
                        result.size()));
            }
            break;
        }
        case MidiEntryTool::blastBeat:
        {
            auto index = 0;
            for (auto beat = start;
                 beat < end - 0.0000001;
                 beat += step, ++index)
            {
                result.push_back(toolNote(
                    request,
                    cymbal,
                    42,
                    beat,
                    request.velocity - 12,
                    result.size()));
                result.push_back(toolNote(
                    request,
                    snare,
                    38,
                    beat,
                    request.velocity - (index % 4 == 0 ? 0 : 10),
                    result.size()));
                if (index % 2 == 0)
                    result.push_back(toolNote(
                        request,
                        kickLeft,
                        36,
                        beat,
                        request.velocity,
                        result.size()));
            }
            break;
        }
        case MidiEntryTool::doubleKick:
        {
            auto index = 0;
            for (auto beat = start;
                 beat < end - 0.0000001;
                 beat += step, ++index)
            {
                const auto* entry = index % 2 == 0
                    ? kickLeft
                    : kickRight;
                result.push_back(toolNote(
                    request,
                    entry,
                    index % 2 == 0 ? 36 : 37,
                    beat,
                    request.velocity - (index % 4 == 0 ? 0 : 8),
                    result.size()));
            }
            break;
        }
    }
    return result;
}

MidiNote createMidiNote(const MidiClip& clip,
                        double startBeats,
                        int pitch,
                        double durationBeats,
                        int velocity,
                        const DrumMap* drumMap)
{
    MidiNote note;
    note.pitch = juce::jlimit(0, 127, pitch);
    note.startBeats = juce::jlimit(
        0.0,
        std::max(0.0, clip.durationBeats - 1.0 / 128.0),
        startBeats);
    note.durationBeats = juce::jlimit(
        1.0 / 128.0,
        std::max(1.0 / 128.0, clip.durationBeats - note.startBeats),
        durationBeats);
    note.velocity = juce::jlimit(1, 127, velocity);
    const auto* entry = drumMap != nullptr
        ? drumMap->entryForPitch(note.pitch)
        : nullptr;
    applyDrumMetadata(note, entry, clip.notes.size());
    return note;
}

void applyDrumMapMetadata(MidiNote& note,
                          const DrumMap* drumMap,
                          std::optional<std::size_t> roundRobinIndex)
{
    note.drumMapEntryId.clear();
    note.articulation.clear();
    note.chokeGroup.clear();
    note.cymbalState = CymbalState::none;
    note.footControlValue = -1;
    note.roundRobinHint = -1;
    const auto* entry = drumMap != nullptr ? drumMap->entryForPitch(note.pitch) : nullptr;
    if (!roundRobinIndex.has_value() && entry != nullptr)
    {
        const auto found = std::find(
            entry->roundRobinNotes.cbegin(), entry->roundRobinNotes.cend(), note.pitch);
        roundRobinIndex = found != entry->roundRobinNotes.cend()
            ? static_cast<std::size_t>(std::distance(entry->roundRobinNotes.cbegin(), found)) + 1
            : 0;
    }
    applyDrumMetadata(note, entry, roundRobinIndex.value_or(0));
}

bool moveMidiNotes(MidiClip& clip,
                   const std::vector<juce::String>& noteIds,
                   double deltaBeats,
                   int deltaPitch,
                   const DrumMap* drumMap)
{
    const auto indices = selectedIndices(clip, noteIds);
    if (indices.empty())
        return false;
    auto minimumStart = std::numeric_limits<double>::max();
    auto maximumEnd = 0.0;
    auto minimumPitch = 127;
    auto maximumPitch = 0;
    for (const auto index : indices)
    {
        const auto& note = clip.notes[index];
        minimumStart = std::min(minimumStart, note.actualStartBeats());
        maximumEnd = std::max(maximumEnd, note.endBeats());
        minimumPitch = std::min(minimumPitch, note.pitch);
        maximumPitch = std::max(maximumPitch, note.pitch);
    }
    deltaBeats = juce::jlimit(
        -minimumStart,
        clip.durationBeats - maximumEnd,
        deltaBeats);
    deltaPitch = juce::jlimit(-minimumPitch, 127 - maximumPitch, deltaPitch);
    if (std::abs(deltaBeats) < 0.0000001 && deltaPitch == 0)
        return false;
    const auto* validDrumMap =
        clip.editorMode == MidiEditorMode::drums
            && drumMap != nullptr
            && drumMap->id == clip.drumMapId
        ? drumMap
        : nullptr;
    for (const auto index : indices)
    {
        auto& note = clip.notes[index];
        note.startBeats += deltaBeats;
        if (deltaPitch == 0)
            continue;

        const auto previousRoundRobinHint = note.roundRobinHint;
        note.pitch += deltaPitch;
        const auto* entry = validDrumMap != nullptr
            ? validDrumMap->entryForPitch(note.pitch)
            : nullptr;
        const auto retainedRoundRobin =
            entry != nullptr
                && previousRoundRobinHint >= 0
                && static_cast<std::size_t>(previousRoundRobinHint)
                    <= entry->roundRobinNotes.size()
            ? static_cast<std::size_t>(previousRoundRobinHint)
            : std::size_t { 0 };
        applyDrumMapMetadata(
            note,
            validDrumMap,
            retainedRoundRobin);
    }
    sortMidiNotes(clip);
    return true;
}

bool resizeMidiNotes(MidiClip& clip,
                     const std::vector<juce::String>& noteIds,
                     double deltaBeats,
                     double minimumDurationBeats)
{
    const auto indices = selectedIndices(clip, noteIds);
    if (indices.empty())
        return false;
    auto changed = false;
    for (const auto index : indices)
    {
        auto& note = clip.notes[index];
        const auto duration = juce::jlimit(
            minimumDurationBeats,
            std::max(minimumDurationBeats,
                     clip.durationBeats - note.actualStartBeats()),
            note.durationBeats + deltaBeats);
        if (std::abs(duration - note.durationBeats) < 0.0000001)
            continue;
        note.durationBeats = duration;
        for (auto& expression : note.expressions)
            expression.offsetBeats = std::min(
                expression.offsetBeats,
                note.durationBeats);
        changed = true;
    }
    return changed;
}

bool deleteMidiNotes(MidiClip& clip,
                     const std::vector<juce::String>& noteIds)
{
    const auto oldSize = clip.notes.size();
    clip.notes.erase(
        std::remove_if(
            clip.notes.begin(),
            clip.notes.end(),
            [&noteIds](const auto& note)
            {
                return std::find(
                           noteIds.cbegin(),
                           noteIds.cend(),
                           note.id)
                    != noteIds.cend();
            }),
        clip.notes.end());
    return clip.notes.size() != oldSize;
}

bool setMidiLaneValue(MidiClip& clip,
                      const std::vector<juce::String>& noteIds,
                      MidiEditorLane lane,
                      double normalizedValue,
                      double expressionBeat,
                      MidiExpressionType expressionType)
{
    const auto indices = selectedIndices(clip, noteIds);
    if (indices.empty())
        return false;
    normalizedValue = juce::jlimit(0.0, 1.0, normalizedValue);
    for (const auto index : indices)
    {
        auto& note = clip.notes[index];
        switch (lane)
        {
            case MidiEditorLane::velocity:
                note.velocity = juce::jlimit(
                    1,
                    127,
                    static_cast<int>(std::llround(
                        1.0 + normalizedValue * 126.0)));
                break;
            case MidiEditorLane::timing:
            {
                note.timingOffsetBeats =
                    (normalizedValue * 2.0 - 1.0) * 0.125;
                note.timingOffsetBeats = juce::jlimit(
                    -note.startBeats,
                    clip.durationBeats
                        - note.startBeats
                        - note.durationBeats,
                    note.timingOffsetBeats);
                break;
            }
            case MidiEditorLane::duration:
                note.durationBeats = juce::jlimit(
                    1.0 / 128.0,
                    std::max(
                        1.0 / 128.0,
                        clip.durationBeats - note.actualStartBeats()),
                    1.0 / 128.0
                        + normalizedValue
                            * std::max(
                                0.0,
                                clip.durationBeats
                                    - note.actualStartBeats()
                                    - 1.0 / 128.0));
                for (auto& expression : note.expressions)
                    expression.offsetBeats = std::min(
                        expression.offsetBeats,
                        note.durationBeats);
                break;
            case MidiEditorLane::probability:
                note.probability = normalizedValue;
                break;
            case MidiEditorLane::expression:
            {
                const auto offset = juce::jlimit(
                    0.0,
                    note.durationBeats,
                    expressionBeat - note.actualStartBeats());
                auto point = expressionPoint(
                    expressionType,
                    offset,
                    normalizedValue);
                const auto existing = std::find_if(
                    note.expressions.begin(),
                    note.expressions.end(),
                    [&point](const auto& candidate)
                    {
                        return candidate.type == point.type
                            && candidate.controller == point.controller
                            && std::abs(
                                   candidate.offsetBeats
                                   - point.offsetBeats)
                                < 1.0 / 128.0;
                    });
                if (existing != note.expressions.end())
                {
                    existing->offsetBeats = point.offsetBeats;
                    existing->value = point.value;
                }
                else
                {
                    note.expressions.push_back(std::move(point));
                }
                std::stable_sort(
                    note.expressions.begin(),
                    note.expressions.end(),
                    [](const auto& left, const auto& right)
                    {
                        return left.offsetBeats < right.offsetBeats;
                    });
                break;
            }
        }
    }
    sortMidiNotes(clip);
    return true;
}

void regenerateMidiClipIds(MidiClip& clip)
{
    clip.id = juce::Uuid().toString();
    for (auto& note : clip.notes)
    {
        note.id = juce::Uuid().toString();
        for (auto& expression : note.expressions)
            expression.id = juce::Uuid().toString();
    }
}

void sortMidiNotes(MidiClip& clip)
{
    std::stable_sort(
        clip.notes.begin(),
        clip.notes.end(),
        [](const auto& left, const auto& right)
        {
            if (std::abs(left.actualStartBeats()
                         - right.actualStartBeats())
                > 0.0000001)
                return left.actualStartBeats() < right.actualStartBeats();
            if (left.pitch != right.pitch)
                return left.pitch < right.pitch;
            return left.id < right.id;
        });
}

std::optional<MidiCaptureConversion> convertCapturedMidiToClip(
    const Project& project,
    const CapturedMidiWindow& captured,
    std::int64_t captureStartStreamSample,
    double timelineStartSeconds,
    double captureDurationSeconds,
    double sampleRate,
    const DrumMap* drumMap,
    juce::String clipName,
    juce::String& error,
    const juce::String& targetTrackId)
{
    if (!std::isfinite(timelineStartSeconds)
        || !std::isfinite(captureDurationSeconds)
        || !std::isfinite(sampleRate)
        || timelineStartSeconds < 0.0
        || captureDurationSeconds <= 0.0
        || sampleRate <= 0.0)
    {
        error = "MIDI capture conversion requires a valid timeline range and sample rate.";
        return std::nullopt;
    }

    MidiCaptureConversion result;
    result.ignoredEvents = captured.unsupportedEvents;
    result.clip.name = clipName.trim().isNotEmpty()
        ? clipName.trim()
        : juce::String("Recorded MIDI");
    result.clip.startBeats = project.beatsAt(timelineStartSeconds);
    result.clip.durationBeats = std::max(
        1.0 / 128.0,
        project.beatsAt(timelineStartSeconds + captureDurationSeconds)
            - result.clip.startBeats);
    result.clip.drumMapId = drumMap != nullptr ? drumMap->id : juce::String();
    result.clip.editorMode = drumMap != nullptr
        ? MidiEditorMode::drums
        : MidiEditorMode::pianoRoll;

    std::array<std::vector<std::size_t>, 16 * 128> activeByKey;
    std::array<std::vector<std::size_t>, 16> activeByChannel;
    std::array<int, 16 * 128> footControlValues;
    footControlValues.fill(-1);
    std::array<bool, 128> mappedFootControllers {};
    if (drumMap != nullptr)
        for (const auto& entry : drumMap->entries)
        {
            if (entry.footControlCC < -1 || entry.footControlCC > 127)
            {
                error = "The drum map contains an invalid foot controller.";
                return std::nullopt;
            }
            if (entry.footControlCC >= 0)
                mappedFootControllers[static_cast<std::size_t>(entry.footControlCC)] = true;
        }
    const auto beatAtEvent = [&](const CapturedMidiEvent& event)
    {
        const auto seconds = static_cast<double>(
            std::max<std::int64_t>(
                0,
                event.streamSample - captureStartStreamSample))
            / sampleRate;
        return project.beatsAt(timelineStartSeconds + seconds)
            - result.clip.startBeats;
    };

    auto orderedEvents = captured.events;
    std::stable_sort(
        orderedEvents.begin(), orderedEvents.end(),
        [](const auto& left, const auto& right)
        {
            return left.streamSample < right.streamSample;
        });
    const auto targetKey = midiInputTrackKey(targetTrackId);
    for (const auto& event : orderedEvents)
    {
        if (event.targetTrackKey != 0 && event.targetTrackKey != targetKey)
            continue;
        if (event.streamSample < captureStartStreamSample
            || event.streamSample
                > captureStartStreamSample
                    + static_cast<std::int64_t>(
                        std::llround(captureDurationSeconds * sampleRate)))
            continue;
        const juce::MidiMessage message(
            event.data.data(),
            static_cast<int>(event.size),
            0.0);
        const auto channel = message.getChannel();
        if (channel < 1 || channel > 16)
        {
            ++result.ignoredEvents;
            continue;
        }
        const auto channelIndex = static_cast<std::size_t>(channel - 1);
        const auto relativeBeat = juce::jlimit(
            0.0,
            result.clip.durationBeats,
            beatAtEvent(event));
        if (message.isController()
            && mappedFootControllers[static_cast<std::size_t>(message.getControllerNumber())])
        {
            const auto controller = message.getControllerNumber();
            footControlValues[channelIndex * 128 + static_cast<std::size_t>(controller)]
                = message.getControllerValue();
            for (const auto noteIndex : activeByChannel[channelIndex])
            {
                auto& activeNote = result.clip.notes[noteIndex];
                const auto* entry = drumMap->entryForId(activeNote.drumMapEntryId);
                if (entry != nullptr && entry->footControlCC == controller)
                    addRecordedExpression(
                        activeNote, MidiExpressionType::controller, controller,
                        std::max(0.0, relativeBeat - activeNote.actualStartBeats()),
                        static_cast<double>(message.getControllerValue()) / 127.0);
            }
            continue;
        }
        if (message.isNoteOn())
        {
            if (relativeBeat >= result.clip.durationBeats)
            {
                ++result.ignoredEvents;
                continue;
            }
            MidiNote note;
            note.pitch = message.getNoteNumber();
            note.channel = channel;
            note.startBeats = relativeBeat;
            note.velocity = juce::jlimit(
                1,
                127,
                static_cast<int>(message.getVelocity()));
            note.durationBeats = std::min(
                0.25,
                std::max(
                    1.0 / 128.0,
                    result.clip.durationBeats - relativeBeat));
            const auto* entry = drumMap != nullptr ? drumMap->entryForPitch(note.pitch) : nullptr;
            applyDrumMapMetadata(note, drumMap);
            if (entry != nullptr && entry->footControlCC >= 0)
            {
                const auto value = footControlValues[
                    channelIndex * 128 + static_cast<std::size_t>(entry->footControlCC)];
                if (value >= 0)
                    note.footControlValue = value;
            }
            const auto index = result.clip.notes.size();
            result.clip.notes.push_back(std::move(note));
            activeByKey[channelIndex * 128
                        + static_cast<std::size_t>(
                            message.getNoteNumber())]
                .push_back(index);
            activeByChannel[channelIndex].push_back(index);
            continue;
        }
        if (message.isNoteOff())
        {
            auto& active = activeByKey[
                channelIndex * 128
                + static_cast<std::size_t>(message.getNoteNumber())];
            if (active.empty())
            {
                ++result.unmatchedNoteOffs;
                continue;
            }
            const auto noteIndex = active.back();
            active.pop_back();
            auto& note = result.clip.notes[noteIndex];
            note.durationBeats = std::max(
                1.0 / 128.0,
                relativeBeat - note.actualStartBeats());
            note.durationBeats = std::min(
                note.durationBeats,
                result.clip.durationBeats - note.actualStartBeats());
            note.releaseVelocity = message.getVelocity();
            auto& channelNotes = activeByChannel[channelIndex];
            channelNotes.erase(
                std::remove(
                    channelNotes.begin(),
                    channelNotes.end(),
                    noteIndex),
                channelNotes.end());
            continue;
        }

        auto targetIndex = std::optional<std::size_t> {};
        if (message.isAftertouch())
        {
            const auto& active = activeByKey[
                channelIndex * 128
                + static_cast<std::size_t>(message.getNoteNumber())];
            if (!active.empty())
                targetIndex = active.back();
        }
        else if (!activeByChannel[channelIndex].empty())
        {
            targetIndex = activeByChannel[channelIndex].back();
        }
        if (!targetIndex.has_value())
        {
            ++result.ignoredEvents;
            continue;
        }

        auto& note = result.clip.notes[*targetIndex];
        const auto expressionOffset = std::max(
            0.0,
            relativeBeat - note.actualStartBeats());
        if (message.isAftertouch())
        {
            addRecordedExpression(
                note,
                MidiExpressionType::pressure,
                -1,
                expressionOffset,
                static_cast<double>(message.getAfterTouchValue()) / 127.0);
        }
        else if (message.isChannelPressure())
        {
            addRecordedExpression(
                note,
                MidiExpressionType::channelPressure,
                -1,
                expressionOffset,
                static_cast<double>(message.getChannelPressureValue())
                    / 127.0);
        }
        else if (message.isPitchWheel())
        {
            addRecordedExpression(
                note,
                MidiExpressionType::pitchBend,
                -1,
                expressionOffset,
                (static_cast<double>(message.getPitchWheelValue()) - 8192.0)
                    / 8192.0);
        }
        else if (message.isController())
        {
            const auto controller = message.getControllerNumber();
            addRecordedExpression(
                note,
                controller == 74
                    ? MidiExpressionType::timbre
                    : MidiExpressionType::controller,
                controller == 74 ? -1 : controller,
                expressionOffset,
                static_cast<double>(message.getControllerValue()) / 127.0);
        }
        else
        {
            ++result.ignoredEvents;
        }
    }

    for (const auto& channelNotes : activeByChannel)
    {
        for (const auto noteIndex : channelNotes)
        {
            auto& note = result.clip.notes[noteIndex];
            note.durationBeats = std::max(
                1.0 / 128.0,
                result.clip.durationBeats
                    - note.actualStartBeats());
        }
    }
    for (auto& note : result.clip.notes)
    {
        note.durationBeats = std::min(
            note.durationBeats,
            result.clip.durationBeats - note.actualStartBeats());
        if (note.durationBeats <= 0.0)
            note.durationBeats = 1.0 / 128.0;
        for (auto& expression : note.expressions)
            expression.offsetBeats = juce::jlimit(
                0.0,
                note.durationBeats,
                expression.offsetBeats);
        std::stable_sort(
            note.expressions.begin(),
            note.expressions.end(),
            [](const auto& left, const auto& right)
            {
                return left.offsetBeats < right.offsetBeats;
            });
    }
    if (result.clip.notes.empty())
    {
        error = "The MIDI capture did not contain any complete or active notes.";
        return std::nullopt;
    }
    sortMidiNotes(result.clip);
    juce::String validationError;
    if (!MidiClip::fromVar(result.clip.toVar(), validationError).has_value())
    {
        error = "Recorded MIDI is invalid: " + validationError;
        return std::nullopt;
    }
    return result;
}
}

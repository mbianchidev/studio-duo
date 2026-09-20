#include "TestHarness.h"
#include "TestSuites.h"

#include "model/ProjectCommands.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace
{
bool closeTo(double left, double right)
{
    return std::abs(left - right) < 0.000001;
}

studio::Project sectionProject()
{
    auto project = studio::Project::createDefault();
    project.sections = {
        { "intro", "Intro", 0.0 },
        { "verse", "Verse", 4.0 },
        { "cue", "Cue", 6.0 },
        { "outro", "Outro", 8.0 }
    };
    return project;
}

juce::String snapshot(const studio::Project& project)
{
    return juce::JSON::toString(project.toVar());
}

template <typename Change>
const Change* ownedChange(const std::vector<Change>& changes,
                          const juce::String& sectionId)
{
    const auto found = std::find_if(
        changes.cbegin(), changes.cend(),
        [&sectionId](const auto& change)
        {
            return change.sectionId == sectionId;
        });
    return found != changes.cend() ? &*found : nullptr;
}

template <typename Change>
std::vector<Change> manualChanges(std::vector<Change> changes)
{
    std::erase_if(changes, [](const auto& change)
    {
        return change.sectionId.isNotEmpty();
    });
    return changes;
}

bool applySettings(studio::Project& project,
                   studio::CommandStack& commands,
                   const juce::String& sectionId,
                   studio::SectionTransportSettings settings)
{
    juce::String error;
    const auto applied = commands.perform(
        std::make_unique<studio::SetSectionTransportCommand>(
            sectionId, std::move(settings)),
        project, error);
    expect(applied, error.toRawUTF8());
    return applied;
}

void expectRejected(studio::Project& project,
                    studio::CommandStack& commands,
                    std::unique_ptr<studio::ProjectCommand> command,
                    const char* message)
{
    const auto before = snapshot(project);
    const auto couldUndo = commands.canUndo();
    const auto couldRedo = commands.canRedo();
    const auto undoName = commands.undoName();
    const auto redoName = commands.redoName();
    juce::String error;
    expect(!commands.perform(std::move(command), project, error)
               && error.isNotEmpty()
               && snapshot(project) == before
               && commands.canUndo() == couldUndo
               && commands.canRedo() == couldRedo
               && commands.undoName() == undoName
               && commands.redoName() == redoName,
           message);
}

void defaultsAndClickInheritance()
{
    auto project = sectionProject();
    const auto& constProject = project;
    expect(studio::Project::currentFormatVersion == 10
               && project.timeSignatureNumerator == 4
               && project.timeSignatureDenominator == 4
               && project.meterAt(100.0).numerator == 4
               && project.meterAt(100.0).denominator == 4
               && closeTo(project.tempoAt(100.0), 120.0)
               && project.tempoChanges.empty()
               && project.meterChanges.empty(),
           "Generic sections keep the default 120 BPM and 4/4 meter without generating maps.");
    expect(project.findSection("verse") != nullptr
               && constProject.findSection("verse") != nullptr
               && constProject.findSection("missing") == nullptr,
           "Sections have mutable and const stable-ID lookup.");

    project.metronomeEnabled = false;
    project.metronomeSubdivision = 3;
    project.metronomeLevel = 0.4f;
    project.metronomeAccentLevel = 0.8f;
    const auto defaults = project.clickSettingsAt(3.0);
    expect(defaults.enabled
               && defaults.subdivision == 3
               && closeTo(defaults.level, 0.4f)
               && closeTo(defaults.accentLevel, 0.8f)
               && defaults.accentBeats == std::vector<int> { 1 },
           "Inherited click defaults use project controls and a local enabled flag independent of the global gate.");

    studio::SectionTransportSettings verse;
    verse.clickSettings = studio::SectionClickSettings {
        false, 4, 0.3f, 0.7f, { 1, 3, 7, 32 }
    };
    studio::CommandStack commands;
    if (!applySettings(project, commands, "verse", verse))
        return;
    expect(project.clickSettingsAt(3.999) == defaults
               && project.clickSettingsAt(4.0) == *verse.clickSettings
               && project.clickSettingsAt(6.0) == *verse.clickSettings
               && project.clickSettingsAt(7.9) == *verse.clickSettings
               && !project.metronomeEnabled,
           "Click overrides start at their section boundary and pass through generic markers without changing the global gate.");

    studio::SectionTransportSettings outro;
    outro.clickSettings = studio::SectionClickSettings {
        true, 2, 0.2f, 0.9f, {}
    };
    if (!applySettings(project, commands, "outro", outro))
        return;
    expect(project.clickSettingsAt(8.0) == *outro.clickSettings
               && project.clickSettingsAt(100.0).accentBeats.empty(),
           "An explicit empty accent pattern disables accents without disabling the click.");
    juce::String error;
    const auto generic = project.sectionTransportSettings("cue", error);
    expect(generic.has_value()
               && !generic->tempoBpm.has_value()
               && !generic->timeSignature.has_value()
               && !generic->clickSettings.has_value()
               && !generic->rampFromPrevious,
           "Section settings expose saved overrides, not inherited controls.");
    error.clear();
    expect(!project.sectionTransportSettings("missing", error).has_value()
               && error.isNotEmpty(),
           "Reading settings for an absent marker reports an explicit error.");
    expect(project.validateTransport(error),
           "Accent indices up to 32 remain valid even when the current meter has fewer beats.");
    expect(applySettings(project, commands, "outro", {})
               && project.clickSettingsAt(8.0) == *verse.clickSettings
               && commands.undo(project)
               && project.clickSettingsAt(8.0) == *outro.clickSettings
               && commands.redo(project, error)
               && project.clickSettingsAt(8.0) == *verse.clickSettings,
           "Clearing a click override restores inheritance and supports undo and redo.");
}

void sectionBoundariesAndRoundTrip()
{
    auto project = sectionProject();
    studio::SectionTransportSettings settings;
    settings.tempoBpm = 180.0;
    settings.timeSignature = studio::SectionTimeSignature { 7, 8 };
    settings.clickSettings = studio::SectionClickSettings {
        true, 4, 0.5f, 0.9f, { 1, 4, 6 }
    };
    studio::CommandStack commands;
    if (!applySettings(project, commands, "verse", settings))
        return;
    expect(closeTo(project.tempoAt(3.999), 120.0)
               && closeTo(project.tempoAt(4.0), 180.0)
               && project.meterAt(3.999).numerator == 4
               && project.meterAt(3.999).denominator == 4
               && project.meterAt(4.0).numerator == 7
               && project.meterAt(4.0).denominator == 8
               && project.meterAt(4.0).sectionId == "verse"
               && closeTo(project.beatsAt(4.0), 8.0)
               && closeTo(project.beatsAt(6.0), 14.0)
               && closeTo(project.secondsAtBeat(14.0), 6.0)
               && project.tempoChanges.size() == 1,
           "Owned section BPM and 7/8 changes affect authoritative time maps exactly at the boundary without a needless base anchor.");

    juce::String error;
    const auto loaded = studio::Project::fromVar(
        juce::JSON::parse(snapshot(project)), error);
    const auto* loadedSection = loaded.has_value()
        ? loaded->findSection("verse") : nullptr;
    const auto savedSettings = loaded.has_value()
        ? loaded->sectionTransportSettings("verse", error) : std::nullopt;
    expect(loaded.has_value()
               && loadedSection != nullptr
               && loadedSection->id == "verse"
               && loadedSection->name == "Verse"
               && closeTo(loadedSection->timeSeconds, 4.0)
               && loaded->tempoChanges == project.tempoChanges
               && loaded->meterChanges == project.meterChanges
               && savedSettings.has_value()
               && savedSettings->tempoBpm == settings.tempoBpm
               && savedSettings->timeSignature == settings.timeSignature
               && savedSettings->clickSettings == settings.clickSettings,
           "Format 10 round trips preserve section IDs, positions, owned maps, and every click option.");

    auto manualTempo = project.tempoChanges.front();
    auto manualMeter = project.meterChanges.front();
    manualTempo.sectionId.clear();
    manualMeter.sectionId.clear();
    expect(!(manualTempo == project.tempoChanges.front())
               && !(manualMeter == project.meterChanges.front())
               && !manualTempo.toVar().getDynamicObject()->hasProperty("sectionId")
               && !manualMeter.toVar().getDynamicObject()->hasProperty("sectionId")
               && !project.findSection("cue")->toVar()
                       .getDynamicObject()->hasProperty("clickSettings"),
           "Ownership participates in map equality while unowned and inherited JSON fields remain absent.");

    const auto before = studio::ProjectTransportState::fromProject(project);
    auto loop = before;
    loop.loopEnabled = true;
    loop.loopStartSeconds = 1.0;
    loop.loopEndSeconds = 7.0;
    loop.metronomeEnabled = false;
    expect(commands.perform(
               std::make_unique<studio::SetProjectTransportCommand>(before, loop),
               project, error)
               && project.loopEnabled
               && !project.metronomeEnabled
               && project.tempoChanges == before.tempoChanges
               && commands.undo(project)
               && !project.loopEnabled
               && commands.redo(project, error)
               && project.loopEnabled,
           "Global loop and click commands validate owned maps against the actual project's sections.");

    auto zeroProject = sectionProject();
    zeroProject.tempo = 132.0;
    zeroProject.timeSignatureNumerator = 5;
    studio::CommandStack zeroCommands;
    expect(applySettings(zeroProject, zeroCommands, "intro", settings)
               && closeTo(zeroProject.tempo, 132.0)
               && zeroProject.timeSignatureNumerator == 5
               && closeTo(zeroProject.tempoAt(0.0), 180.0)
               && applySettings(zeroProject, zeroCommands, "intro", {})
               && closeTo(zeroProject.tempoAt(0.0), 132.0)
               && zeroProject.meterAt(0.0).numerator == 5
               && zeroProject.meterAt(0.0).denominator == 4,
           "Setting and clearing a section at zero never changes the user's base tempo or time signature.");
}

void adoptionRampsAndHistory()
{
    auto project = sectionProject();
    project.tempoChanges = {
        { 0.0, 100.0, true },
        { 4.0, 160.0, true },
        { 10.0, 200.0, false }
    };
    project.meterChanges = { { 4.0, 5, 4 }, { 10.0, 3, 4 } };
    const auto originalTempo = project.tempoChanges;
    const auto originalMeter = project.meterChanges;
    juce::String error;
    const auto unowned = project.sectionTransportSettings("verse", error);
    expect(unowned.has_value()
               && !unowned->tempoBpm.has_value()
               && !unowned->timeSignature.has_value(),
           "Manual map points at a section boundary are not implicitly claimed.");

    studio::SectionTransportSettings settings;
    settings.tempoBpm = 180.0;
    settings.timeSignature = studio::SectionTimeSignature { 7, 8 };
    settings.clickSettings = studio::SectionClickSettings {};
    studio::CommandStack commands;
    if (!applySettings(project, commands, "verse", settings))
        return;
    const auto* tempo = ownedChange(project.tempoChanges, "verse");
    expect(tempo != nullptr
               && closeTo(tempo->bpm, 180.0)
               && tempo->rampToNext
               && !project.tempoChanges.front().rampToNext
               && project.tempoChanges.back() == originalTempo.back()
               && project.meterChanges.back() == originalMeter.back()
               && project.tempoChanges.size() == originalTempo.size()
               && project.meterChanges.size() == originalMeter.size(),
           "Explicit section settings adopt manual points, preserve outgoing ramps, and apply the incoming ramp to the predecessor.");
    const auto editedTempo = project.tempoChanges;
    const auto editedMeter = project.meterChanges;
    auto revised = settings;
    revised.tempoBpm = 190.0;
    revised.rampFromPrevious = true;
    expect(applySettings(project, commands, "verse", revised)
               && project.tempoChanges[1].rampToNext
               && project.tempoChanges.front().rampToNext
               && closeTo(project.tempoChanges[1].bpm, 190.0)
               && commands.undo(project)
               && project.tempoChanges == editedTempo,
           "Re-editing an owned tempo point preserves its outgoing ramp while independently changing its incoming ramp.");
    project.name = "Independent project edit";
    project.loopEnabled = true;
    expect(commands.undo(project)
               && project.tempoChanges == originalTempo
               && project.meterChanges == originalMeter
               && !project.findSection("verse")->clickSettings.has_value()
               && project.name == "Independent project edit"
               && project.loopEnabled
               && commands.redo(project, error)
               && project.tempoChanges == editedTempo
               && project.meterChanges == editedMeter
               && project.findSection("verse")->clickSettings == settings.clickSettings,
           "Atomic section undo and redo restore maps and click state without overwriting unrelated project state.");
    expect(applySettings(project, commands, "verse", {})
               && project.tempoChanges == manualChanges(editedTempo)
               && project.meterChanges == manualChanges(editedMeter)
               && !project.findSection("verse")->clickSettings.has_value()
               && commands.undo(project)
               && project.tempoChanges == editedTempo
               && project.meterChanges == editedMeter
               && commands.redo(project, error)
               && project.tempoChanges == manualChanges(editedTempo),
           "Clearing a section removes only owned points and its click override, with reversible map history.");

    auto rampProject = sectionProject();
    studio::SectionTransportSettings ramp;
    ramp.tempoBpm = 240.0;
    ramp.rampFromPrevious = true;
    studio::CommandStack rampCommands;
    if (!applySettings(rampProject, rampCommands, "verse", ramp))
        return;
    const auto readRamp = rampProject.sectionTransportSettings("verse", error);
    expect(rampProject.tempoChanges.size() == 2
               && closeTo(rampProject.tempoChanges.front().timeSeconds, 0.0)
               && closeTo(rampProject.tempoChanges.front().bpm, 120.0)
               && rampProject.tempoChanges.front().sectionId.isEmpty()
               && rampProject.tempoChanges.front().rampToNext
               && closeTo(rampProject.tempoAt(2.0), 180.0)
               && closeTo(rampProject.beatsAt(4.0), 12.0)
               && readRamp.has_value()
               && readRamp->rampFromPrevious
               && rampCommands.undo(rampProject)
               && rampProject.tempoChanges.empty()
               && rampCommands.redo(rampProject, error)
               && rampProject.tempoChanges.size() == 2,
           "An incoming ramp without a predecessor creates one reversible base anchor and uses existing ramp time math.");

    auto predecessorProject = sectionProject();
    predecessorProject.tempoChanges = { { 2.0, 90.0, false } };
    studio::CommandStack predecessorCommands;
    expect(applySettings(predecessorProject, predecessorCommands, "verse", ramp)
               && predecessorProject.tempoChanges.size() == 2
               && closeTo(predecessorProject.tempoChanges.front().timeSeconds, 2.0)
               && predecessorProject.tempoChanges.front().rampToNext,
           "Incoming ramps use the previous actual tempo point rather than a generic marker or an extra zero anchor.");

    auto nearbyProject = sectionProject();
    nearbyProject.tempoChanges = { { 4.00005, 150.0, true } };
    nearbyProject.meterChanges = { { 4.00005, 5, 4 } };
    const auto nearbyTempo = nearbyProject.tempoChanges;
    const auto nearbyMeter = nearbyProject.meterChanges;
    studio::CommandStack nearbyCommands;
    expect(applySettings(nearbyProject, nearbyCommands, "verse", settings)
               && manualChanges(nearbyProject.tempoChanges) == nearbyTempo
               && manualChanges(nearbyProject.meterChanges) == nearbyMeter,
           "Only exactly coincident manual points are adopted; nearby manual events remain authoritative and unchanged.");
}

void movingAndRemovingOwnedSections()
{
    auto project = sectionProject();
    project.tempoChanges = {
        { 0.0, 80.0, false }, { 2.0, 90.0, true },
        { 4.0, 150.0, true }, { 10.0, 140.0, false }
    };
    project.meterChanges = { { 0.0, 4, 4 }, { 10.0, 3, 4 } };
    studio::SectionTransportSettings settings;
    settings.tempoBpm = 170.0;
    settings.rampFromPrevious = true;
    settings.timeSignature = studio::SectionTimeSignature { 7, 8 };
    settings.clickSettings = studio::SectionClickSettings {
        false, 3, 0.3f, 1.0f, { 1, 5 }
    };
    studio::CommandStack commands;
    if (!applySettings(project, commands, "verse", settings))
        return;
    const auto original = *project.findSection("verse");
    const auto originalTempo = project.tempoChanges;
    const auto originalMeter = project.meterChanges;
    const auto manualTempo = manualChanges(originalTempo);
    const auto manualMeter = manualChanges(originalMeter);
    studio::SongSection moved { original.id, "Moved verse", 7.0 };
    juce::String error;
    expect(commands.perform(
               std::make_unique<studio::SetSongSectionCommand>(original, moved),
               project, error),
           "A configured section can move between generic markers.");
    const auto* tempo = ownedChange(project.tempoChanges, "verse");
    const auto* meter = ownedChange(project.meterChanges, "verse");
    expect(tempo != nullptr && meter != nullptr
               && closeTo(tempo->timeSeconds, 7.0)
               && closeTo(tempo->bpm, 170.0)
               && tempo->rampToNext
               && closeTo(meter->timeSeconds, 7.0)
               && meter->numerator == 7 && meter->denominator == 8
               && manualChanges(project.tempoChanges) == manualTempo
               && manualChanges(project.meterChanges) == manualMeter
               && project.findSection("verse")->clickSettings == settings.clickSettings
               && commands.undo(project)
               && project.tempoChanges == originalTempo
               && project.meterChanges == originalMeter
               && commands.redo(project, error),
           "Moving a section preserves owned values, outgoing ramps, click settings, and all manual points through undo and redo.");
    const auto movedTempo = project.tempoChanges;
    const auto movedMeter = project.meterChanges;
    expect(commands.perform(
               std::make_unique<studio::RemoveSongSectionCommand>("verse"),
               project, error)
               && project.findSection("verse") == nullptr
               && project.tempoChanges == manualTempo
               && project.meterChanges == manualMeter
               && commands.undo(project)
               && project.findSection("verse") != nullptr
               && project.findSection("verse")->clickSettings == settings.clickSettings
               && project.tempoChanges == movedTempo
               && project.meterChanges == movedMeter
               && commands.redo(project, error)
               && project.tempoChanges == manualTempo
               && project.meterChanges == manualMeter,
           "Removing a configured section removes only its owned points and restores all section state on undo.");

    const auto cue = *project.findSection("cue");
    auto movedCue = cue;
    movedCue.timeSeconds = 2.0;
    expect(commands.perform(
               std::make_unique<studio::SetSongSectionCommand>(cue, movedCue),
               project, error)
               && commands.perform(
                   std::make_unique<studio::RemoveSongSectionCommand>("cue"),
                   project, error)
               && commands.undo(project)
               && commands.undo(project)
               && project.tempoChanges == manualTempo
               && project.meterChanges == manualMeter,
           "Moving and removing generic markers never adopt, move, or remove coincident manual transport points.");
}

void collisionsAndInvalidSettings()
{
    auto project = sectionProject();
    project.tempoChanges = { { 9.0, 100.0, false } };
    project.meterChanges = { { 11.0, 3, 4 } };
    studio::SectionTransportSettings settings;
    settings.tempoBpm = 180.0;
    settings.timeSignature = studio::SectionTimeSignature { 7, 8 };
    studio::CommandStack commands;
    if (!applySettings(project, commands, "verse", settings))
        return;
    const auto original = *project.findSection("verse");
    auto renamed = original;
    renamed.name = "Renamed verse";
    juce::String error;
    expect(commands.perform(
               std::make_unique<studio::SetSongSectionCommand>(original, renamed),
               project, error)
               && commands.undo(project),
           "Invalid transport tests retain a redo branch.");
    for (const auto position : { 8.0, 9.0, 11.0, 8.00005 })
    {
        auto moved = original;
        moved.timeSeconds = position;
        expectRejected(project, commands,
                       std::make_unique<studio::SetSongSectionCommand>(original, moved),
                       "Marker, tempo, and meter collisions reject section movement without changing project or history.");
    }
    expectRejected(project, commands,
                   std::make_unique<studio::SetSectionTransportCommand>("missing", settings),
                   "Configuring an absent section fails without mutation.");

    const auto rejectSettings = [&](studio::SectionTransportSettings invalid)
    {
        expectRejected(project, commands,
                       std::make_unique<studio::SetSectionTransportCommand>("verse", std::move(invalid)),
                       "Invalid section transport values leave all project state and command history unchanged.");
    };
    for (const auto bpm : {
             19.0, 401.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity() })
    {
        auto invalid = settings;
        invalid.tempoBpm = bpm;
        rejectSettings(invalid);
    }
    for (const auto signature : {
             studio::SectionTimeSignature { 0, 4 },
             studio::SectionTimeSignature { 33, 4 },
             studio::SectionTimeSignature { 7, 3 } })
    {
        auto invalid = settings;
        invalid.timeSignature = signature;
        rejectSettings(invalid);
    }
    std::vector<studio::SectionClickSettings> invalidClicks;
    for (const auto subdivision : { 0, 9 })
    {
        studio::SectionClickSettings click;
        click.subdivision = subdivision;
        invalidClicks.push_back(click);
    }
    for (const auto level : {
             -0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity() })
    {
        studio::SectionClickSettings click;
        click.level = level;
        invalidClicks.push_back(click);
        click.level = 0.5f;
        click.accentLevel = level;
        invalidClicks.push_back(click);
    }
    for (const auto& accents : {
             std::vector<int> { 1, 1 }, std::vector<int> { 0 },
             std::vector<int> { 33 } })
    {
        studio::SectionClickSettings click;
        click.accentBeats = accents;
        invalidClicks.push_back(click);
    }
    for (const auto& click : invalidClicks)
    {
        error.clear();
        expect(!click.validate(error) && error.isNotEmpty(),
               "Invalid click patterns return validation errors.");
        auto invalid = settings;
        invalid.clickSettings = click;
        rejectSettings(invalid);
        studio::SongSection added { "invalid-click", "Invalid click", 12.0, click };
        expectRejected(project, commands,
                       std::make_unique<studio::AddSongSectionCommand>(added),
                       "Adding a marker validates its optional click settings without mutation.");
        error.clear();
        expect(!studio::SongSection::fromVar(added.toVar(), error).has_value()
                   && error.isNotEmpty(),
               "Serialized sections cannot bypass click validation.");
    }
    expect(commands.redo(project, error)
               && project.findSection("verse")->name == renamed.name,
           "All rejected section settings preserve the previous redo action.");

    auto foreignOwner = sectionProject();
    foreignOwner.tempoChanges = { { 4.0, 160.0, false, "outro" } };
    studio::CommandStack foreignCommands;
    expectRejected(foreignOwner, foreignCommands,
                   std::make_unique<studio::SetSectionTransportCommand>("verse", settings),
                   "A conflicting tempo point owned by another section is never adopted or overwritten.");
    foreignOwner.tempoChanges.clear();
    foreignOwner.meterChanges = { { 4.0, 3, 4, "outro" } };
    expectRejected(foreignOwner, foreignCommands,
                   std::make_unique<studio::SetSectionTransportCommand>("verse", settings),
                   "A conflicting meter point owned by another section rejects the entire combined edit.");
}

void serializedValidationAndLegacyMaps()
{
    const auto rejectProject = [](studio::Project project)
    {
        juce::String error;
        expect(!project.validateTransport(error) && error.isNotEmpty(),
               "Transport validation rejects missing, mismatched, and duplicate section ownership.");
        error.clear();
        expect(!studio::Project::fromVar(project.toVar(), error).has_value()
                   && error.isNotEmpty(),
               "Fully loaded projects enforce section-owned map references.");
    };
    for (const auto& owner : { juce::String("missing"), juce::String("verse") })
    {
        auto invalid = sectionProject();
        invalid.tempoChanges = { { 5.0, 150.0, false, owner } };
        rejectProject(invalid);
        invalid.tempoChanges.clear();
        invalid.meterChanges = { { 5.0, 7, 8, owner } };
        rejectProject(invalid);
    }
    auto duplicate = sectionProject();
    duplicate.tempoChanges = { { 4.00001, 150.0, false, "verse" } };
    rejectProject(duplicate);
    duplicate.tempoChanges = {
        { 4.0, 150.0, false, "verse" },
        { 4.00001, 170.0, false, "verse" }
    };
    rejectProject(duplicate);
    duplicate.tempoChanges.clear();
    duplicate.meterChanges = {
        { 4.0, 7, 8, "verse" }, { 4.00001, 5, 4, "verse" }
    };
    rejectProject(duplicate);
    auto invalidClick = sectionProject();
    invalidClick.findSection("verse")->clickSettings = studio::SectionClickSettings {};
    invalidClick.findSection("verse")->clickSettings->accentBeats = { 2, 2 };
    rejectProject(invalidClick);

    for (const auto& invalidAccent : {
             juce::var(1.5), juce::var("1"), juce::var(true),
             juce::var(static_cast<juce::int64>(4294967297LL)) })
    {
        auto value = studio::SectionClickSettings {}.toVar();
        juce::Array<juce::var> accents;
        accents.add(invalidAccent);
        // NamedValueSet treats some differently typed values as equivalent.
        value.getDynamicObject()->removeProperty("accentBeats");
        value.getDynamicObject()->setProperty("accentBeats", juce::var(accents));
        juce::String error;
        expect(!studio::SectionClickSettings::fromVar(value, error).has_value()
                   && error.isNotEmpty(),
               "Accent JSON requires bounded integers, not coercible strings, booleans, fractions, or overflowing integers.");
    }
    for (const auto& property : { "enabled", "subdivision", "level", "accentLevel", "accentBeats" })
    {
        auto value = studio::SectionClickSettings {}.toVar();
        value.getDynamicObject()->setProperty(property, "invalid");
        juce::String error;
        expect(!studio::SectionClickSettings::fromVar(value, error).has_value()
                   && error.isNotEmpty(),
               "Malformed click property types fail explicitly instead of silently using defaults.");
    }

    auto legacy = sectionProject();
    legacy.tempoChanges = { { 0.0, 105.0, true }, { 5.0, 165.0, false } };
    legacy.meterChanges = { { 0.0, 3, 4 }, { 5.0, 5, 8 } };
    const auto original = snapshot(legacy);
    juce::String error;
    const auto loaded = studio::Project::fromVar(
        juce::JSON::parse(original), error);
    expect(loaded.has_value()
               && loaded->tempoChanges == legacy.tempoChanges
               && loaded->meterChanges == legacy.meterChanges
               && closeTo(loaded->beatsAt(6.0), legacy.beatsAt(6.0))
               && closeTo(loaded->tempoAt(2.5), legacy.tempoAt(2.5))
               && loaded->meterAt(6.0) == legacy.meterAt(6.0)
               && !loaded->findSection("verse")->clickSettings.has_value(),
           "Legacy-style unowned maps and generic markers keep their existing time math and inheritance.");
    studio::CommandStack commands;
    expect(applySettings(legacy, commands, "verse", {})
               && snapshot(legacy) == original
               && commands.undo(legacy)
               && snapshot(legacy) == original,
           "Clearing a generic section does not claim or alter manual maps.");
}
}

void sectionTransportTests()
{
    defaultsAndClickInheritance();
    sectionBoundariesAndRoundTrip();
    adoptionRampsAndHistory();
    movingAndRemovingOwnedSections();
    collisionsAndInvalidSettings();
    serializedValidationAndLegacyMaps();
}

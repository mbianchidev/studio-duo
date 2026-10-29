#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "ui/TimelineComponent.h"
#include "ui/MasteringWorkspaceComponent.h"
#include "ui/AutomationPanel.h"
#include "ui/PluginParameterPanel.h"
#include "ui/RoutingPanel.h"
#include "ui/StudioTheme.h"
#include "ui/ClipProcessingEditorComponent.h"
#include "ui/MidiEditorComponent.h"
#include "plugin_host/PluginIsolationPolicy.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
void midiNoteKeyboardNavigation()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.type = studio::TrackType::midi;
    studio::MidiClip clip;
    clip.name = "Synthetic keyboard notes";
    studio::MidiNote late, early;
    late.id = "keyboard-note-late";
    late.startBeats = 0.9;
    late.timingOffsetBeats = 0.125;
    late.pitch = 72;
    early.id = "keyboard-note-early";
    early.startBeats = 1.0;
    early.timingOffsetBeats = -0.125;
    early.pitch = 60;
    clip.notes = { late, early };
    track.midiClips = { clip };
    studio::MidiEditorComponent editor;
    editor.setSize(1000, 500);
    editor.setVisible(false);
    editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    editor.setProject(&project);
    editor.setSelection(track.id, clip.id);
    int edits = 0;
    editor.onClipEdited = [&](const auto&, const auto&, const auto& after, const auto&)
    {
        ++edits;
        track.midiClips.front() = after;
        editor.setProject(&project);
        editor.setSelection(track.id, clip.id);
    };
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0))
               && edits == 0,
           "Alt+Right selects an existing MIDI note without moving it or requiring a mouse.");
    const auto selectionValue = [&]
    {
        const auto* handler = editor.getAccessibilityHandler();
        const auto* value = handler != nullptr ? handler->getValueInterface() : nullptr;
        expect(handler != nullptr && handler->getRole() == juce::AccessibilityRole::list
                   && handler->getTitle().contains(clip.name) && value != nullptr && value->isReadOnly(),
               "The MIDI grid exposes its clip identity and a native read-only selection value.");
        return value != nullptr ? value->getCurrentValueAsString() : juce::String();
    };
    auto announced = selectionValue();
    expect(announced.contains("MIDI note 60") && announced.contains("beat 0.875")
               && announced.contains("duration 0.250") && announced.contains("velocity 100")
               && announced.contains("channel 1") && announced.contains("probability 100.0%"),
           "The selected MIDI note announces its pitch, timing, duration, velocity, channel and probability.");
    const auto rangeModifiers = juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier;
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, rangeModifiers, 0))
               && edits == 0 && selectionValue().contains("2 notes selected"),
           "Alt+Shift+Right extends the MIDI selection without editing notes.");
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::leftKey, rangeModifiers, 0))
               && edits == 0 && selectionValue().contains("1 note selected"),
           "Reversing extended MIDI navigation contracts the range back to its anchor.");
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::altModifier, 0))
               && selectionValue().contains("MIDI note 60"),
           "MIDI note browsing clamps at the first note instead of wrapping.");
    int selectors = 0;
    for (auto* child : editor.getChildren())
        if (const auto* selector = dynamic_cast<const juce::ComboBox*>(child))
        {
            ++selectors;
            expect(selector->getTitle().isNotEmpty() && selector->getWantsKeyboardFocus(),
                   "Every MIDI lane/expression/grid/pattern/routing selector has a native name and keyboard focus.");
        }
    expect(selectors == 5, "The MIDI selector audit covers all five native selectors.");
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey))
               && edits == 1 && track.midiClips.front().notes.size() == 1
               && track.midiClips.front().notes.front().id == late.id,
           "Keyboard MIDI selection follows actual played beats, not persisted note order or unshifted start beats.");
    expect(selectionValue().containsIgnoreCase("no notes selected"),
           "Deleting MIDI notes clears stale selection metadata after the project refresh.");
    auto& remaining = track.midiClips.front().notes.front();
    remaining.velocity = 80;
    remaining.channel = 9;
    remaining.probability = 0.25;
    studio::MidiExpressionPoint pressure;
    pressure.offsetBeats = 0.2;
    pressure.value = 0.4;
    remaining.expressions = { pressure };
    editor.setProject(&project);
    editor.setSelection(track.id, clip.id);
    editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
    announced = selectionValue();
    expect(announced.contains("velocity 80") && announced.contains("channel 9")
               && announced.contains("probability 25.0%"),
           "A same-name MIDI selection announces changed values after a project refresh.");
    for (auto* child : editor.getChildren())
        if (auto* selector = dynamic_cast<juce::ComboBox*>(child);
            selector != nullptr && selector->getTitle() == "MIDI note property lane")
            selector->setSelectedId(5, juce::sendNotificationSync);
    editor.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0));
    editor.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0));
    const auto& expression = track.midiClips.front().notes.front().expressions.front();
    expect(edits == 3 && std::abs(expression.offsetBeats) < 1e-9 && std::abs(expression.value - 0.5) < 1e-9
               && selectionValue().contains("Poly pressure 0.500 at note beat 0.000"),
           "Repeated keyboard expression edits advance and announce the point at the note cursor, not a later unrelated point.");
    editor.setProject(nullptr);
    const auto* handler = editor.getAccessibilityHandler();
    expect(handler != nullptr && handler->getValueInterface() != nullptr
               && handler->getValueInterface()->getCurrentValueAsString() == "No MIDI clip selected",
           "An unavailable project clears the MIDI selection's native value.");
    expect(!editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey))
               && !editor.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey))
               && !editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)),
           "An unavailable MIDI clip does not consume note-editing keys as successful operations.");
}

void midiCursorKeyboardEntry()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.type = studio::TrackType::midi;
    track.midiClips.emplace_back();
    const auto clipId = track.midiClips.front().id;
    studio::MidiEditorComponent editor;
    editor.setSize(1000, 500);
    editor.setProject(&project);
    editor.setSelection(track.id, clipId);
    int edits = 0;
    editor.onClipEdited = [&](const auto&, const auto&, const auto& after, const auto&)
    {
        ++edits;
        track.midiClips.front() = after;
        editor.setProject(&project);
        editor.setSelection(track.id, clipId);
    };
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey))
               && editor.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && edits == 0,
           "Arrows position an empty MIDI grid's note-entry cursor without creating or moving a note.");
    editor.setProject(&project);
    editor.setSelection(track.id, clipId);
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    const auto& notes = track.midiClips.front().notes;
    expect(edits == 1 && notes.size() == 1 && std::abs(notes.front().startBeats - 0.25) < 1e-9
               && notes.front().pitch == 61,
           "Return creates a note at the keyboard cursor, preserved across ordinary same-clip refreshes.");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0));
    expect(edits == 3 && notes.size() == 1 && std::abs(notes.front().startBeats - 0.5) < 1e-9
               && std::abs(notes.front().durationBeats - 0.5) < 1e-9,
           "Existing arrow nudges and Shift+arrow resizing still edit the selected note.");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey))
               && editor.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && edits == 3,
           "Escape returns to cursor navigation without mutating the formerly selected note.");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(edits == 4 && notes.size() == 2 && notes.back().pitch == 60
               && std::abs(notes.back().startBeats - 0.75) < 1e-9,
           "Keyboard entry can create a second note at a different beat and pitch without a mouse.");
    expect(!editor.keyPressed(juce::KeyPress(juce::KeyPress::tabKey)),
           "MIDI note navigation never traps Tab.");
    studio::DrumMap map;
    map.name = "Synthetic keyboard kit";
    for (const auto pitch : { 36, 52, 60, 90 })
    {
        studio::DrumMapEntry entry;
        entry.noteNumber = pitch;
        map.entries.push_back(entry);
    }
    project.drumMaps.push_back(map);
    auto& drumClip = track.midiClips.front();
    drumClip.editorMode = studio::MidiEditorMode::drums;
    drumClip.drumMapId = map.id;
    drumClip.notes.clear();
    editor.setSelection(track.id, {});
    editor.setSelection(track.id, clipId);
    expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && edits == 4,
           "Empty drum-grid cursor movement follows mapped kit rows rather than unmapped semitones.");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(edits == 5 && notes.size() == 1 && notes.front().pitch == 52
               && notes.front().drumMapEntryId == map.entries[1].id,
           "Keyboard drum entry retains the chosen mapped voice and its ordinary drum metadata.");
}

void midiProtectedKeyboardShortcuts()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.type = studio::TrackType::midi;
    track.midiClips.emplace_back();
    track.midiClips.front().notes.emplace_back();
    studio::MidiEditorComponent editor;
    editor.setProject(&project);
    editor.setSelection(track.id, track.midiClips.front().id);
    editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
    int edits = 0;
    editor.onClipEdited = [&](const auto&, const auto&, const auto&, const auto&) { ++edits; };
    for (const auto& key : {
             juce::KeyPress(juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0),
             juce::KeyPress(juce::KeyPress::deleteKey, juce::ModifierKeys::commandModifier, 0),
             juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0),
             juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier, 0),
             juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier, 0),
             juce::KeyPress(juce::KeyPress::returnKey, juce::ModifierKeys::altModifier, 0),
             juce::KeyPress(juce::KeyPress::deleteKey, juce::ModifierKeys::altModifier, 0),
             juce::KeyPress(juce::KeyPress::escapeKey, juce::ModifierKeys::altModifier, 0) })
        expect(!editor.keyPressed(key) && edits == 0,
               "Unrelated workspace and assistive-technology chords never become destructive MIDI edits.");
}

void midiKeyboardSelectionVisibility()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.type = studio::TrackType::midi;
    studio::DrumMap map;
    for (int pitch = 0; pitch < 96; ++pitch)
    {
        studio::DrumMapEntry entry;
        entry.noteNumber = pitch;
        entry.name = "Synthetic voice " + juce::String(pitch);
        map.entries.push_back(entry);
    }
    project.drumMaps.push_back(map);
    studio::StudioTheme theme;
    const auto originalPalette = theme.palette();
    studio::MidiEditorComponent editor;
    editor.setLookAndFeel(&theme);
    for (const auto drums : { false, true })
    {
        studio::MidiClip clip;
        clip.editorMode = drums ? studio::MidiEditorMode::drums : studio::MidiEditorMode::pianoRoll;
        clip.drumMapId = drums ? map.id : juce::String();
        studio::MidiNote high, low;
        high.pitch = drums ? 95 : 127;
        high.startBeats = 2.0;
        low.pitch = 0;
        low.startBeats = 0.5;
        clip.notes = { high, low };
        track.midiClips = { clip };
        editor.setSelection(track.id, {});
        editor.setProject(&project);
        editor.setSelection(track.id, clip.id);
        for (const auto height : { 300, 900 })
        {
            editor.setSize(1000, height);
            for (const auto& preset : studio::studioThemePresets())
            {
                theme.applyPalette(preset.palette);
                editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
                for (const auto& note : { low, high })
                {
                    editor.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
                    juce::Image image(juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true, juce::SoftwareImageType());
                    juce::Graphics graphics(image);
                    editor.paint(graphics);
                    const auto labels = drums ? 190 : 66;
                    const auto x = labels + static_cast<int>((note.startBeats + note.durationBeats * 0.5)
                        / clip.durationBeats * (editor.getWidth() - labels));
                    int outlinePixels = 0;
                    auto outlineTop = image.getHeight();
                    auto outlineBottom = -1;
                    for (int y = 0; y < image.getHeight(); ++y)
                        if (image.getPixelAt(x, y) == juce::Colours::white)
                        {
                            ++outlinePixels;
                            outlineTop = std::min(outlineTop, y);
                            outlineBottom = y;
                        }
                    const auto gridBottom = height - std::min(92, std::max(48, (height - 70) / 3));
                    expect(outlinePixels >= 2 && outlineTop >= 70 && outlineBottom < gridBottom,
                           ("Keyboard selection renders the actual selected " + juce::String(drums ? "drum" : "piano")
                            + " note " + juce::String(note.pitch) + " within the grid at height "
                            + juce::String(height) + " in " + preset.name + ".").toRawUTF8());
                }
            }
        }
    }
    theme.applyPalette(originalPalette);
    editor.setLookAndFeel(nullptr);
}

void midiPointerSelectionDoesNotMoveNotes()
{
    auto project = studio::Project::createDefault();
    auto& track = project.tracks.front();
    track.type = studio::TrackType::midi;
    studio::DrumMap map;
    for (int pitch = 0; pitch < 96; ++pitch)
    {
        studio::DrumMapEntry entry;
        entry.noteNumber = pitch;
        map.entries.push_back(entry);
    }
    project.drumMaps.push_back(map);
    studio::MidiClip clip;
    clip.editorMode = studio::MidiEditorMode::drums;
    clip.drumMapId = map.id;
    studio::MidiNote first, partial;
    first.pitch = 95;
    partial.pitch = 87;
    partial.startBeats = 2.0;
    clip.notes = { first, partial };
    track.midiClips = { clip };
    studio::MidiEditorComponent editor;
    editor.setSize(1000, 300);
    editor.setProject(&project);
    editor.setSelection(track.id, clip.id);
    int edits = 0;
    editor.onClipEdited = [&](const auto&, const auto&, const auto& after, const auto&)
    {
        ++edits;
        track.midiClips.front() = after;
        editor.setProject(&project);
        editor.setSelection(track.id, clip.id);
    };
    const juce::Point<float> position { 620.0f, 218.0f };
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent down { juce::Desktop::getInstance().getMainMouseSource(), position, {},
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor, now, position, now, 1, false };
    const auto drag = down.withNewPosition(position);
    editor.mouseDown(down);
    editor.mouseDrag(drag);
    editor.mouseUp(drag);
    expect(edits == 0 && track.midiClips.front().notes.back().pitch == partial.pitch,
           "Selecting a partially visible drum note does not scroll beneath a stationary pointer and transpose it.");
}

void clipProcessingControls()
{
    studio::AudioClip clip;
    clip.name = "Synthetic clip";
    clip.durationSeconds = 2.0;
    clip.gainDecibels = -4.0f;
    clip.fadeInSeconds = 0.25;
    clip.fadeOutSeconds = 0.5;
    studio::StudioTheme theme;
    using Mode = studio::ClipProcessingEditorComponent::Mode;
    for (const auto mode : { Mode::gain, Mode::fadeIn, Mode::fadeOut })
    {
        studio::ClipProcessingEditorComponent editor(clip, mode);
        editor.setLookAndFeel(&theme);
        auto* value = dynamic_cast<juce::TextEditor*>(editor.findChildWithID("clip-edit.value"));
        auto* curve = dynamic_cast<juce::TextEditor*>(editor.findChildWithID("clip-edit.curve"));
        auto* status = dynamic_cast<juce::Label*>(editor.findChildWithID("clip-edit.status"));
        expect(value != nullptr && curve != nullptr && status != nullptr,
               "The non-drag clip editor uses real native value, curve and error controls.");
        if (value == nullptr || curve == nullptr || status == nullptr)
            continue;
        for (auto* control : { value, curve })
            expect(control->getTitle().isNotEmpty() && control->getWantsKeyboardFocus()
                       && editor.getLocalBounds().contains(control->getBounds()),
                   "Every clip edit field has a native name, keyboard focus and on-surface bounds.");
        auto changes = 0, closes = 0;
        double appliedValue = 0.0;
        float appliedCurve = 0.0f;
        editor.onApply = [&](double number, float shape)
        {
            ++changes;
            appliedValue = number;
            appliedCurve = shape;
            return juce::Result::ok();
        };
        editor.onClose = [&] { ++closes; };
        value->setText("nan", false);
        expect(editor.applyChanges().failed() && changes == 0 && closes == 0 && status->getText().contains("Error:"),
               "Invalid clip values remain open with a visible, non-color-only error and no partial edit.");
        value->setText(mode == Mode::gain ? "25" : "2.1", false);
        expect(editor.applyChanges().failed() && changes == 0, "Clip gain and fade duration bounds are enforced before dispatch.");
        value->setText(mode == Mode::gain ? "+3.25 dB" : "0.375", false);
        curve->setText("1.1", false);
        if (mode != Mode::gain)
            expect(editor.applyChanges().failed() && changes == 0, "Invalid fade curves cannot be silently clamped into a successful edit.");
        curve->setText("-0.5", false);
        expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) && changes == 1 && closes == 1
                   && std::abs(appliedValue - (mode == Mode::gain ? 3.25 : 0.375)) < 1e-9
                   && (mode == Mode::gain || std::abs(appliedCurve + 0.5f) < 1e-6f),
               "Return applies one complete precise gain/fade edit through the ordinary callback.");
        expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && changes == 1 && closes == 2
                   && !editor.keyPressed(juce::KeyPress(juce::KeyPress::tabKey)),
               "Escape closes without another mutation; Tab is not trapped.");
        if (mode == Mode::gain)
            expect(!curve->isVisible(), "Gain editing does not expose an unrelated fade control.");
        if (mode == Mode::fadeIn)
        {
            juce::Image image(juce::Image::ARGB, editor.getWidth(), editor.getHeight(), true, juce::SoftwareImageType());
            juce::Graphics graphics(image);
            editor.paintEntireComponent(graphics, true);
            expect(image.getPixelAt(2, 2).getARGB() == studio::StudioColours::panel,
                   "The precise clip editor renders through the native offscreen software path.");
            const auto destination = juce::SystemStats::getEnvironmentVariable("STUDIO_DUO_CLIP_EDIT_IMAGE", {});
            if (destination.isNotEmpty())
            {
                const auto stream = juce::File(destination).createOutputStream();
                expect(stream != nullptr && juce::PNGImageFormat().writeImageToStream(image, *stream),
                       "A synthetic clip editor preview can be retained without capturing or operating the desktop.");
            }
        }
        editor.setLookAndFeel(nullptr);
    }
    studio::AudioClip precise;
    precise.gainDecibels = 3.25f;
    precise.durationSeconds = precise.fadeInSeconds = 2.0 / 48000.0;
    precise.fadeInCurve = 0.12345678f;
    for (const auto mode : { Mode::gain, Mode::fadeIn })
    {
        studio::ClipProcessingEditorComponent editor(precise, mode);
        auto retained = false;
        editor.onApply = [&](double number, float shape)
        {
            const auto expected = mode == Mode::gain ? static_cast<double>(precise.gainDecibels) : precise.fadeInSeconds;
            retained = std::abs(number - expected) < std::numeric_limits<double>::epsilon()
                && (mode == Mode::gain || std::abs(shape - precise.fadeInCurve) < std::numeric_limits<float>::epsilon());
            return juce::Result::ok();
        };
        const auto result = editor.applyChanges();
        expect(result.wasOk() && retained,
               ("Opening and applying precise gain or a sample-length fade preserves its existing value without decimal rounding or a false duration-bound error (mode "
                + juce::String(static_cast<int>(mode)) + "): " + result.getErrorMessage()).toRawUTF8());
    }
}

void preciseSlipAndStretchControls()
{
    studio::AudioClip clip;
    clip.name = "Synthetic elastic clip";
    clip.sourceOffsetSeconds = 2.0;
    clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 12.0;
    clip.durationSeconds = 3.0;
    clip.playbackRate = 2.0;
    using Mode = studio::ClipProcessingEditorComponent::Mode;
    for (const auto mode : { Mode::slip, Mode::stretch })
    {
        studio::ClipProcessingEditorComponent editor(clip, mode);
        auto* value = dynamic_cast<juce::TextEditor*>(editor.findChildWithID("clip-edit.value"));
        auto* curve = editor.findChildWithID("clip-edit.curve");
        auto* status = dynamic_cast<juce::Label*>(editor.findChildWithID("clip-edit.status"));
        expect(value != nullptr && curve != nullptr && !curve->isVisible() && status != nullptr,
               "Precise slip/stretch use named native number fields without unrelated fade controls.");
        if (value == nullptr || status == nullptr)
            continue;
        int changes = 0, closes = 0;
        editor.onApply = [&](double, float) { ++changes; return juce::Result::ok(); };
        editor.onClose = [&] { ++closes; };
        for (const auto& invalid : { juce::String("nan"), mode == Mode::slip ? juce::String("-2.01") : juce::String("1.49"),
                                     mode == Mode::slip ? juce::String("4.01") : juce::String("24.01") })
        {
            value->setText(invalid, false);
            expect(editor.applyChanges().failed() && changes == 0 && closes == 0,
                   "Precise slip/stretch reject non-finite and out-of-source/rate values before dispatch.");
        }
        value->setText(mode == Mode::slip ? "1" : "6", false);
        editor.onApply = [](double, float) { return juce::Result::fail("Synthetic linked clip is out of range."); };
        expect(editor.applyChanges().failed() && closes == 0 && status->getText().contains("linked clip"),
               "A linked edit failure remains visible and keeps the precise editor open.");
        editor.onApply = [&](double number, float)
        {
            ++changes;
            expect(std::abs(number - (mode == Mode::slip ? 1.0 : 6.0)) < 1.0e-9,
                   "Precise slip/stretch dispatch the requested numeric value.");
            return juce::Result::ok();
        };
        expect(editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) && changes == 1 && closes == 1,
               "Return applies a successful precise slip/stretch once and closes its editor.");
    }
}

void automationPointKeyboardEditing()
{
    auto project = studio::Project::createDefault();
    const auto trackId = project.tracks.front().id;
    studio::AutomationLane volume;
    volume.name = "Synthetic volume";
    volume.target.trackId = trackId;
    volume.points = { { "volume-point", 0.0, 0.5 } };
    auto pan = volume;
    pan.id = "synthetic-pan-lane";
    pan.name = "Synthetic pan";
    pan.target.type = studio::AutomationTargetType::trackPan;
    pan.points = { { "pan-first", 0.125, 0.12345678901234567 },
                   { "pan-second", 2.0, 0.8 } };
    project.automationLanes = { volume, pan };
    studio::AutomationPanel panel;
    panel.setProject(&project);
    panel.setTrack(trackId);
    auto* lanes = dynamic_cast<juce::ComboBox*>(panel.findChildWithID("automation.lanes"));
    auto* points = dynamic_cast<juce::ComboBox*>(panel.findChildWithID("automation.points"));
    auto* position = dynamic_cast<juce::TextEditor*>(panel.findChildWithID("automation.position"));
    auto* value = dynamic_cast<juce::TextEditor*>(panel.findChildWithID("automation.value"));
    auto* apply = dynamic_cast<juce::Button*>(panel.findChildWithID("automation.apply"));
    auto* remove = dynamic_cast<juce::Button*>(panel.findChildWithID("automation.remove-selected"));
    auto* status = dynamic_cast<juce::Label*>(panel.findChildWithID("automation.status"));
    expect(lanes != nullptr && points != nullptr && position != nullptr && value != nullptr
               && apply != nullptr && remove != nullptr && status != nullptr,
           "Automation exposes native point selection, exact position/value, apply/remove and validation controls.");
    if (lanes == nullptr || points == nullptr || position == nullptr || value == nullptr
        || apply == nullptr || remove == nullptr || status == nullptr)
        return;

    studio::CommandStack commands;
    int edits = 0;
    panel.onUpdateLane = [&](auto before, auto after)
    {
        ++edits;
        juce::String error;
        const auto applied = commands.perform(
            std::make_unique<studio::SetAutomationLaneCommand>(std::move(before), std::move(after)),
            project, error);
        panel.refresh();
        return applied ? juce::Result::ok() : juce::Result::fail(error);
    };
    lanes->setSelectedItemIndex(1, juce::sendNotificationSync);
    expect(points->getNumItems() == 2 && points->getText().contains("0.125")
               && points->getText().contains("seconds"),
           "The selected lane exposes each automation point with its time unit and current value.");
    apply->onClick();
    expect(edits == 0 && juce::exactlyEqual(project.automationLanes.back().points.front().value, pan.points.front().value),
           "Applying unchanged automation retains full numeric precision and creates no undo edit.");
    points->setSelectedItemIndex(1, juce::sendNotificationSync);
    panel.refresh();
    expect(lanes->getSelectedItemIndex() == 1 && points->getSelectedItemIndex() == 1,
           "Refreshing automation retains lane and point identities instead of resetting to the first lane.");
    std::swap(project.automationLanes.front(), project.automationLanes.back());
    panel.refresh();
    expect(lanes->getSelectedItemIndex() == 0 && lanes->getText() == pan.name
               && points->getSelectedItemIndex() == 1,
           "Reordering lanes preserves the selected lane and point by ID, not their old list indices.");
    std::swap(project.automationLanes.front(), project.automationLanes.back());
    panel.refresh();
    position->setText("0.0625", false);
    value->setText("0.9876543210987654", false);
    apply->onClick();
    const auto& edited = project.automationLanes.back().points;
    expect(edits == 1 && edited.front().id == "pan-second"
               && edited.front().position == 0.0625 && juce::exactlyEqual(edited.front().value, 0.9876543210987654)
               && edited.back().id == "pan-first" && lanes->getSelectedItemIndex() == 1
               && points->getSelectedItemIndex() == 0,
           "Exact point edits sort chronologically while retaining the edited identity across synchronous refreshes.");
    expect(commands.undo(project)
               && project.automationLanes.back().points.back().id == "pan-second"
               && project.automationLanes.back().points.back().position == 2.0,
           "Keyboard automation point edits use the ordinary undoable project command.");
    panel.refresh();
    expect(points->getSelectedItemIndex() == 1,
           "Undo and refresh follow the selected point ID rather than its former sorted index.");
    remove->onClick();
    expect(edits == 2 && project.automationLanes.back().points.size() == 1
               && project.automationLanes.back().points.front().id == "pan-first"
               && project.automationLanes.front().points.front().id == "volume-point",
           "Remove selected deletes the chosen point, not the point nearest the unrelated playhead or another lane.");
    auto coincident = project.automationLanes.back().points.front();
    coincident.id = "coincident-point";
    coincident.value = 0.9;
    project.automationLanes.back().points.push_back(coincident);
    panel.refresh();
    points->setSelectedItemIndex(1, juce::sendNotificationSync);
    remove->onClick();
    expect(edits == 3 && project.automationLanes.back().points.size() == 1
               && project.automationLanes.back().points.front().id == "pan-first",
           "Coincident automation points remain individually selectable and removable by their own identities.");
}

void automationPointValidationAndTimebase()
{
    auto project = studio::Project::createDefault();
    project.tempo = 137.0;
    project.tempoChanges = { { 0.0, 137.0, true }, { 2.0, 193.0, false } };
    studio::AutomationLane lane;
    lane.target.trackId = project.tracks.front().id;
    lane.points = { { "precise-point", 1.0 / 48000.0, 0.12345678901234567 } };
    project.automationLanes = { lane };
    studio::AutomationPanel panel;
    panel.setProject(&project);
    panel.setTrack(lane.target.trackId);
    auto* points = dynamic_cast<juce::ComboBox*>(panel.findChildWithID("automation.points"));
    auto* position = dynamic_cast<juce::TextEditor*>(panel.findChildWithID("automation.position"));
    auto* value = dynamic_cast<juce::TextEditor*>(panel.findChildWithID("automation.value"));
    auto* apply = dynamic_cast<juce::Button*>(panel.findChildWithID("automation.apply"));
    auto* remove = dynamic_cast<juce::Button*>(panel.findChildWithID("automation.remove-selected"));
    auto* status = dynamic_cast<juce::Label*>(panel.findChildWithID("automation.status"));
    juce::Button* beatTime = nullptr;
    juce::Button* step = nullptr;
    juce::Button* addPoint = nullptr;
    juce::Slider* slider = nullptr;
    for (auto* child : panel.getChildren())
    {
        if (auto* button = dynamic_cast<juce::Button*>(child))
        {
            if (button->getTitle() == "Use beat-based automation time") beatTime = button;
            if (button->getTitle() == "Use step interpolation") step = button;
            if (button->getButtonText() == "ADD POINT") addPoint = button;
        }
        if (auto* candidate = dynamic_cast<juce::Slider*>(child)) slider = candidate;
    }
    expect(points != nullptr && position != nullptr && value != nullptr && apply != nullptr
               && remove != nullptr && status != nullptr && beatTime != nullptr && step != nullptr
               && addPoint != nullptr && slider != nullptr,
           "The automation validation fixture reaches every exact, slider, timebase and point action.");
    if (points == nullptr || position == nullptr || value == nullptr || apply == nullptr
        || remove == nullptr || status == nullptr || beatTime == nullptr || step == nullptr
        || addPoint == nullptr || slider == nullptr)
        return;
    studio::CommandStack commands;
    int edits = 0;
    const auto update = [&](auto before, auto after)
    {
        ++edits;
        juce::String error;
        return commands.perform(
            std::make_unique<studio::SetAutomationLaneCommand>(std::move(before), std::move(after)),
            project, error) ? juce::Result::ok() : juce::Result::fail(error);
    };
    panel.onUpdateLane = update;
    expect(panel.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) && edits == 0,
           "Return on an unchanged sample-precise point does not round its time or value or create an edit.");
    for (const auto* invalid : { "", "nan", "inf", "-0.1", "1second", "1 2", "1e999" })
    {
        position->setText(invalid, false);
        apply->onClick();
        expect(edits == 0 && status->getText().startsWith("Error:")
                   && position->getText() == invalid
                   && juce::exactlyEqual(project.automationLanes.front().points.front().position,
                                         lane.points.front().position),
               "Invalid automation positions remain visible and cannot dispatch or partially mutate the lane.");
    }
    position->setText("0.5", false);
    for (const auto* invalid : { "", "nan", "inf", "-0.001", "1.001", "0.1tail" })
    {
        value->setText(invalid, false);
        apply->onClick();
        expect(edits == 0 && status->getText().contains("0 to 1") && value->getText() == invalid,
               "Exact automation values reject incomplete, non-finite and out-of-range input instead of clamping it.");
    }
    value->setText("0.8", false);
    if (value->onTextChange)
        value->onTextChange();
    expect(std::abs(slider->getValue() - 0.8) < 1e-12,
           "A valid exact value synchronizes the native slider without changing the saved point.");
    panel.onUpdateLane = [](auto, auto) { return juce::Result::fail("Synthetic frozen lane."); };
    apply->onClick();
    expect(edits == 0 && status->getText().contains("Synthetic frozen lane")
               && position->getText() == "0.5" && value->getText() == "0.8",
           "A rejected project command retains its precise inputs and reports the actual failure.");
    beatTime->setToggleState(true, juce::dontSendNotification);
    beatTime->onClick();
    expect(!beatTime->getToggleState() && project.automationLanes.front().timebase == studio::AutomationTimebase::seconds,
           "A rejected timebase change restores the actual lane setting rather than leaving a false beat-time state.");
    step->setToggleState(true, juce::dontSendNotification);
    step->onClick();
    expect(!step->getToggleState() && project.automationLanes.front().interpolation == studio::AutomationInterpolation::linear,
           "A rejected interpolation change restores the actual setting without pretending the edit succeeded.");
    panel.onUpdateLane = update;
    expect(panel.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)) && edits == 1
               && status->getText().isEmpty() && project.automationLanes.front().points.front().position == 0.5,
           "Return retries the retained exact inputs through the ordinary command after a failure.");
    for (const auto modifiers : std::array<int, 5> {
             juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier,
             juce::ModifierKeys::altModifier, juce::ModifierKeys::shiftModifier,
             juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier })
        expect(!panel.keyPressed(juce::KeyPress(juce::KeyPress::returnKey, modifiers, 0)) && edits == 1,
               "Modified Return chords remain available to workspace and assistive-technology commands.");
    expect(!panel.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))
               && !panel.keyPressed(juce::KeyPress(juce::KeyPress::tabKey)),
           "Exact automation editing never traps Escape dismissal or Tab navigation.");

    const auto expectedBeat = project.beatsAt(0.5);
    beatTime->setToggleState(true, juce::dontSendNotification);
    beatTime->onClick();
    expect(edits == 2 && points->getText().contains("beats") && position->getTitle().contains("beats")
               && std::abs(project.automationLanes.front().points.front().position - expectedBeat) < 1e-12,
           "Changing timebase retains the selected point and its timeline time across a tempo ramp.");
    position->setText("3.125", false);
    value->setText("0.3", false);
    panel.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(edits == 3 && project.automationLanes.front().points.front().position == 3.125,
           "Exact beat-lane positions are edited in beats, not silently interpreted as seconds.");
    panel.setPositionSeconds(0.75);
    value->setText("0.7654321987654321", false);
    addPoint->onClick();
    const auto& added = project.automationLanes.front().points.front();
    expect(edits == 4 && project.automationLanes.front().points.size() == 2
               && added.id != "precise-point" && points->getSelectedItemIndex() == 0
               && std::abs(added.position - project.beatsAt(0.75)) < 1e-12
               && juce::exactlyEqual(added.value, 0.7654321987654321),
           "Add point keeps its playhead semantics, exact typed value, chosen lane and newly created selection.");
    remove->onClick();
    remove->onClick();
    expect(edits == 6 && project.automationLanes.front().points.empty()
               && !points->isEnabled() && !position->isEnabled() && !apply->isEnabled()
               && !remove->isEnabled() && addPoint->isEnabled(),
           "Removing the final point disables stale edit actions but retains keyboard point creation.");
    panel.setProject(nullptr);
    for (auto* child : panel.getChildren())
        if (dynamic_cast<juce::Button*>(child) != nullptr || dynamic_cast<juce::ComboBox*>(child) != nullptr
            || dynamic_cast<juce::Slider*>(child) != nullptr || dynamic_cast<juce::TextEditor*>(child) != nullptr)
            expect(!child->isEnabled(), "An unavailable project disables every automation editing control.");
    expect(!panel.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)),
           "An unavailable automation project cannot consume Return as a successful edit.");
}

void automationPointSampleRange()
{
    auto project = studio::Project::createDefault();
    studio::AutomationLane lane;
    lane.target.trackId = project.tracks.front().id;
    lane.points = { { "bounded-point", 0.125, 0.5 } };
    for (const auto sampleRate : { 44100.0, 48000.0, 192000.0 })
    {
        for (const auto timebase : { studio::AutomationTimebase::seconds, studio::AutomationTimebase::beats })
        {
            lane.timebase = timebase;
            project.automationLanes = { lane };
            studio::AutomationPanel panel(sampleRate);
            panel.setProject(&project);
            panel.setTrack(lane.target.trackId);
            auto* position = dynamic_cast<juce::TextEditor*>(panel.findChildWithID("automation.position"));
            auto* apply = dynamic_cast<juce::Button*>(panel.findChildWithID("automation.apply"));
            auto* status = dynamic_cast<juce::Label*>(panel.findChildWithID("automation.status"));
            if (position == nullptr || apply == nullptr || status == nullptr)
            {
                expect(false, "The bounded automation point fixture has exact editing controls.");
                continue;
            }
            int edits = 0;
            double dispatched = 0.0;
            panel.onUpdateLane = [&](auto, auto after)
            {
                ++edits;
                dispatched = after.points.front().position;
                return juce::Result::ok();
            };
            position->setText("1e100", false);
            apply->onClick();
            expect(edits == 0 && position->getText() == "1e100" && status->getText().contains("sample range"),
                   "Finite second/beat positions outside the signed audio sample range are rejected before reaching the scheduler.");
            if (timebase != studio::AutomationTimebase::seconds)
                continue;
            const auto boundary = static_cast<double>(std::numeric_limits<std::int64_t>::max()) / sampleRate;
            const auto outside = std::nextafter(boundary, std::numeric_limits<double>::infinity());
            position->setText(juce::String(outside, 17), false);
            apply->onClick();
            expect(edits == 0 && status->getText().contains("sample range"),
                   "A representable time immediately outside the signed sample boundary cannot overflow llround.");
            const auto inside = std::nextafter(boundary, 0.0);
            position->setText(juce::String(inside, 17), false);
            apply->onClick();
            expect(edits == 1 && juce::exactlyEqual(dispatched, inside),
                   "A representable time immediately inside the signed sample boundary remains editable at each tested audio rate.");
        }
    }
}

void automationPointNativeControls()
{
    auto project = studio::Project::createDefault();
    studio::AutomationLane lane;
    lane.name = "Synthetic automation";
    lane.target.trackId = project.tracks.front().id;
    lane.points = { { "native-point", 2.125, 0.12345678901234567 } };
    project.automationLanes = { lane };
    studio::StudioTheme theme;
    const auto originalPalette = theme.palette();
    for (const auto& preset : studio::studioThemePresets())
    {
        theme.applyPalette(preset.palette);
        studio::AutomationPanel panel;
        panel.setLookAndFeel(&theme);
        panel.setVisible(false);
        panel.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        panel.setProject(&project);
        panel.setTrack(lane.target.trackId);
        int controls = 0;
        for (auto* child : panel.getChildren())
        {
            if (dynamic_cast<juce::Button*>(child) == nullptr && dynamic_cast<juce::ComboBox*>(child) == nullptr
                && dynamic_cast<juce::Slider*>(child) == nullptr && dynamic_cast<juce::TextEditor*>(child) == nullptr)
                continue;
            ++controls;
            const auto* handler = child->getAccessibilityHandler();
            expect(handler != nullptr && handler->getTitle().isNotEmpty()
                       && child->getWantsKeyboardFocus() && !child->getBounds().isEmpty()
                       && panel.getLocalBounds().contains(child->getBounds()),
                   ("Automation control '" + child->getTitle() + "' has a native name, keyboard focus and on-panel bounds in "
                    + preset.name + ".").toRawUTF8());
            if (const auto* editor = dynamic_cast<const juce::TextEditor*>(child))
            {
                const auto* text = handler != nullptr ? handler->getTextInterface() : nullptr;
                expect(handler != nullptr && handler->getRole() == juce::AccessibilityRole::editableText
                           && text != nullptr && !text->isReadOnly()
                           && text->getText({ 0, text->getTotalNumCharacters() }) == editor->getText(),
                       "Exact automation inputs expose their complete, unrounded text through the native editable-text interface.");
            }
        }
        expect(controls == 17, "The native automation audit includes every selector, editor, slider and action.");
        auto* position = dynamic_cast<juce::TextEditor*>(panel.findChildWithID("automation.position"));
        auto* apply = dynamic_cast<juce::Button*>(panel.findChildWithID("automation.apply"));
        auto* status = dynamic_cast<juce::Label*>(panel.findChildWithID("automation.status"));
        if (position != nullptr && apply != nullptr && status != nullptr)
        {
            position->setText("invalid", false);
            apply->onClick();
            const auto* handler = status->getAccessibilityHandler();
            const auto* value = handler != nullptr ? handler->getValueInterface() : nullptr;
            expect(value != nullptr && value->getCurrentValueAsString() == status->getText()
                       && status->getText().startsWith("Error:"),
                   "Automation validation exposes its actual non-color-only error through the native value interface.");
            project.automationLanes.front().points.clear();
            panel.refresh();
            const auto* disabled = position->getAccessibilityHandler();
            expect(disabled != nullptr && disabled->getRole() == juce::AccessibilityRole::staticText,
                   "An empty lane exposes its unavailable position field as non-editable native text.");
            project.automationLanes.front().points = lane.points;
            panel.refresh();
            const auto* enabled = position->getAccessibilityHandler();
            expect(enabled != nullptr && enabled->getRole() == juce::AccessibilityRole::editableText,
                   "Restoring a point invalidates a cached disabled text role so assistive clients can edit it again.");
            panel.setProject(nullptr);
            panel.setProject(&project);
            const auto* exactValue = panel.findChildWithID("automation.value")->getAccessibilityHandler();
            expect(exactValue != nullptr && exactValue->getRole() == juce::AccessibilityRole::editableText,
                   "Reopening an available project restores the exact value field's native editable role too.");
        }
        juce::Image image(juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics graphics(image);
        panel.paintEntireComponent(graphics, true);
        expect(image.getPixelAt(2, 2).getARGB() == studio::StudioColours::panel,
               "The complete automation editor renders using the native offscreen software path in every theme.");
        const auto destination = juce::SystemStats::getEnvironmentVariable("STUDIO_DUO_AUTOMATION_IMAGE", {});
        if (destination.isNotEmpty() && juce::String(preset.id) == "studio-gray")
        {
            const auto stream = juce::File(destination).createOutputStream();
            expect(stream != nullptr && juce::PNGImageFormat().writeImageToStream(image, *stream),
                   "The synthetic automation preview is retained without capturing or operating the desktop.");
        }
        panel.setLookAndFeel(nullptr);
    }
    theme.applyPalette(originalPalette);
}

void namedProductionControls()
{
    studio::PluginParameterPanel parameters("Synthetic processor", "synthetic-insert",
        { { 0, "gain", "Gain", 0.25f, true }, { 1, "meter", "Meter", 0.5f, false },
          { 2, "mode", "Mode", 0.0f, true, 3 } });
    juce::Slider* slider = nullptr;
    juce::ComboBox* selector = nullptr;
    for (auto* child : parameters.getChildren())
    {
        if (auto* candidate = dynamic_cast<juce::Slider*>(child))
            slider = candidate;
        if (auto* candidate = dynamic_cast<juce::ComboBox*>(child))
            selector = candidate;
    }
    expect(slider != nullptr && selector != nullptr,
           "The generic parameter editor exposes native selector and value controls.");
    if (slider != nullptr && selector != nullptr)
    {
        expect(selector->getTitle().isNotEmpty() && selector->getWantsKeyboardFocus()
                   && slider->getTitle().contains("Gain") && slider->getWantsKeyboardFocus(),
               "Generic plugin controls have accessible names and explicit keyboard focus.");
        int starts = 0, changes = 0, ends = 0;
        parameters.onGestureStarted = [&](const auto&, const auto&, const auto&, int, float) { ++starts; };
        parameters.onValueChanged = [&](const auto&, const auto&, const auto&, int, float) { ++changes; };
        parameters.onGestureEnded = [&](const auto&, const auto&, const auto&, int, float) { ++ends; };
        slider->setValue(0.75, juce::sendNotificationSync);
        expect(starts == 1 && changes == 1 && ends == 1,
               "Non-drag parameter edits retain the complete automation gesture.");
        selector->setSelectedId(2, juce::sendNotificationSync);
        expect(slider->getTitle().contains("Meter") && !slider->isEnabled(),
               "Parameter selection updates its accessible identity and preserves read-only values.");
        selector->setSelectedId(3, juce::sendNotificationSync);
        juce::Component& sliderComponent = *slider;
        expect(slider->getInterval() == 0.5
                   && sliderComponent.keyPressed(juce::KeyPress(juce::KeyPress::rightKey))
                   && std::abs(slider->getValue() - 0.5) < 1e-6
                   && starts == 2 && changes == 2 && ends == 2,
               "Keyboard editing moves a discrete plugin parameter by one real step, not an inaudible fractional increment.");
    }
    studio::PluginParameterPanel empty("Unavailable", "empty", {});
    for (auto* child : empty.getChildren())
        if (dynamic_cast<juce::Slider*>(child) != nullptr || dynamic_cast<juce::ComboBox*>(child) != nullptr)
            expect(!child->isEnabled(), "An empty parameter editor cannot present a working-looking control.");

    studio::AutomationPanel automation;
    auto namedControls = 0;
    for (auto* child : automation.getChildren())
        if (dynamic_cast<juce::ComboBox*>(child) != nullptr || dynamic_cast<juce::Slider*>(child) != nullptr)
        {
            ++namedControls;
            expect(child->getTitle().isNotEmpty() && (!child->isEnabled() || child->getWantsKeyboardFocus()),
                   "Every automation mode/lane/target/value control is named and keyboard-reachable when available.");
        }
    expect(namedControls == 5, "The automation audit covers all five selector/slider controls.");

    auto project = studio::Project::createDefault();
    studio::RoutingPanel routing;
    routing.setProject(&project);
    routing.setTrack(project.tracks.front().id);
    juce::Button* edit = nullptr;
    for (auto* child : routing.getChildren())
        if (auto* button = dynamic_cast<juce::Button*>(child);
            button != nullptr && button->getTitle() == "Edit existing routes")
            edit = button;
    expect(edit != nullptr && !edit->isEnabled(),
           "Existing routes have a native keyboard action, disabled when no routes are available.");
    auto route = release_fixture::mainRoute("keyboard-send", project.tracks.front().id, project.masterTrackId());
    route.kind = studio::RouteKind::send;
    project.routingConnections.push_back(route);
    routing.setProject(&project);
    expect(edit != nullptr && edit->isEnabled() && edit->getWantsKeyboardFocus(),
           "Adding a send enables its keyboard editing alternative.");
    routing.setProject(nullptr);
    expect(edit != nullptr && !edit->isEnabled(),
           "Removing the project disables route editing instead of leaving a stale action.");
}

void isolationConsentPolicy()
{
    studio::PluginInsert insert;
    expect(!studio::requiresReducedIsolationConsent(insert, studio::PluginBridgeMode::sandboxed)
               && studio::requiresReducedIsolationConsent(insert, studio::PluginBridgeMode::trustedInProcess)
               && studio::requiresReducedIsolationConsent(insert, studio::PluginBridgeMode::araCompatibility),
           "Both third-party reduced-isolation modes require explicit consent, unlike sandboxed mode.");
    for (const auto mode : { studio::PluginBridgeMode::trustedInProcess,
                             studio::PluginBridgeMode::araCompatibility })
    {
        const auto warning = studio::reducedIsolationWarning(mode);
        expect(warning.contains("crash or hang") && warning.contains("recovery point")
                   && warning.contains("sandbox crash containment does not apply"),
               "Every reduced-isolation warning describes crash/hang risk and recovery without a containment claim.");
    }
    insert.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    expect(!studio::requiresReducedIsolationConsent(insert, insert.bridgeMode)
               && studio::requiresReducedIsolationConsent(insert, studio::PluginBridgeMode::araCompatibility),
           "An existing consent is mode-specific and does not authorize switching to ARA.");
    insert.bundledDevice = true;
    expect(!studio::requiresReducedIsolationConsent(insert, studio::PluginBridgeMode::araCompatibility),
           "Bundled DSP does not acquire a misleading third-party consent dialog.");
}

void themeContrastContracts()
{
    for (const auto& preset : studio::studioThemePresets())
    {
        juce::String error;
        expect(preset.palette.isAccessible(error),
               (juce::String(preset.name) + ": " + error).toRawUTF8());
        auto transparent = preset.palette;
        transparent.text &= 0x00ffffff;
        expect(!transparent.isAccessible(error) && error.contains("opaque"),
               "Transparent custom colours cannot bypass contrast validation.");
        auto unreadableTransport = preset.palette;
        unreadableTransport.transportRaised = unreadableTransport.text;
        expect(!unreadableTransport.isAccessible(error),
               "Raised transport surfaces participate in text contrast validation.");
    }
}

void masteringKeyboardControls()
{
    release_fixture::Directory directory;
    const auto media = directory.file.getChildFile("synthetic.wav");
    expect(release_fixture::writeMedia(media).wasOk(), "The mastering keyboard fixture media can be written.");
    auto project = release_fixture::exchangeProject(media);
    studio::MasteringWorkspaceComponent workspace;
    workspace.setProject(&project);
    juce::StringArray editorTitles;
    int buttons = 0;
    for (int index = 0; index < workspace.getNumChildComponents(); ++index)
    {
        auto* child = workspace.getChildComponent(index);
        if (const auto* button = dynamic_cast<juce::Button*>(child))
        {
            ++buttons;
            expect(!button->isEnabled() || button->getWantsKeyboardFocus(),
                   "Every mastering add/analyze/export/DDP/portable/repair control is keyboard-reachable.");
        }
        if (const auto* editor = dynamic_cast<juce::TextEditor*>(child))
        {
            expect(editor->getTitle().isNotEmpty() && !editorTitles.contains(editor->getTitle()),
                   "Mastering metadata and timing editors have distinct accessible names.");
            editorTitles.add(editor->getTitle());
        }
        if (const auto* selector = dynamic_cast<juce::ComboBox*>(child))
            expect(selector->getTitle().isNotEmpty(),
                   "The mastering source-mix selector has an accessible name.");
    }
    expect(buttons == 11 && editorTitles.size() == 18,
           "The keyboard audit covers every mastering action and metadata/timing field.");
}
}

void keyboardAccessibilityTests()
{
    midiNoteKeyboardNavigation();
    midiCursorKeyboardEntry();
    midiProtectedKeyboardShortcuts();
    midiKeyboardSelectionVisibility();
    midiPointerSelectionDoesNotMoveNotes();
    clipProcessingControls();
    preciseSlipAndStretchControls();
    automationPointKeyboardEditing();
    automationPointValidationAndTimebase();
    automationPointSampleRange();
    automationPointNativeControls();
    namedProductionControls();
    isolationConsentPolicy();
    themeContrastContracts();
    masteringKeyboardControls();
    studio::StudioIconButton activation(studio::StudioIcon::stop, "Stop", {});
    auto activations = 0;
    activation.onClick = [&] { ++activations; };
    juce::Component& activationComponent = activation;
    expect(activationComponent.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)),
           "Space activates a focused icon control instead of invoking global playback.");
    const auto activationDeadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (activations == 0 && juce::Time::getMillisecondCounterHiRes() < activationDeadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    expect(activations == 1, "Keyboard icon activation dispatches its ordinary action exactly once.");
    studio::Project project;
    project.tempo = 137.0;
    project.tempoChanges = { { 0.0, 137.0, true }, { 2.0, 193.0, false } };
    studio::Track first;
    first.id = "keyboard-a";
    first.name = "Keyboard audio";
    first.versionsCollapsed = true;
    studio::AudioClip late;
    late.id = "keyboard-late";
    late.name = "Late clip";
    late.startSeconds = 2.0;
    studio::AudioClip early;
    early.id = "keyboard-early";
    early.name = "Early clip";
    early.startSeconds = 0.5;
    first.clips = { late, early };
    studio::Track hidden;
    hidden.id = "keyboard-hidden";
    hidden.parentTrackId = first.id;
    hidden.versionNumber = 1;
    studio::Track second;
    second.id = "keyboard-b";
    second.name = "Keyboard instrument";
    second.type = studio::TrackType::instrument;
    studio::MidiClip midi;
    midi.id = "keyboard-midi";
    midi.name = "Keyboard MIDI";
    second.midiClips = { midi };
    studio::Track master;
    master.id = "keyboard-master";
    master.type = studio::TrackType::master;
    project.tracks = { first, hidden, second, master };

    studio::TimelineComponent timeline;
    timeline.setBounds(0, 0, 1200, 500);
    timeline.setVisible(false);
    timeline.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    timeline.setProject(&project);
    timeline.setSelection(first.id, {});
    juce::String trackId = first.id;
    juce::String clipId;
    timeline.onTrackSelected = [&](const auto& selected)
    {
        trackId = selected;
        clipId.clear();
        timeline.setSelection(trackId, clipId);
    };
    timeline.onClipSelected = [&](const auto& track, const auto& clip)
    {
        trackId = track;
        clipId = clip;
        timeline.setSelection(trackId, clipId);
    };

    expect(timeline.getWantsKeyboardFocus(),
           "The primary timeline is reachable by Tab.");
    const auto* accessibility = timeline.getAccessibilityHandler();
    expect(accessibility != nullptr && accessibility->getRole() == juce::AccessibilityRole::list
               && accessibility->getTitle().contains("Keyboard audio"),
           "The custom timeline exposes a native semantic role and selected-track name.");
    expect(accessibility != nullptr && accessibility->getValueInterface() != nullptr
               && accessibility->getValueInterface()->isReadOnly(),
           "The timeline exposes a read-only selection value to native accessibility clients.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::downKey)) && trackId == second.id,
           "Down selects the next visible track, skipping collapsed take lanes.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && clipId == midi.id,
           "Right selects an ordinary MIDI clip without requiring a mouse.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && trackId == first.id && clipId.isEmpty(),
           "Up returns to the prior visible track and clears the previous track's clip.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && clipId == early.id,
           "Clip navigation is chronological, not dependent on persisted array order.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && clipId == late.id,
           "Right selects the next audio clip.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && clipId == late.id,
           "Clip navigation clamps at the final clip instead of wrapping or losing selection.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)) && clipId == early.id,
           "Left selects the previous clip.");
    accessibility = timeline.getAccessibilityHandler();
    expect(accessibility != nullptr && accessibility->getTitle().contains(early.name),
           "Assistive technology can identify the selected audio clip.");

    double movedSeconds = -1.0;
    juce::String movedClip;
    timeline.onClipMoved = [&](const auto& clip, const auto& destination, double seconds)
    {
        movedClip = clip;
        movedSeconds = seconds;
        expect(destination == first.id, "A keyboard nudge preserves the destination track.");
    };
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0))
               && movedClip == early.id
               && std::abs(movedSeconds
                           - project.secondsAtBeat(project.beatsAt(early.startSeconds) + 0.25)) < 0.000001,
           "Shift+Right nudges the selected clip by the musical edit grid across a tempo ramp.");
    double soughtSeconds = -1.0;
    timeline.onSeek = [&](double seconds)
    {
        soughtSeconds = seconds;
        timeline.setPlayheadSeconds(seconds);
    };
    timeline.setPlayheadSeconds(0.5);
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0))
               && std::abs(soughtSeconds - project.secondsAtBeat(project.beatsAt(0.5) + 0.25)) < 0.000001,
           "Alt+Right moves the playhead by the musical edit grid.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::homeKey)) && soughtSeconds == 0.0,
           "Home reaches the beginning of the project.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::endKey))
               && std::abs(soughtSeconds - project.timelineEndSeconds()) < 0.000001,
           "End reaches the end of the project.");
    auto muteRequests = 0;
    auto soloRequests = 0;
    auto armRequests = 0;
    timeline.onTrackMute = [&](const auto& id) { muteRequests += id == first.id ? 1 : 0; };
    timeline.onTrackSolo = [&](const auto& id) { soloRequests += id == first.id ? 1 : 0; };
    timeline.onTrackArm = [&](const auto& id) { armRequests += id == first.id ? 1 : 0; };
    expect(timeline.keyPressed(juce::KeyPress('M', juce::ModifierKeys::altModifier, 0)) && muteRequests == 1,
           "Alt+M reaches the selected track's mute operation.");
    expect(timeline.keyPressed(juce::KeyPress('S', juce::ModifierKeys::altModifier, 0)) && soloRequests == 1,
           "Alt+S reaches solo without splitting the selected clip.");
    expect(timeline.keyPressed(juce::KeyPress('R', juce::ModifierKeys::altModifier, 0)) && armRequests == 1,
           "Alt+R reaches record arming.");
    project.tracks.front().muted = project.tracks.front().solo = project.tracks.front().armed = true;
    auto& announcedClip = project.tracks.front().clips[1];
    announcedClip.gainDecibels = -4.0f;
    announcedClip.fadeInSeconds = 0.125;
    timeline.setProject(&project);
    const auto announced = timeline.getAccessibilityHandler()->getValueInterface()->getCurrentValueAsString();
    expect(announced.containsIgnoreCase("muted") && announced.containsIgnoreCase("solo")
               && announced.containsIgnoreCase("armed") && announced.contains("-4.0 dB")
               && announced.contains("0.125"),
           "The native selection value announces current track/clip states and edit values, not just names or color.");
    float keyboardGain = -100.0f;
    timeline.onClipGainChanged = [&](const auto& id, float value)
    {
        expect(id == early.id, "A keyboard gain edit preserves the selected audio clip.");
        keyboardGain = value;
    };
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0))
               && std::abs(keyboardGain + 3.0f) < 1e-5f,
           "Alt+Up edits clip gain through the same callback as the drag gesture.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::downKey,
               juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier, 0))
               && std::abs(keyboardGain + 4.1f) < 1e-5f,
           "Alt+Shift+Down supplies precise non-drag clip gain steps.");
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && trackId == first.id && clipId.isEmpty(),
           "Escape clears clip selection without abandoning its track or trapping focus.");
    const auto activateMenuAction = [](const juce::PopupMenu& menu, const juce::String& title)
    {
        for (juce::PopupMenu::MenuItemIterator item(menu, true); item.next();)
            if (item.getItem().text == title && item.getItem().isEnabled && item.getItem().action)
            {
                item.getItem().action();
                return true;
            }
        return false;
    };
    timeline.onToggleTrackVersions = [&](const auto& id)
    {
        expect(id == first.id, "The keyboard version action targets the selected parent.");
        project.tracks.front().versionsCollapsed = false;
        timeline.setProject(&project);
    };
    expect(activateMenuAction(timeline.keyboardContextMenuForTesting(), "Expand versions")
               && !project.tracks.front().versionsCollapsed,
           "The actual keyboard context menu expands take lanes without a mouse.");
    studio::AudioClip takeClip;
    takeClip.id = "keyboard-take-clip";
    takeClip.name = "Keyboard take clip";
    project.tracks[1].clips = { takeClip };
    studio::CompRegion comp;
    comp.sourceTrackId = hidden.id;
    comp.durationSeconds = 0.5;
    project.tracks.front().compRegions.push_back(comp);
    timeline.setProject(&project);
    timeline.setSelection(hidden.id, takeClip.id);
    juce::String usedTake, compClip, clearedComp;
    timeline.onUseTake = [&](const auto& id) { usedTake = id; };
    timeline.onUseClipForComp = [&](const auto& id) { compClip = id; };
    timeline.onClearComp = [&](const auto& id) { clearedComp = id; };
    const auto takeMenu = timeline.keyboardContextMenuForTesting();
    expect(activateMenuAction(takeMenu, "Use this take as playlist")
               && activateMenuAction(takeMenu, "Use this clip range in comp")
               && activateMenuAction(takeMenu, "Clear parent comp")
               && usedTake == hidden.id && compClip == takeClip.id && clearedComp == first.id,
           "Keyboard take/comp actions dispatch the selected take, clip range and parent through their ordinary project callbacks.");
    auto preciseEditors = 0;
    for (juce::PopupMenu::MenuItemIterator item(takeMenu, true); item.next();)
        if (item.getItem().text == "Edit clip gain..." || item.getItem().text == "Edit fade in..."
            || item.getItem().text == "Edit fade out...")
            preciseEditors += item.getItem().isEnabled && static_cast<bool>(item.getItem().action) ? 1 : 0;
    expect(preciseEditors == 3, "The same keyboard menu exposes precise native gain and both fade-shape editors.");
    expect(!timeline.keyPressed(juce::KeyPress(juce::KeyPress::tabKey)),
           "The timeline does not trap Tab navigation.");
    expect(!timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0)),
           "Unrelated modified shortcuts are not swallowed by timeline navigation.");
    timeline.setSelection(second.id, midi.id);
    movedClip.clear();
    timeline.onClipMoved = [&](const auto& id, const auto& destination, double seconds)
    {
        movedClip = id;
        movedSeconds = seconds;
        expect(destination == second.id, "A MIDI nudge preserves its source track.");
    };
    expect(timeline.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0))
               && movedClip == midi.id
               && std::abs(movedSeconds - project.secondsAtBeat(midi.startBeats + 0.25)) < 0.000001,
           "MIDI clips share the musical keyboard-nudge workflow.");
    timeline.setProject(nullptr);
    expect(!timeline.keyPressed(juce::KeyPress(juce::KeyPress::downKey)),
           "An unavailable project cannot perform keyboard edits.");

    studio::Project scrollProject;
    for (int index = 0; index < 26; ++index)
    {
        studio::Track track;
        track.id = "scroll-track-" + juce::String(index);
        studio::AudioClip clip;
        clip.id = track.id + "-clip";
        clip.startSeconds = 60.0;
        track.clips = { clip };
        scrollProject.tracks.push_back(std::move(track));
    }
    studio::TimelineComponent scrolling;
    scrolling.setProject(&scrollProject);
    scrolling.setSize(scrolling.preferredWidth(600), scrolling.preferredHeight(300));
    juce::Viewport viewport;
    viewport.setBounds(0, 0, 600, 300);
    viewport.setViewedComponent(&scrolling, false);
    for (int index = 0; index < 26; ++index)
        expect(scrolling.keyPressed(juce::KeyPress(juce::KeyPress::downKey)),
               "Every visible track can be reached through keyboard navigation.");
    expect(scrolling.keyPressed(juce::KeyPress(juce::KeyPress::rightKey))
               && viewport.getViewPositionY() > 0 && viewport.getViewPositionX() > 0,
           "An offscreen keyboard selection is revealed horizontally and vertically.");
}

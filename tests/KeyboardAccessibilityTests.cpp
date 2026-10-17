#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "ui/TimelineComponent.h"
#include "ui/MasteringWorkspaceComponent.h"

#include <cmath>

namespace
{
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

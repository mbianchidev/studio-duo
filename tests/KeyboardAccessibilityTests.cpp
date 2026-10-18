#include "ReleaseFixtures.h"
#include "TestSuites.h"

#include "ui/TimelineComponent.h"
#include "ui/MasteringWorkspaceComponent.h"
#include "ui/AutomationPanel.h"
#include "ui/PluginParameterPanel.h"
#include "ui/RoutingPanel.h"
#include "ui/StudioTheme.h"
#include "ui/ClipProcessingEditorComponent.h"
#include "plugin_host/PluginIsolationPolicy.h"

#include <cmath>
#include <limits>

namespace
{
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
        expect(editor.applyChanges().wasOk() && retained,
               "Opening and applying precise gain or a sample-length fade preserves its existing value without decimal rounding or a false duration-bound error.");
    }
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
            expect(child->getTitle().isNotEmpty() && child->getWantsKeyboardFocus(),
                   "Every automation mode/lane/target/value control is named and keyboard-reachable.");
        }
    expect(namedControls == 4, "The automation audit covers all four selector/value controls.");

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
    clipProcessingControls();
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

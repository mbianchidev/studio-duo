#include "TestHarness.h"
#include "TestSuites.h"

#include "model/ProjectCommands.h"
#include "ui/TransportSettingsComponent.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
juce::Component* findControl(juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id)
        return &component;
    for (auto* child : component.getChildren())
        if (auto* found = findControl(*child, id))
            return found;
    return nullptr;
}

template <typename Control>
Control& control(juce::Component& component, const char* id)
{
    if (auto* found = dynamic_cast<Control*>(findControl(component, id)))
        return *found;
    throw std::runtime_error(std::string("Missing transport settings control: ") + id);
}

void select(juce::Component& component, const char* id, int value)
{
    control<juce::ComboBox>(component, id).setSelectedId(value, juce::sendNotificationSync);
}

void toggle(juce::Component& component, const char* id, bool value)
{
    control<juce::ToggleButton>(component, id).setToggleState(value, juce::sendNotificationSync);
}

void edit(juce::Component& component, const char* id, const char* value)
{
    control<juce::TextEditor>(component, id).setText(value, false);
}

bool near(double first, double second)
{
    return std::abs(first - second) < 1.0e-6;
}

studio::Project transportProject()
{
    auto project = studio::Project::createDefault();
    project.tempo = 120.0;
    project.timeSignatureNumerator = 4;
    project.timeSignatureDenominator = 4;
    project.tempoChanges.clear();
    project.meterChanges.clear();
    project.markers = {
        { "start-marker", "Boundary", 0.0 },
        { "middle-marker", "Section", 4.0 },
        { "end-marker", "Boundary", 8.0 }
    };
    project.sections = {
        { "start-marker", "Start", 0.0, {} },
        { "middle-marker", "Middle", 4.0, {} },
        { "end-marker", "End", 8.0, {} }
    };
    project.loopEnabled = false;
    project.loopStartSeconds = 0.0;
    project.loopEndSeconds = 8.0;
    project.metronomeEnabled = true;
    project.metronomeSubdivision = 1;
    project.metronomeLevel = 0.65f;
    project.metronomeAccentLevel = 1.0f;
    return project;
}

void applySection(studio::Project& project, const char* id,
                  const studio::SectionTransportSettings& settings)
{
    juce::String error;
    studio::SetSectionTransportCommand command(id, settings);
    expect(command.perform(project, error),
           ("Create a synthetic section transport fixture: " + error).toRawUTF8());
}

void loopSecondsAndPresets()
{
    auto project = transportProject();
    studio::LoopSettingsComponent component(project, 48000.0, juce::Range<double>(2.25, 6.75));
    juce::String error;
    auto value = component.settings(error);
    expect(value && !value->enabled && near(value->startSeconds, 0.0)
               && near(value->endSeconds, 8.0)
               && control<juce::ComboBox>(component, "loop.mode").getSelectedId() == 2,
           "A valid default loop prefers musical positions and preserves the disabled state.");
    select(component, "loop.mode", 1);
    edit(component, "loop.start", "1.25");
    edit(component, "loop.end", "5.75");
    value = component.settings(error);
    expect(value && !value->enabled && near(value->startSeconds, 1.25)
               && near(value->endSeconds, 5.75),
           "Disabled loops remain editable and return exact valid timeline boundaries.");
    toggle(component, "loop.enabled", true);
    value = component.settings(error);
    expect(value && value->enabled && near(value->startSeconds, 1.25),
           "Loop enable is explicit and does not replace the selected range.");
    select(component, "loop.mode", 2);
    value = component.settings(error);
    expect(value && near(value->startSeconds, 1.25) && near(value->endSeconds, 5.75),
           "Changing to musical notation converts rather than resets valid typed boundaries.");
    select(component, "loop.mode", 1);
    value = component.settings(error);
    expect(value && near(value->startSeconds, 1.25) && near(value->endSeconds, 5.75),
           "Returning to timeline seconds preserves the resolved musical range.");
    toggle(component, "loop.enabled", false);
    select(component, "loop.preset", 2);
    value = component.settings(error);
    expect(value && !value->enabled && near(value->startSeconds, 2.25)
               && near(value->endSeconds, 6.75),
           "The selected-clip preset preserves loop enable and resolves the supplied snapshot range.");
    select(component, "loop.preset", 1);
    value = component.settings(error);
    expect(value && !value->enabled && near(value->startSeconds, 0.0)
               && near(value->endSeconds, project.lengthSeconds()),
           "The whole-project preset covers the project without enabling playback.");

    studio::LoopSettingsComponent absentSelection(project, 48000.0);
    expect(!control<juce::ComboBox>(absentSelection, "loop.preset").isItemEnabled(2),
           "The selected-clip preset is disabled when no clip range was supplied.");
    select(absentSelection, "loop.preset", 2);
    expect(control<juce::Label>(absentSelection, "loop.error").getText().contains("Select a clip"),
           "A programmatic request for an unavailable preset reports a useful error.");
}

void loopMusicalMaps()
{
    auto precise = transportProject();
    precise.loopStartSeconds = 101.0 / 48000.0;
    precise.loopEndSeconds = 901.0 / 48000.0;
    studio::LoopSettingsComponent preciseComponent(precise, 48000.0);
    juce::String preciseError;
    select(preciseComponent, "loop.mode", 2);
    const auto preciseRange = preciseComponent.settings(preciseError);
    expect(preciseRange
               && control<juce::ComboBox>(preciseComponent, "loop.mode").getSelectedId() == 1
               && std::llround(preciseRange->startSeconds * 48000.0) == 101
               && std::llround(preciseRange->endSeconds * 48000.0) == 901,
           "Switching loop units cannot silently quantize sample-precise boundaries to musical ticks.");

    auto project = transportProject();
    project.tempoChanges = { { 0.0, 120.0, false, {} }, { 4.0, 60.0, false, {} } };
    project.meterChanges = { { 0.0, 4, 4, {} }, { 4.0, 3, 4, {} } };
    studio::LoopSettingsComponent component(project, 48000.0);
    select(component, "loop.mode", 2);
    edit(component, "loop.start", "3");
    edit(component, "loop.end", "4:1:0");
    juce::String error;
    auto value = component.settings(error);
    expect(value && near(value->startSeconds, 4.0) && near(value->endSeconds, 7.0),
           "Musical loop positions follow changing tempo and meter rather than a fixed bar length.");
    edit(component, "loop.start", "3:2:480");
    value = component.settings(error);
    expect(value && near(value->startSeconds, 5.5),
           "Musical positions resolve 960 ticks per beat under the active tempo map.");
    edit(component, "loop.start", "3:4");
    expect(!component.settings(error) && error.isNotEmpty(),
           "A beat beyond a 3/4 bar is rejected by the shared musical-position resolver.");
    edit(component, "loop.start", "3:2:960");
    expect(!component.settings(error), "Ticks outside 0..959 are rejected.");
}

void invalidLoopInputIsNotDiscarded()
{
    studio::LoopSettingsComponent component(transportProject(), 48000.0);
    select(component, "loop.mode", 1);
    edit(component, "loop.start", "not-a-time");
    edit(component, "loop.end", "4");
    select(component, "loop.mode", 2);
    juce::String error;
    expect(control<juce::ComboBox>(component, "loop.mode").getSelectedId() == 1
               && control<juce::TextEditor>(component, "loop.start").getText() == "not-a-time"
               && !component.settings(error)
               && control<juce::Label>(component, "loop.error").getText().isNotEmpty(),
           "Mode conversion refuses to discard malformed typed loop boundaries.");
    for (const auto* invalid : { "", "NaN", "Inf", "2seconds", "1,25", "1e999" })
    {
        edit(component, "loop.start", invalid);
        expect(!component.settings(error), "Loop seconds require a complete finite number.");
    }
    edit(component, "loop.start", "5");
    edit(component, "loop.end", "4");
    expect(!component.settings(error), "Reversed loop boundaries are rejected while looping is disabled.");
    edit(component, "loop.start", "-1");
    expect(!component.settings(error), "Negative loop starts are rejected.");
    edit(component, "loop.start", "1");
    edit(component, "loop.end", "1.000000001");
    expect(!component.settings(error), "A loop must span at least one output sample.");
    edit(component, "loop.end", "1.0001");
    expect(component.settings(error).has_value(), "A short loop containing samples is valid.");

    studio::LoopSettingsComponent invalidRate(transportProject(), 0.0);
    expect(!invalidRate.settings(error) && error.isNotEmpty(),
           "An invalid playback sample rate is not silently replaced.");
}

void loopMarkerIdsAndPendingConversions()
{
    auto project = transportProject();
    project.markers = {
        { "end-marker", "Boundary", 8.0 },
        { "first-marker", "Boundary", 1.0001 },
        { "second-marker", "Boundary", 1.0002 }
    };
    studio::LoopSettingsComponent component(project, 48000.0);
    project.markers.clear();
    select(component, "loop.mode", 3);
    auto& first = control<juce::ComboBox>(component, "loop.startMarker");
    auto& last = control<juce::ComboBox>(component, "loop.endMarker");
    expect(first.getItemText(0) != first.getItemText(1)
               && first.getItemText(0).contains("1.000")
               && last.getItemText(2).contains("8.000"),
           "Marker choices are sorted and disambiguate equal rounded labels.");
    select(component, "loop.startMarker", 2);
    select(component, "loop.endMarker", 3);
    juce::String error;
    auto value = component.settings(error);
    expect(value && near(value->startSeconds, 1.0002) && near(value->endSeconds, 8.0),
           "Marker mode resolves the chosen stable IDs from an isolated project snapshot.");
    select(component, "loop.mode", 1);
    value = component.settings(error);
    expect(value && near(value->startSeconds, 1.0002),
           "Leaving marker mode converts the selected positions to fixed timeline seconds.");
    select(component, "loop.mode", 3);
    select(component, "loop.startMarker", 3);
    select(component, "loop.endMarker", 1);
    expect(!component.settings(error), "Reversed marker ranges are rejected.");
    select(component, "loop.startMarker", 0);
    expect(!component.settings(error), "A missing marker selection is rejected instead of rebound.");

    auto plain = transportProject();
    studio::LoopSettingsComponent pending(plain, 48000.0);
    select(pending, "loop.mode", 1);
    edit(pending, "loop.start", "1");
    edit(pending, "loop.end", "3");
    select(pending, "loop.mode", 3);
    expect(!pending.settings(error),
           "A range with no matching markers is not silently replaced by different marker positions.");
    select(pending, "loop.mode", 1);
    value = pending.settings(error);
    expect(value && near(value->startSeconds, 1.0) && near(value->endSeconds, 3.0),
           "Leaving an untouched pending marker selection restores the previous valid range.");

    plain.markers.resize(1);
    studio::LoopSettingsComponent insufficient(plain, 48000.0);
    expect(!control<juce::ComboBox>(insufficient, "loop.mode").isItemEnabled(3),
           "Marker mode requires at least two distinct stable IDs.");

    auto duplicates = transportProject();
    duplicates.markers = {
        { "boundary-one", "Boundary", 1.0 },
        { "boundary-two", "Boundary", 1.0 },
        { "boundary-three", "Boundary", 1.0 },
        { "end-marker", "End", 8.0 }
    };
    studio::LoopSettingsComponent repeated(duplicates, 48000.0);
    select(repeated, "loop.mode", 3);
    select(repeated, "loop.startMarker", 3);
    select(repeated, "loop.endMarker", 4);
    select(repeated, "loop.mode", 1);
    select(repeated, "loop.mode", 3);
    expect(control<juce::ComboBox>(repeated, "loop.startMarker").getSelectedId() == 3,
           "A previously selected stable marker ID survives conversion even with multiple equal-time markers.");
}

void sectionDefaultsAndCustomSettings()
{
    auto project = transportProject();
    project.tempoChanges = { { 0.0, 120.0, false, {} } };
    studio::SectionSettingsComponent component(project, "middle-marker");
    juce::String error;
    auto value = component.settings(error);
    expect(value && !value->tempoBpm && !value->timeSignature && !value->clickSettings
               && control<juce::TextEditor>(component, "section.numerator").getText() == "4"
               && control<juce::ComboBox>(component, "section.denominator").getSelectedId() == 4,
           "Unconfigured sections preserve inherited 4/4 without creating transport overrides.");
    toggle(component, "section.overrideTempo", true);
    edit(component, "section.bpm", "135.5");
    toggle(component, "section.ramp", true);
    toggle(component, "section.overrideMeter", true);
    edit(component, "section.numerator", "6");
    select(component, "section.denominator", 8);
    toggle(component, "section.overrideClick", true);
    toggle(component, "section.clickEnabled", false);
    select(component, "section.subdivision", 7);
    edit(component, "section.level", "65.5");
    edit(component, "section.accentLevel", "90.25");
    edit(component, "section.accentBeats", "1, 4, 32");
    value = component.settings(error);
    expect(value && value->tempoBpm && near(*value->tempoBpm, 135.5)
               && value->rampFromPrevious && value->timeSignature
               && value->timeSignature->numerator == 6 && value->timeSignature->denominator == 8,
           "Section tempo, ramp and meter overrides parse as complete typed settings.");
    expect(value && value->clickSettings && !value->clickSettings->enabled
               && value->clickSettings->subdivision == 7
               && near(value->clickSettings->level, 0.655)
               && near(value->clickSettings->accentLevel, 0.9025)
               && value->clickSettings->accentBeats == std::vector<int>({ 1, 4, 32 }),
           "Muted section click settings preserve subdivisions, percent levels and accent beats up to 32.");
    expect(control<juce::TextEditor>(component, "section.level").isEnabled()
               && control<juce::TextEditor>(component, "section.accentBeats").isEnabled(),
           "Click fields stay editable when the local click is muted.");
    select(component, "section.subdivision", 5);
    edit(component, "section.accentBeats", "");
    value = component.settings(error);
    expect(value && value->clickSettings && value->clickSettings->subdivision == 5
               && value->clickSettings->accentBeats.empty(),
           "Five subdivisions and an explicitly unaccented pattern are supported.");
    expect(!project.findSection("middle-marker")->clickSettings
               && project.meterChanges.empty() && project.tempoChanges.size() == 1,
           "Reading dialog settings validates on a copy rather than mutating the caller's project.");
}

void sectionInheritanceAndClearing()
{
    auto project = transportProject();
    studio::SectionTransportSettings previous;
    previous.tempoBpm = 90.0;
    previous.timeSignature = studio::SectionTimeSignature { 6, 8 };
    studio::SectionClickSettings earlierClick;
    earlierClick.enabled = false;
    earlierClick.subdivision = 5;
    earlierClick.level = 0.4f;
    earlierClick.accentLevel = 0.9f;
    earlierClick.accentBeats = { 1, 4 };
    previous.clickSettings = earlierClick;
    applySection(project, "start-marker", previous);
    studio::SectionTransportSettings current;
    current.tempoBpm = 150.0;
    current.timeSignature = studio::SectionTimeSignature { 5, 4 };
    studio::SectionClickSettings currentClick;
    currentClick.subdivision = 7;
    currentClick.level = 0.7f;
    currentClick.accentBeats = { 1, 3 };
    current.clickSettings = currentClick;
    applySection(project, "middle-marker", current);

    studio::SectionSettingsComponent component(project, "middle-marker");
    toggle(component, "section.overrideTempo", false);
    toggle(component, "section.overrideMeter", false);
    toggle(component, "section.overrideClick", false);
    juce::String error;
    auto value = component.settings(error);
    expect(value && !value->tempoBpm && !value->timeSignature && !value->clickSettings
               && control<juce::TextEditor>(component, "section.bpm").getText() == "90"
               && control<juce::TextEditor>(component, "section.numerator").getText() == "6"
               && control<juce::ComboBox>(component, "section.denominator").getSelectedId() == 8
               && control<juce::ComboBox>(component, "section.subdivision").getSelectedId() == 5
               && !control<juce::ToggleButton>(component, "section.clickEnabled").getToggleState()
               && control<juce::TextEditor>(component, "section.level").getText() == "40",
           "Clearing overrides previews prior values with current owned tempo, meter and click settings excluded.");
    expect(control<juce::Label>(component, "section.summary").getText().contains("90 BPM")
               && control<juce::Label>(component, "section.summary").getText().contains("6/8"),
           "The inheritance summary does not retain stale current-section overrides.");
    toggle(component, "section.overrideTempo", true);
    toggle(component, "section.overrideMeter", true);
    toggle(component, "section.overrideClick", true);
    value = component.settings(error);
    expect(value && value->tempoBpm && near(*value->tempoBpm, 150.0)
               && value->timeSignature && value->timeSignature->numerator == 5
               && value->clickSettings && value->clickSettings->subdivision == 7,
           "Re-enabling an override restores its editable draft without changing inheritance semantics.");
    edit(component, "section.bpm", "unfinished");
    toggle(component, "section.overrideTempo", false);
    expect(component.settings(error).has_value(),
           "Clearing a malformed override remains possible and returns inheritance.");
    toggle(component, "section.overrideTempo", true);
    expect(!component.settings(error)
               && control<juce::TextEditor>(component, "section.bpm").getText() == "unfinished",
           "Malformed custom drafts are preserved, never silently clamped.");

    studio::SectionSettingsComponent genericSection(project, "end-marker");
    value = genericSection.settings(error);
    expect(value && !value->tempoBpm && !value->timeSignature && !value->clickSettings
               && control<juce::TextEditor>(genericSection, "section.bpm").getText() == "150"
               && control<juce::ComboBox>(genericSection, "section.subdivision").getSelectedId() == 7,
           "A generic later section inherits earlier explicit settings without owning or resetting them.");
}

void sectionGlobalClickAndValidation()
{
    auto project = transportProject();
    project.metronomeEnabled = false;
    studio::SectionSettingsComponent component(project, "middle-marker");
    toggle(component, "section.overrideClick", true);
    toggle(component, "section.clickEnabled", true);
    juce::String error;
    auto value = component.settings(error);
    expect(value && value->clickSettings && value->clickSettings->enabled
               && !project.metronomeEnabled
               && control<juce::Label>(component, "section.clickHint").getText().contains("Global CLICK is off"),
           "Local click configuration is editable while global CLICK stays an independent master gate.");
    toggle(component, "section.overrideTempo", true);
    for (const auto* invalid : { "", "NaN", "Inf", "120bpm", "19.9", "400.1" })
    {
        edit(component, "section.bpm", invalid);
        expect(!component.settings(error) && error.isNotEmpty(),
               "Section BPM must be finite, complete and between 20 and 400.");
    }
    edit(component, "section.bpm", "120");
    toggle(component, "section.overrideMeter", true);
    for (const auto* invalid : { "", "3.5", "3.0", "1e1", "0", "33", "4beats" })
    {
        edit(component, "section.numerator", invalid);
        expect(!component.settings(error), "Meter numerator is a strict whole number in 1..32.");
    }
    edit(component, "section.numerator", "6");
    select(component, "section.denominator", 3);
    expect(!component.settings(error), "A non-power-of-two denominator is rejected.");
    select(component, "section.denominator", 8);
    for (const auto* invalid : { "1,1", "0", "33", "1,,4", "1,", "1.5", "1e0", "-1", "1 4" })
    {
        edit(component, "section.accentBeats", invalid);
        expect(!component.settings(error), "Accent CSV requires unique complete integer beats in 1..32.");
    }
    edit(component, "section.accentBeats", "1,4");
    toggle(component, "section.clickEnabled", false);
    for (const auto* invalid : { "-1", "100.1", "NaN", "50percent", "" })
    {
        edit(component, "section.level", invalid);
        expect(!component.settings(error)
                   && control<juce::TextEditor>(component, "section.level").isEnabled(),
               "Muted click levels still validate and remain editable for repair.");
    }
    edit(component, "section.level", "65");
    edit(component, "section.accentLevel", "101");
    expect(!component.settings(error), "Accent level is constrained to 0..100 percent.");
    edit(component, "section.accentLevel", "0");
    select(component, "section.subdivision", 0);
    expect(!component.settings(error), "A missing click subdivision is rejected.");
    select(component, "section.subdivision", 8);
    expect(component.settings(error).has_value(), "The maximum click subdivision and zero accent level are valid.");

    studio::SectionSettingsComponent missing(project, "deleted-marker");
    expect(!missing.settings(error) && error.containsIgnoreCase("missing"),
           "A missing selected section fails explicitly instead of editing another marker.");
}

void layoutAndCallbackSafety()
{
    auto project = transportProject();
    studio::LoopSettingsComponent loop(project, 48000.0);
    studio::SectionSettingsComponent section(project, "middle-marker");
    bool applied = false;
    loop.onApply = [&](auto) { applied = true; };
    section.onApply = [&](auto) { applied = true; };
    select(loop, "loop.mode", 1);
    toggle(loop, "loop.enabled", true);
    toggle(section, "section.overrideClick", true);
    select(section, "section.subdivision", 7);
    expect(!applied, "Changing settings controls never invokes an apply callback.");

    for (auto* component : { static_cast<juce::Component*>(&loop), static_cast<juce::Component*>(&section) })
    {
        component->setSize(350, 390);
        const auto prefix = component == &loop ? juce::String("loop") : juce::String("section");
        auto* viewport = dynamic_cast<juce::Viewport*>(findControl(*component, prefix + ".viewport"));
        expect(viewport != nullptr && viewport->getViewedComponent() != nullptr,
               "Native settings keep their forms in an accessible scrolling viewport.");
        if (viewport == nullptr || viewport->getViewedComponent() == nullptr)
            continue;
        const auto* body = viewport->getViewedComponent();
        expect(body->getHeight() > viewport->getHeight(),
               "A compact desktop scrolls rather than cropping transport controls.");
        for (auto* child : body->getChildren())
            if (child->isVisible())
                expect(!child->getBounds().isEmpty() && body->getLocalBounds().contains(child->getBounds()),
                       "Every visible settings field and label fits inside its scroll content.");
        for (const auto suffix : { ".apply", ".cancel" })
        {
            auto* button = findControl(*component, prefix + suffix);
            expect(button != nullptr && component->getLocalBounds().contains(button->getBounds())
                       && button->getY() >= viewport->getBottom() && button->getWantsKeyboardFocus(),
                   "Apply and Cancel stay visible below the scrolling form and keyboard reachable.");
        }
    }
    expect(control<juce::TextEditor>(loop, "loop.start").getTitle().isNotEmpty()
               && control<juce::TextEditor>(section, "section.accentBeats").getTitle().isNotEmpty()
               && control<juce::ComboBox>(section, "section.subdivision").getTooltip().isNotEmpty(),
           "Transport controls expose accessible titles and explanatory tooltips.");
    edit(loop, "loop.start", "invalid");
    loop.setVisible(true);
    loop.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(!applied && loop.isVisible()
               && control<juce::Label>(loop, "loop.error").getText().isNotEmpty(),
           "Return on invalid loop settings reports an inline error without closing or applying.");
    loop.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    expect(!applied && !loop.isVisible(), "Escape cancels loop editing without applying.");
    toggle(section, "section.overrideTempo", true);
    edit(section, "section.bpm", "invalid");
    section.setVisible(true);
    section.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(!applied && section.isVisible()
               && control<juce::Label>(section, "section.error").getText().isNotEmpty(),
           "Return on invalid section settings keeps the dialog open with an inline error.");
    section.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    expect(!applied && !section.isVisible(), "Escape cancels section editing without applying.");
}
}

void transportSettingsTests()
{
    try
    {
        loopSecondsAndPresets();
        loopMusicalMaps();
        invalidLoopInputIsNotDiscarded();
        loopMarkerIdsAndPendingConversions();
        sectionDefaultsAndCustomSettings();
        sectionInheritanceAndClearing();
        sectionGlobalClickAndValidation();
        layoutAndCallbackSafety();
    }
    catch (const std::exception& error)
    {
        expect(false, error.what());
    }
}

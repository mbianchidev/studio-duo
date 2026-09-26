#pragma once

#include "StudioIconButton.h"
#include "DrumPerformanceComponent.h"
#include "midi/MidiEditing.h"
#include "model/ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace studio
{
enum class MidiEditorCommandId
{
    createNote = 0x5300,
    deleteNotes,
    moveNotesLeft,
    moveNotesRight,
    moveNotesUp,
    moveNotesDown,
    resizeNotesShorter,
    resizeNotesLonger,
    selectAllNotes,
    captureRetrospective,
    applyHumanize,
    expandPattern
};

class MidiEditorComponent final : public juce::Component
{
public:
    MidiEditorComponent();

    void setProject(const Project* projectToEdit);
    void setSelection(juce::String trackId, juce::String clipId);
    void showDrumPads(bool show = true);
    [[nodiscard]] bool isShowingDrumPads() const noexcept;
    [[nodiscard]] DrumPerformanceComponent& drumPerformance() noexcept;

    std::function<void(const juce::String&,
                       const MidiClip&,
                       const MidiClip&,
                       const juce::String&)>
        onClipEdited;
    std::function<void()> onCaptureRetrospective;
    std::function<void()> onImportDrumMap;
    std::function<void(int)> onEditDrumMapEntry;
    std::function<void()> onHumanize;
    std::function<void(const juce::String&)> onApplyRoutingTemplate;
    std::function<void()> onClose;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;

private:
    enum class DragMode
    {
        none,
        move,
        resize,
        lane
    };

    [[nodiscard]] const MidiClip* currentClip() const;
    [[nodiscard]] const MidiClip& displayedClip() const;
    [[nodiscard]] const DrumMap* currentDrumMap() const;
    [[nodiscard]] juce::Rectangle<int> noteArea() const;
    [[nodiscard]] juce::Rectangle<int> laneArea() const;
    [[nodiscard]] int labelWidth() const noexcept;
    [[nodiscard]] double gridBeats() const noexcept;
    [[nodiscard]] double beatForX(float x) const noexcept;
    [[nodiscard]] float xForBeat(double beat) const noexcept;
    [[nodiscard]] int pitchForY(float y) const;
    [[nodiscard]] float yForPitch(int pitch) const;
    [[nodiscard]] juce::Rectangle<float> noteBounds(
        const MidiNote& note) const;
    [[nodiscard]] const MidiNote* noteAt(
        juce::Point<float> position) const;
    [[nodiscard]] std::vector<const DrumMapEntry*> drumRows() const;
    [[nodiscard]] int maximumRowOffset() const;
    [[nodiscard]] MidiEditorLane selectedLane() const noexcept;
    [[nodiscard]] MidiExpressionType selectedExpressionType() const noexcept;

    void refreshControls();
    void changeEditorMode(MidiEditorMode mode);
    void commitEdit(const MidiClip& before,
                    MidiClip after,
                    const juce::String& commandName);
    void addNoteAt(double beat, int pitch);
    void runEntryTool(MidiEntryTool tool);
    void expandSelectedPattern();
    void applyLaneAt(juce::Point<float> position);
    void selectOnly(const juce::String& noteId);
    void removeSelectedNotes();

    const Project* project = nullptr;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    std::vector<juce::String> selectedNoteIds;
    std::optional<MidiClip> dragBefore;
    std::optional<MidiClip> dragPreview;
    DragMode dragMode = DragMode::none;
    juce::Point<float> dragStart;
    double dragStartBeat = 0.0;
    int dragStartPitch = 60;
    double cursorBeat = 0.0;
    int cursorPitch = 60;
    int rowOffset = 0;
    bool showingPads = false;

    juce::TextButton modeButton { "PIANO" };
    juce::TextButton padsButton { "DRUM PADS" };
    DrumPerformanceComponent drumPerformancePanel;
    juce::ComboBox laneSelector;
    juce::ComboBox expressionSelector;
    juce::ComboBox gridSelector;
    juce::TextButton captureButton { "CAPTURE" };
    juce::TextButton humanizeButton { "HUMANIZE" };
    StudioIconButton importMapButton {
        StudioIcon::importFile,
        "Import drum map",
        "Import a Studio Duo drum-map JSON file"
    };
    StudioIconButton editMapButton {
        StudioIcon::edit,
        "Edit drum map row",
        "Edit the selected drum-map row"
    };
    StudioIconButton closeButton {
        StudioIcon::close,
        "Close MIDI editor",
        "Close the MIDI lower editor"
    };
    juce::TextButton flamButton { "FLAM" };
    juce::TextButton rollButton { "ROLL" };
    juce::TextButton gravityButton { "GRAVITY" };
    juce::TextButton blastButton { "BLAST" };
    juce::TextButton doubleKickButton { "DOUBLE KICK" };
    juce::ComboBox patternSelector;
    StudioIconButton expandPatternButton {
        StudioIcon::expand,
        "Expand pattern",
        "Expand the selected alias into ordinary notes"
    };
    juce::ComboBox routingTemplateSelector;
    juce::TextButton applyRoutingButton { "APPLY ROUTING" };

    static constexpr int toolbarHeight = 70;
    static constexpr int laneHeight = 92;
    static constexpr int pianoRows = 24;
    static constexpr float minimumDrumRowHeight = 18.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiEditorComponent)
};
}

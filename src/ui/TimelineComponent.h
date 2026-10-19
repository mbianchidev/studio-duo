#pragma once

#include "model/ProjectModel.h"
#include "ClipProcessingEditorComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class TimelineComponent final : public juce::Component
{
public:
    struct RecordingPreview
    {
        juce::String trackId;
        double startSeconds = 0.0;
        double durationSeconds = 0.0;
        std::vector<float> waveformPeaks;
    };

    TimelineComponent();

    void setProject(const Project* projectToDisplay);
    void setSelection(juce::String trackId, juce::String clipId);
    void setPlayheadSeconds(double seconds);
    void setViewportPosition(int horizontalPosition);
    void setRecordingPreviews(std::vector<RecordingPreview> previews);
    void clearRecordingPreviews();
    void setPixelsPerSecond(double pixels);
    void setSnapEnabled(bool enabled);
    void setEditGridBeats(double beats);
    [[nodiscard]] double getPixelsPerSecond() const noexcept;
    [[nodiscard]] float xForSeconds(double seconds) const noexcept;
    [[nodiscard]] int preferredWidth(int minimumWidth) const;
    [[nodiscard]] int preferredHeight(int minimumHeight) const;

    static constexpr double defaultPixelsPerSecond = 96.0;

#if defined(STUDIO_DUO_TESTING)
    struct PaintStatistics
    {
        int gridLinesConsidered = 0;
        int tracksPainted = 0;
        int clipsPainted = 0;
    };
    [[nodiscard]] PaintStatistics paintStatisticsForTesting() const noexcept
    {
        return testingPaintStatistics;
    }
    [[nodiscard]] juce::PopupMenu keyboardContextMenuForTesting()
    {
        ensureSelectionVisible();
        return createContextMenu(keyboardContextPosition(), false);
    }
#endif

    std::function<void(const juce::String&)> onTrackSelected;
    std::function<void(const juce::String&)> onTrackMute;
    std::function<void(const juce::String&)> onTrackSolo;
    std::function<void(const juce::String&)> onTrackArm;
    std::function<void(const juce::String&, float)>
        onTrackVolumeGestureStarted;
    std::function<void(const juce::String&, float)>
        onTrackVolumeChanged;
    std::function<void(const juce::String&, juce::Rectangle<int>)>
        onInputMenuRequested;
    std::function<void(const juce::String&, juce::Rectangle<int>)> onEditTrack;
    std::function<void(const juce::String&)> onToggleTrackVersions;
    std::function<void(const juce::String&)> onDuplicateTrack;
    std::function<void(const juce::String&)> onDeleteTrack;
    std::function<void()> onAddTrack;
    std::function<void(const juce::String&, const juce::String&)> onClipSelected;
    std::function<void(const juce::String&, double)> onCreateMidiClip;
    std::function<void(const juce::String&, const juce::String&, double)> onClipMoved;
    std::function<void(const juce::String&, double, double, double)> onClipTrimmed;
    std::function<void(double)> onSeek;
    std::function<void(double, double)> onZoomRequested;
    std::function<void(double)> onAddMarkerRequested;
    std::function<void(const juce::String&)> onEditMarkerRequested;
    std::function<void(const juce::String&)> onRemoveMarkerRequested;
    std::function<void(const juce::String&, double)> onMoveMarkerRequested;
    std::function<void(double)> onAddSectionRequested;
    std::function<void(const juce::String&)> onEditSectionRequested;
    std::function<void(const juce::String&)> onRemoveSectionRequested;
    std::function<void(const juce::String&)> onConfigureSectionRequested;
    std::function<void(const juce::String&, double, double)>
        onSectionRangeChanged;
    std::function<void()> onSplitSelected;
    std::function<void()> onTrimStartSelected;
    std::function<void()> onTrimEndSelected;
    std::function<void()> onDuplicateSelected;
    std::function<void()> onDeleteSelected;
    std::function<void(const juce::String&)> onUseTake;
    std::function<void(const juce::String&)> onUseClipForComp;
    std::function<void(const juce::String&)> onClearComp;
    std::function<void(const juce::String&)> onAnalyseTransients;
    std::function<void(const juce::String&, StretchMode)> onSetStretchMode;
    std::function<void(const juce::String&, double)> onSetPlaybackRate;
    std::function<void(const juce::String&, double)> onWarpTransientToTimeline;
    std::function<void(const juce::String&, double)> onSetFadeIn;
    std::function<void(const juce::String&, double)> onSetFadeOut;
    std::function<void(const juce::String&, float)> onClipGainChanged;
    std::function<void(const juce::String&, bool, double, float)>
        onClipFadeChanged;
    std::function<void(const juce::String&)> onToggleClipMute;
    std::function<void(const juce::String&)> onCreateCrossfade;
    std::function<void(const juce::String&)> onToggleClipPolarity;
    std::function<void(const juce::String&)> onToggleClipReverse;
    std::function<void(const juce::String&)> onConsolidateClip;

    void paint(juce::Graphics& graphics) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;

private:
#if defined(STUDIO_DUO_TESTING)
    PaintStatistics testingPaintStatistics;
#endif
    struct Hit
    {
        juce::String trackId;
        juce::String clipId;
        juce::Rectangle<float> bounds;
        bool midi = false;
        double startSeconds = 0.0;
    };

    enum class DragMode
    {
        none,
        move,
        trimStart,
        trimEnd,
        gain,
        fadeIn,
        fadeOut,
        fadeInCurve,
        fadeOutCurve
    };

    enum class SectionDragMode
    {
        none,
        move,
        resizeEnd
    };

    [[nodiscard]] juce::String markerIdAt(juce::Point<float> position) const;
    [[nodiscard]] juce::String sectionIdAt(juce::Point<float> position) const;
    [[nodiscard]] double sectionEndSeconds(
        std::size_t index,
        double fallback) const noexcept;
    [[nodiscard]] std::vector<Hit> clipHits(juce::Rectangle<float> interest = {}) const;
    [[nodiscard]] std::vector<const Track*> visibleTracks() const;
    [[nodiscard]] int trackIndexAt(float y) const noexcept;
    [[nodiscard]] float trackY(const juce::String& trackId) const noexcept;
    [[nodiscard]] double xToSeconds(float x) const noexcept;
    [[nodiscard]] float secondsToX(double seconds) const noexcept;
    [[nodiscard]] double snappedSeconds(double seconds) const noexcept;
    static void drawClipWaveform(juce::Graphics& graphics,
                                 const AudioClip& clip,
                                 juce::Rectangle<float> bounds,
                                 float alpha);
    [[nodiscard]] static float fadeCurveValue(float progress,
                                              float curve) noexcept;
    [[nodiscard]] juce::Point<float> fadeLengthHandle(
        juce::Rectangle<float> bounds,
        bool fadeIn,
        double fadeSeconds) const noexcept;
    [[nodiscard]] juce::Point<float> fadeCurveHandle(
        juce::Rectangle<float> bounds,
        bool fadeIn,
        double fadeSeconds,
        float curve) const noexcept;
    [[nodiscard]] DragMode dragModeAt(const Hit& hit,
                                     const AudioClip& clip,
                                     juce::Point<float> position) const noexcept;
    void updateHoverState(juce::Point<float> position);
    void showContextMenu(juce::Point<float> position, bool selectFromPosition = true);
    juce::PopupMenu createContextMenu(juce::Point<float> position, bool selectFromPosition);
    juce::Point<float> keyboardContextPosition() const;
    void showClipProcessingEditor(const juce::String& clipId, ClipProcessingEditorComponent::Mode mode,
                                  juce::Point<float> position);
    void selectForKeyboard(juce::String trackId, juce::String clipId = {});
    void ensureSelectionVisible();
    void updateAccessibleSelection();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Project* project = nullptr;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    juce::String accessibleSelectionValue { "Timeline; no selection" };
    juce::String draggingTrackVolumeId;
    juce::String draggedMarkerId;
    juce::String draggedSectionId;
    juce::String draggedClipId;
    juce::String hoveredClipId;
    juce::String dragOriginalTrackId;
    juce::String dragPreviewTrackId;
    double playheadSeconds = 0.0;
    double markerDragOriginalSeconds = 0.0;
    double markerDragPreviewSeconds = 0.0;
    double sectionDragOriginalStart = 0.0;
    double sectionDragOriginalEnd = 0.0;
    double sectionDragPreviewStart = 0.0;
    double sectionDragPreviewEnd = 0.0;
    float dragPreviewTrackVolume = 0.0f;
    std::vector<RecordingPreview> recordingPreviews;
    double pixelsPerSecond = 96.0;
    double editGridBeats = 0.25;
    bool snapEnabled = true;
    int viewportPositionX = 0;
    double dragOriginalStart = 0.0;
    double dragOriginalSourceOffset = 0.0;
    double dragOriginalDuration = 0.0;
    double dragPreviewStart = 0.0;
    double dragPreviewSourceOffset = 0.0;
    double dragPreviewDuration = 0.0;
    float dragStartX = 0.0f;
    float dragStartY = 0.0f;
    float dragOriginalGainDecibels = 0.0f;
    float dragPreviewGainDecibels = 0.0f;
    double dragOriginalFadeSeconds = 0.0;
    double dragPreviewFadeSeconds = 0.0;
    double dragMaximumFadeSeconds = 0.0;
    float dragOriginalFadeCurve = 0.0f;
    float dragPreviewFadeCurve = 0.0f;
    DragMode dragMode = DragMode::none;
    DragMode hoveredDragMode = DragMode::none;
    SectionDragMode sectionDragMode =
        SectionDragMode::none;

    static constexpr double minimumPixelsPerSecond = 24.0;
    static constexpr double maximumPixelsPerSecond = 9600.0;
    static constexpr int markerLaneHeight = 20;
    static constexpr int sectionLaneHeight = 24;
    static constexpr int timelineRulerTop =
        markerLaneHeight + sectionLaneHeight;
    static constexpr int rulerHeight = 80;
    static constexpr int trackHeaderWidth = 176;
    static constexpr int trackHeight = 88;
    static constexpr int addTrackHeight = 44;
};
}

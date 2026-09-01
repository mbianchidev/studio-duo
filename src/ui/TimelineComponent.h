#pragma once

#include "model/ProjectModel.h"

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
    [[nodiscard]] double getPixelsPerSecond() const noexcept;
    [[nodiscard]] float xForSeconds(double seconds) const noexcept;
    [[nodiscard]] int preferredWidth(int minimumWidth) const;
    [[nodiscard]] int preferredHeight(int minimumHeight) const;

    std::function<void(const juce::String&)> onTrackSelected;
    std::function<void(const juce::String&)> onTrackMute;
    std::function<void(const juce::String&)> onTrackSolo;
    std::function<void(const juce::String&)> onTrackArm;
    std::function<void(const juce::String&)> onToggleTrackVersions;
    std::function<void(const juce::String&)> onDeleteTrack;
    std::function<void()> onAddTrack;
    std::function<void(const juce::String&, const juce::String&)> onClipSelected;
    std::function<void(const juce::String&, const juce::String&, double)> onClipMoved;
    std::function<void(const juce::String&, double, double, double)> onClipTrimmed;
    std::function<void(double)> onSeek;
    std::function<void(double)> onZoomRequested;
    std::function<void()> onSplitSelected;
    std::function<void()> onTrimStartSelected;
    std::function<void()> onTrimEndSelected;
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
    std::function<void(const juce::String&)> onCreateCrossfade;
    std::function<void(const juce::String&)> onToggleClipPolarity;
    std::function<void(const juce::String&)> onToggleClipReverse;
    std::function<void(const juce::String&)> onConsolidateClip;

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;

private:
    struct Hit
    {
        juce::String trackId;
        juce::String clipId;
        juce::Rectangle<float> bounds;
    };

    enum class DragMode
    {
        none,
        move,
        trimStart,
        trimEnd
    };

    [[nodiscard]] std::vector<Hit> clipHits() const;
    [[nodiscard]] std::vector<const Track*> visibleTracks() const;
    [[nodiscard]] int trackIndexAt(float y) const noexcept;
    [[nodiscard]] float trackY(const juce::String& trackId) const noexcept;
    [[nodiscard]] double xToSeconds(float x) const noexcept;
    [[nodiscard]] float secondsToX(double seconds) const noexcept;
    static void drawClipWaveform(juce::Graphics& graphics,
                                 const AudioClip& clip,
                                 juce::Rectangle<float> bounds,
                                 float alpha);
    void showContextMenu(const juce::MouseEvent& event);

    const Project* project = nullptr;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    juce::String draggedClipId;
    juce::String dragOriginalTrackId;
    juce::String dragPreviewTrackId;
    double playheadSeconds = 0.0;
    std::vector<RecordingPreview> recordingPreviews;
    double pixelsPerSecond = 96.0;
    int viewportPositionX = 0;
    double dragOriginalStart = 0.0;
    double dragOriginalSourceOffset = 0.0;
    double dragOriginalDuration = 0.0;
    double dragPreviewStart = 0.0;
    double dragPreviewSourceOffset = 0.0;
    double dragPreviewDuration = 0.0;
    float dragStartX = 0.0f;
    DragMode dragMode = DragMode::none;

    static constexpr int rulerHeight = 36;
    static constexpr int trackHeaderWidth = 176;
    static constexpr int trackHeight = 88;
    static constexpr int addTrackHeight = 44;
};
}

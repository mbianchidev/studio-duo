#pragma once

#include "model/ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class TimelineComponent final : public juce::Component
{
public:
    TimelineComponent();

    void setProject(const Project* projectToDisplay);
    void setSelection(juce::String trackId, juce::String clipId);
    void setPlayheadSeconds(double seconds);
    void setRecordingPreview(juce::String trackId,
                             double startSeconds,
                             double durationSeconds,
                             std::vector<float> waveformPeaks);
    void clearRecordingPreview();
    void setPixelsPerSecond(double pixels);
    [[nodiscard]] int preferredWidth(int minimumWidth) const;
    [[nodiscard]] int preferredHeight(int minimumHeight) const;

    std::function<void(const juce::String&)> onTrackSelected;
    std::function<void(const juce::String&)> onTrackMute;
    std::function<void(const juce::String&)> onTrackSolo;
    std::function<void(const juce::String&)> onTrackArm;
    std::function<void()> onAddTrack;
    std::function<void(const juce::String&, const juce::String&)> onClipSelected;
    std::function<void(const juce::String&, const juce::String&, double)> onClipMoved;
    std::function<void(const juce::String&, double, double, double)> onClipTrimmed;
    std::function<void(double)> onSeek;

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

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
    [[nodiscard]] int trackIndexAt(float y) const noexcept;
    [[nodiscard]] float trackY(const juce::String& trackId) const noexcept;
    [[nodiscard]] double xToSeconds(float x) const noexcept;
    [[nodiscard]] float secondsToX(double seconds) const noexcept;

    const Project* project = nullptr;
    juce::String selectedTrackId;
    juce::String selectedClipId;
    juce::String draggedClipId;
    juce::String dragOriginalTrackId;
    juce::String dragPreviewTrackId;
    juce::String recordingTrackId;
    double playheadSeconds = 0.0;
    double recordingStartSeconds = 0.0;
    double recordingDurationSeconds = 0.0;
    std::vector<float> recordingPeaks;
    double pixelsPerSecond = 96.0;
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

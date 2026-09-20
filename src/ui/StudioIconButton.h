#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace studio
{
enum class StudioIcon
{
    newFile,
    openFolder,
    save,
    exportFile,
    settings,
    undo,
    redo,
    play,
    pause,
    stop,
    record,
    loop,
    loopRange,
    metronome,
    mute,
    solo,
    volume,
    midi,
    inspect,
    mixer,
    tracks,
    add,
    bus,
    importFile,
    duplicate,
    deleteItem,
    marker,
    automation,
    chevronLeft,
    chevronRight,
    chevronUp,
    chevronDown,
    split,
    trimStart,
    trimEnd,
    zoomOut,
    zoomIn,
    edit,
    close,
    expand,
    scan,
    folder,
    route
};

inline constexpr std::array allStudioIcons {
    StudioIcon::newFile,
    StudioIcon::openFolder,
    StudioIcon::save,
    StudioIcon::exportFile,
    StudioIcon::settings,
    StudioIcon::undo,
    StudioIcon::redo,
    StudioIcon::play,
    StudioIcon::pause,
    StudioIcon::stop,
    StudioIcon::record,
    StudioIcon::loop,
    StudioIcon::loopRange,
    StudioIcon::metronome,
    StudioIcon::mute,
    StudioIcon::solo,
    StudioIcon::volume,
    StudioIcon::midi,
    StudioIcon::inspect,
    StudioIcon::mixer,
    StudioIcon::tracks,
    StudioIcon::add,
    StudioIcon::bus,
    StudioIcon::importFile,
    StudioIcon::duplicate,
    StudioIcon::deleteItem,
    StudioIcon::marker,
    StudioIcon::automation,
    StudioIcon::chevronLeft,
    StudioIcon::chevronRight,
    StudioIcon::chevronUp,
    StudioIcon::chevronDown,
    StudioIcon::split,
    StudioIcon::trimStart,
    StudioIcon::trimEnd,
    StudioIcon::zoomOut,
    StudioIcon::zoomIn,
    StudioIcon::edit,
    StudioIcon::close,
    StudioIcon::expand,
    StudioIcon::scan,
    StudioIcon::folder,
    StudioIcon::route
};

[[nodiscard]] juce::Path createStudioIconPath(StudioIcon icon);
void drawStudioIcon(juce::Graphics& graphics,
                    StudioIcon icon,
                    juce::Rectangle<float> bounds,
                    juce::Colour colour,
                    float strokeWidth = 1.5f);

class StudioIconButton final : public juce::TextButton
{
public:
    static constexpr int compactWidth = 32;
    static constexpr int transportWidth = 38;

    StudioIconButton(StudioIcon icon,
                     juce::String accessibleLabel,
                     juce::String tooltip);

    void setIcon(StudioIcon icon);
    [[nodiscard]] StudioIcon getIcon() const noexcept;
    void setAccessibleLabel(juce::String label);
    void setVisibleLabel(juce::String label);
    void setShowLabel(bool shouldShow);
    [[nodiscard]] bool isShowingLabel() const noexcept;

    void paintButton(juce::Graphics& graphics,
                     bool highlighted,
                     bool down) override;

private:
    StudioIcon icon;
    juce::String visibleLabel;
    bool showLabel = false;
};
}

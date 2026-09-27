#pragma once

#include "midi/MidiModel.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <optional>

namespace studio
{
class DrumPerformanceComponent final : public juce::Component,
                                       private juce::Timer
{
public:
    DrumPerformanceComponent();
    ~DrumPerformanceComponent() override;

    void setContext(const juce::String& trackId,
                    const MidiClip* clip,
                    const DrumMap* map);
    void setKeyboardEnabled(bool enabled);
    [[nodiscard]] bool isKeyboardEnabled() const noexcept;
    juce::Result assignSound(std::size_t pad, int noteNumber);
    juce::Result assignKey(std::size_t pad, int keyCode);
    void releaseAllNotes();
    void setTransportState(const juce::String& position,
                           bool playing,
                           bool recording,
                           bool clickEnabled,
                           bool countingIn);

    std::function<juce::Result(const juce::String&, const juce::MidiMessage&)>
        onMidiMessage;
    std::function<bool(const std::vector<DrumPadBinding>&)> onBindingsEdited;
    std::function<void(const juce::String&, bool)> onStatus;
    std::function<void(bool)> onAuditionChanged;
    std::function<void()> onPlay;
    std::function<void()> onRecord;
    std::function<void()> onClick;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(FocusChangeType cause) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    class PadButton final : public juce::TextButton
    {
    public:
        void paintButton(juce::Graphics& graphics, bool over, bool down) override;
        juce::String soundName;
        int keyCode = 0;
        int noteNumber = 0;
        bool sounding = false;
    };

    void timerCallback() override;
    void refreshPads();
    void refreshSelectedPad();
    void refreshHint();
    void refreshAuditionState();
    void selectPad(std::size_t pad);
    void pressPad(std::size_t pad, bool keyboard, int velocity);
    void releasePad(std::size_t pad, bool keyboard);
    bool send(const juce::MidiMessage& message);
    juce::Result changeBinding(std::size_t pad, DrumPadBinding binding);
    void showFailure(const juce::Result& result);
    [[nodiscard]] bool performanceHasFocus() const;

    juce::String selectedTrackId;
    juce::String selectedClipId;
    std::optional<DrumMap> drumMap;
    std::vector<DrumPadBinding> bindings;
    std::array<PadButton, drumPadCount> pads;
    std::array<bool, drumPadCount> heldKeys {};
    std::array<bool, drumPadCount> heldMouse {};
    std::array<int, drumPadCount> soundingChannels {};
    std::size_t selectedPad = 8;
    bool keyboardEnabled = false;
    bool learningKey = false;
    bool auditionActive = false;

    juce::TextButton keyboardButton { "KEYBOARD OFF" };
    juce::TextButton playButton { "PLAY" };
    juce::TextButton recordButton { "RECORD" };
    juce::TextButton clickButton { "CLICK" };
    juce::Label transportLabel;
    juce::Label selectedPadLabel;
    juce::Label soundLabel;
    juce::ComboBox soundSelector;
    juce::Label velocityLabel;
    juce::Slider velocitySlider;
    juce::Label channelLabel;
    juce::ComboBox channelSelector;
    juce::TextButton bindKeyButton { "BIND KEY" };
    juce::Label detailsLabel;
    juce::Label hintLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumPerformanceComponent)
};
}

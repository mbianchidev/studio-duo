#pragma once

#include "midi/MidiModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace studio
{
class DrumGeometryComponent final : public juce::Component,
                                    private juce::ListBoxModel
{
public:
    struct Hit
    {
        juce::String noteId;
        int pitch = 36;
        double beat = 0.0;
        double phase = 0.0;
        int velocity = 100;
        double probability = 1.0;
    };

    struct Ring
    {
        int pitch = 36;
        juce::String name;
        juce::String articulation;
        juce::String outputGroup;
        std::vector<Hit> hits;
    };

    DrumGeometryComponent();

    void setContext(const MidiClip* clip, const DrumMap* map, double beatsPerBar);
    void setTransportPosition(double positionBeats, bool playing);
    [[nodiscard]] const std::vector<Ring>& rings() const noexcept { return rhythmRings; }
    [[nodiscard]] std::optional<double> playheadPhase() const noexcept;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& graphics,
                          int width, int height, bool selected) override;
    juce::String getNameForRow(int row) override;
    void selectedRowsChanged(int row) override;
    [[nodiscard]] juce::Rectangle<float> wheelBounds() const;
    [[nodiscard]] juce::Point<float> pointForPhase(double phase, float radius) const;
    [[nodiscard]] juce::Colour ringColour(std::size_t ring) const;
    void refreshHint();

    std::vector<Ring> rhythmRings;
    double durationBeats = 0.0;
    double clipStartBeats = 0.0;
    double transportBeats = 0.0;
    bool transportPlaying = false;
    int selectedRing = -1;
    juce::ListBox legend { "Drum voices" };
    juce::Label summaryLabel;
    juce::Label hintLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumGeometryComponent)
};
}

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace studio
{
class UserGuideComponent final
    : public juce::Component
{
public:
    UserGuideComponent();
    ~UserGuideComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void findNext();

    juce::Label title;
    juce::TextEditor search;
    juce::TextButton findButton {
        "FIND NEXT"
    };
    juce::Label searchStatus;
    juce::TextEditor guide;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        UserGuideComponent)
};
}

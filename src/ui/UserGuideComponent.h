#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <vector>

namespace studio
{
class UserGuideComponent final
    : public juce::Component,
      private juce::ListBoxModel
{
public:
    struct Page
    {
        juce::String title;
        juce::String body;
    };

    UserGuideComponent();
    ~UserGuideComponent() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem(
        int row,
        juce::Graphics& graphics,
        int width,
        int height,
        bool selected) override;
    void selectedRowsChanged(
        int lastRowSelected) override;
    void showPage(int page);
    void findNext();

    std::vector<Page> pages;
    int currentPage = 0;

    juce::Label indexTitle;
    juce::Label indexSubtitle;
    juce::ListBox index;
    juce::Label breadcrumb;
    juce::Label pageTitle;
    juce::TextEditor search;
    juce::TextButton findButton {
        "FIND NEXT"
    };
    juce::Label searchStatus;
    juce::TextEditor guide;
    juce::TextButton previousButton {
        "PREVIOUS"
    };
    juce::Label pagePosition;
    juce::TextButton nextButton {
        "NEXT"
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        UserGuideComponent)
};
}

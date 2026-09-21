#include "UserGuideComponent.h"

#include "StudioTheme.h"

#include <StudioDuoDocumentationData.h>

namespace studio
{
namespace
{
juce::String readableGuideText(
    const juce::String& markdown)
{
    auto lines =
        juce::StringArray::fromLines(markdown);
    juce::String result;
    auto inCodeBlock = false;
    for (auto line : lines)
    {
        line = line.trimEnd();
        if (line.trim().startsWith("```"))
        {
            inCodeBlock = !inCodeBlock;
            continue;
        }
        if (!inCodeBlock)
        {
            auto headingLevel = 0;
            while (headingLevel < line.length()
                   && line[headingLevel] == '#')
                ++headingLevel;
            if (headingLevel > 0
                && headingLevel < line.length()
                && juce::CharacterFunctions::isWhitespace(
                    line[headingLevel]))
            {
                if (result.isNotEmpty())
                    result << "\n";
                line = line.substring(
                    headingLevel)
                           .trimStart();
            }
            line = line.replace("**", {})
                       .replace("__", {})
                       .replace("`", {});
            for (;;)
            {
                const auto open =
                    line.indexOfChar('[');
                const auto middle =
                    open >= 0
                    ? line.indexOf(
                          open,
                          "](")
                    : -1;
                const auto close =
                    middle >= 0
                    ? line.indexOfChar(
                          middle + 2,
                          ')')
                    : -1;
                if (open < 0
                    || middle < 0
                    || close < 0)
                    break;
                const auto label =
                    line.substring(
                        open + 1,
                        middle);
                const auto destination =
                    line.substring(
                        middle + 2,
                        close);
                line = line.substring(0, open)
                    + label
                    + (destination.isNotEmpty()
                           ? " (" + destination + ")"
                           : juce::String())
                    + line.substring(close + 1);
            }
        }
        result << line << "\n";
    }
    return result.trimEnd();
}
}

UserGuideComponent::UserGuideComponent()
{
    addAndMakeVisible(title);
    title.setText(
        "Studio Duo User Guide",
        juce::dontSendNotification);
    title.setFont(
        juce::Font(
            juce::FontOptions(
                24.0f,
                juce::Font::bold)));

    addAndMakeVisible(search);
    search.setTextToShowWhenEmpty(
        "Search the guide",
        juce::Colour(
            StudioColours::secondaryText));
    search.setColour(
        juce::TextEditor::backgroundColourId,
        juce::Colour(
            StudioColours::window));
    search.setColour(
        juce::TextEditor::textColourId,
        juce::Colour(
            StudioColours::text));
    search.setColour(
        juce::TextEditor::outlineColourId,
        juce::Colour(
            StudioColours::border));
    search.setColour(
        juce::TextEditor::focusedOutlineColourId,
        juce::Colour(
            StudioColours::orange));
    search.onReturnKey = [this]
    {
        findNext();
    };

    addAndMakeVisible(findButton);
    findButton.setTooltip(
        "Find the next matching phrase in the user guide");
    findButton.onClick = [this]
    {
        findNext();
    };

    addAndMakeVisible(searchStatus);
    searchStatus.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));

    addAndMakeVisible(guide);
    guide.setMultiLine(true, true);
    guide.setReadOnly(true);
    guide.setScrollbarsShown(true);
    guide.setCaretVisible(false);
    guide.setColour(
        juce::TextEditor::backgroundColourId,
        juce::Colour(
            StudioColours::window));
    guide.setColour(
        juce::TextEditor::textColourId,
        juce::Colour(
            StudioColours::text));
    guide.setColour(
        juce::TextEditor::outlineColourId,
        juce::Colour(
            StudioColours::border));
    guide.setFont(
        juce::Font(
            juce::FontOptions(14.0f)));
    guide.setText(
        readableGuideText(
            juce::String::fromUTF8(
                studio_documentation::
                    userguide_md,
                static_cast<int>(
                    studio_documentation::
                        userguide_mdSize))),
        false);
    setSize(900, 700);
}

UserGuideComponent::~UserGuideComponent()
{
    setLookAndFeel(nullptr);
}

void UserGuideComponent::paint(
    juce::Graphics& graphics)
{
    graphics.fillAll(
        juce::Colour(
            StudioColours::panel));
}

void UserGuideComponent::resized()
{
    auto bounds =
        getLocalBounds().reduced(20);
    title.setBounds(
        bounds.removeFromTop(36));
    bounds.removeFromTop(10);
    auto searchRow =
        bounds.removeFromTop(34);
    findButton.setBounds(
        searchRow.removeFromRight(120)
            .reduced(2));
    searchRow.removeFromRight(8);
    search.setBounds(searchRow);
    searchStatus.setBounds(
        bounds.removeFromTop(24));
    bounds.removeFromTop(6);
    guide.setBounds(bounds);
}

void UserGuideComponent::findNext()
{
    const auto query =
        search.getText().trim();
    if (query.isEmpty())
    {
        searchStatus.setText(
            "Enter text to search.",
            juce::dontSendNotification);
        search.grabKeyboardFocus();
        return;
    }
    const auto text = guide.getText();
    const auto start =
        guide.getHighlightedRegion().getEnd();
    const auto offset =
        text.substring(start)
            .indexOfIgnoreCase(query);
    const auto match = offset >= 0
        ? start + offset
        : text.indexOfIgnoreCase(query);
    if (match < 0)
    {
        searchStatus.setText(
            "No matches found.",
            juce::dontSendNotification);
        return;
    }
    guide.setCaretPosition(match);
    guide.setHighlightedRegion({
        match,
        match + query.length()
    });
    guide.grabKeyboardFocus();
    searchStatus.setText(
        "Match found.",
        juce::dontSendNotification);
}
}

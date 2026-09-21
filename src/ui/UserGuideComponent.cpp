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

juce::String expandedLinks(juce::String line)
{
    for (;;)
    {
        const auto open =
            line.indexOfChar('[');
        const auto middle =
            open >= 0
            ? line.indexOf(open, "](")
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
    return line;
}

void insertStyled(
    juce::TextEditor& editor,
    const juce::String& text,
    const juce::Font& font,
    juce::Colour colour)
{
    editor.setFont(font);
    editor.setColour(
        juce::TextEditor::textColourId,
        colour);
    editor.insertTextAtCaret(text);
}

void insertInlineMarkdown(
    juce::TextEditor& editor,
    juce::String line,
    const juce::Font& baseFont,
    juce::Colour baseColour)
{
    line = expandedLinks(std::move(line));
    const auto codeFont =
        juce::Font(
            juce::FontOptions(
                juce::Font::
                    getDefaultMonospacedFontName(),
                baseFont.getHeight(),
                juce::Font::plain));
    auto position = 0;
    while (position < line.length())
    {
        const auto bold =
            line.indexOf(position, "**");
        const auto code =
            line.indexOfChar(position, '`');
        const auto next =
            bold < 0
                ? code
                : code < 0
                ? bold
                : juce::jmin(bold, code);
        if (next < 0)
        {
            insertStyled(
                editor,
                line.substring(position),
                baseFont,
                baseColour);
            return;
        }
        insertStyled(
            editor,
            line.substring(position, next),
            baseFont,
            baseColour);
        if (next == bold)
        {
            const auto close =
                line.indexOf(next + 2, "**");
            if (close < 0)
            {
                insertStyled(
                    editor,
                    line.substring(next),
                    baseFont,
                    baseColour);
                return;
            }
            insertStyled(
                editor,
                line.substring(
                    next + 2,
                    close),
                baseFont.boldened(),
                baseColour);
            position = close + 2;
            continue;
        }
        const auto close =
            line.indexOfChar(
                next + 1,
                '`');
        if (close < 0)
        {
            insertStyled(
                editor,
                line.substring(next),
                baseFont,
                baseColour);
            return;
        }
        insertStyled(
            editor,
            line.substring(
                next + 1,
                close),
            codeFont,
            juce::Colour(
                StudioColours::green));
        position = close + 1;
    }
}

juce::StringArray tableCells(
    juce::String line)
{
    line = line.trim()
               .trimCharactersAtStart("|")
               .trimCharactersAtEnd("|");
    juce::StringArray cells;
    cells.addTokens(line, "|", {});
    for (auto& cell : cells)
        cell = cell.trim();
    return cells;
}

bool isTableSeparator(
    const juce::StringArray& cells)
{
    if (cells.isEmpty())
        return false;
    for (const auto& cell : cells)
    {
        if (!cell.containsChar('-')
            || !cell.containsOnly("-: "))
            return false;
    }
    return true;
}

juce::String repeated(
    juce::juce_wchar character,
    int count)
{
    juce::String result;
    for (auto index = 0; index < count; ++index)
        result << juce::String::charToString(
            character);
    return result;
}

juce::String paddedRight(
    juce::String value,
    int width)
{
    while (value.length() < width)
        value << " ";
    return value;
}

juce::String formatTable(
    const std::vector<juce::StringArray>& rows)
{
    std::vector<juce::StringArray> contentRows;
    for (const auto& row : rows)
        if (!isTableSeparator(row))
            contentRows.push_back(row);
    if (contentRows.empty())
        return {};
    auto columns = 0;
    for (const auto& row : contentRows)
        columns = juce::jmax(
            columns,
            row.size());
    std::vector<int> widths(
        static_cast<std::size_t>(columns),
        1);
    for (const auto& row : contentRows)
    {
        for (auto column = 0;
             column < row.size();
             ++column)
        {
            widths[static_cast<std::size_t>(
                column)] =
                juce::jmin(
                    48,
                    juce::jmax(
                        widths[
                            static_cast<
                                std::size_t>(
                                column)],
                        row[column].length()));
        }
    }
    const auto border = [&]
    {
        juce::String line { "+" };
        for (const auto width : widths)
            line << repeated('-', width + 2)
                 << "+";
        return line + "\n";
    };
    juce::String result = border();
    for (std::size_t rowIndex = 0;
         rowIndex < contentRows.size();
         ++rowIndex)
    {
        result << "|";
        for (auto column = 0;
             column < columns;
             ++column)
        {
            const auto value =
                column
                        < contentRows[rowIndex]
                              .size()
                    ? contentRows[rowIndex][column]
                          .substring(
                              0,
                              widths[
                                  static_cast<
                                      std::size_t>(
                                      column)])
                    : juce::String();
            result
                << " "
                << paddedRight(
                       value,
                       widths[
                           static_cast<
                               std::size_t>(
                               column)])
                << " |";
        }
        result << "\n";
        if (rowIndex == 0)
            result << border();
    }
    result << border();
    return result;
}

void renderMarkdown(
    juce::TextEditor& editor,
    const juce::String& markdown)
{
    const auto bodyFont =
        juce::Font(
            juce::FontOptions(14.0f));
    const auto headingFont =
        juce::Font(
            juce::FontOptions(
                18.0f,
                juce::Font::bold));
    const auto codeFont =
        juce::Font(
            juce::FontOptions(
                juce::Font::
                    getDefaultMonospacedFontName(),
                13.0f,
                juce::Font::plain));
    const auto textColour =
        juce::Colour(StudioColours::text);
    const auto secondaryColour =
        juce::Colour(
            StudioColours::secondaryText);
    auto lines =
        juce::StringArray::fromLines(markdown);
    editor.setReadOnly(false);
    editor.clear();
    auto inCodeBlock = false;
    for (auto index = 0;
         index < lines.size();
         ++index)
    {
        auto line = lines[index].trimEnd();
        if (line.trim().startsWith("```"))
        {
            inCodeBlock = !inCodeBlock;
            if (!inCodeBlock)
                insertStyled(
                    editor,
                    "\n",
                    bodyFont,
                    textColour);
            continue;
        }
        if (inCodeBlock)
        {
            insertStyled(
                editor,
                line + "\n",
                codeFont,
                juce::Colour(
                    StudioColours::green));
            continue;
        }
        if (line.trim().startsWith("|"))
        {
            std::vector<juce::StringArray>
                rows;
            while (index < lines.size()
                   && lines[index]
                          .trim()
                          .startsWith("|"))
            {
                rows.push_back(
                    tableCells(lines[index]));
                ++index;
            }
            --index;
            insertStyled(
                editor,
                formatTable(rows) + "\n",
                codeFont,
                textColour);
            continue;
        }
        if (line.startsWith("### "))
        {
            insertInlineMarkdown(
                editor,
                line.substring(4),
                headingFont,
                textColour);
            insertStyled(
                editor,
                "\n\n",
                bodyFont,
                textColour);
            continue;
        }
        if (line.startsWith("#### "))
        {
            insertInlineMarkdown(
                editor,
                line.substring(5),
                bodyFont.boldened(),
                textColour);
            insertStyled(
                editor,
                "\n",
                bodyFont,
                textColour);
            continue;
        }
        if (line.startsWith("- "))
        {
            insertStyled(
                editor,
                "  - ",
                bodyFont,
                juce::Colour(
                    StudioColours::orange));
            insertInlineMarkdown(
                editor,
                line.substring(2),
                bodyFont,
                textColour);
            insertStyled(
                editor,
                "\n",
                bodyFont,
                textColour);
            continue;
        }
        if (line.startsWith("> "))
        {
            insertStyled(
                editor,
                "| ",
                codeFont,
                juce::Colour(
                    StudioColours::orange));
            insertInlineMarkdown(
                editor,
                line.substring(2),
                bodyFont,
                secondaryColour);
            insertStyled(
                editor,
                "\n",
                bodyFont,
                textColour);
            continue;
        }
        if (line.trim().containsOnly("-")
            && line.trim().length() >= 3)
        {
            insertStyled(
                editor,
                repeated('-', 64) + "\n",
                codeFont,
                juce::Colour(
                    StudioColours::border));
            continue;
        }
        insertInlineMarkdown(
            editor,
            line,
            bodyFont,
            textColour);
        insertStyled(
            editor,
            "\n",
            bodyFont,
            textColour);
    }
    editor.setReadOnly(true);
    editor.setCaretVisible(false);
}

juce::String pageSummary(
    const juce::String& body)
{
    const auto readable =
        readableGuideText(body);
    for (auto line :
         juce::StringArray::fromLines(
             readable))
    {
        line = line.trim();
        if (line.length() < 20
            || line.startsWith("-")
            || line.startsWith("|"))
            continue;
        return line.substring(
            0,
            juce::jmin(140, line.length()))
            + (line.length() > 140
                   ? "..."
                   : juce::String());
    }
    return "Open this page for detailed instructions and reference.";
}

std::vector<UserGuideComponent::Page>
parseGuidePages(const juce::String& markdown)
{
    std::vector<UserGuideComponent::Page> pages;
    juce::String introduction;
    juce::String currentTitle;
    juce::String currentBody;
    const auto flushPage = [&]
    {
        if (currentTitle.isEmpty())
            return;
        pages.push_back({
            currentTitle,
            currentBody.trimEnd()
        });
        currentTitle.clear();
        currentBody.clear();
    };
    for (auto line :
         juce::StringArray::fromLines(markdown))
    {
        if (line.startsWith("## "))
        {
            flushPage();
            currentTitle =
                line.substring(3).trim();
            continue;
        }
        if (currentTitle.isEmpty())
        {
            if (!line.startsWith("# "))
                introduction << line << "\n";
        }
        else
        {
            currentBody << line << "\n";
        }
    }
    flushPage();

    juce::String overview =
        introduction.trimEnd();
    if (overview.isNotEmpty())
        overview << "\n\n";
    overview << "### Guide index\n\n";
    for (std::size_t index = 0;
         index < pages.size();
         ++index)
    {
        overview
            << "**"
            << juce::String(
                   static_cast<int>(index) + 1)
            << ". "
            << pages[index].title
            << "**"
            << "\n"
            << pageSummary(
                   pages[index].body)
            << "\n\n";
    }
    pages.insert(
        pages.begin(),
        {
            "Overview",
            overview.trimEnd()
        });
    return pages;
}
}

UserGuideComponent::UserGuideComponent()
    : pages(parseGuidePages(
          juce::String::fromUTF8(
              studio_documentation::
                  userguide_md,
              static_cast<int>(
                  studio_documentation::
                      userguide_mdSize)))),
      index("User guide index", this)
{
    addAndMakeVisible(indexTitle);
    indexTitle.setText(
        "USER GUIDE",
        juce::dontSendNotification);
    indexTitle.setFont(
        juce::Font(
            juce::FontOptions(
                20.0f,
                juce::Font::bold)));

    addAndMakeVisible(indexSubtitle);
    indexSubtitle.setText(
        "CONTENTS",
        juce::dontSendNotification);
    indexSubtitle.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));
    indexSubtitle.setFont(
        juce::Font(
            juce::FontOptions(
                11.0f,
                juce::Font::bold)));

    addAndMakeVisible(index);
    index.setRowHeight(44);
    index.setColour(
        juce::ListBox::backgroundColourId,
        juce::Colour(
            StudioColours::panel));
    index.setColour(
        juce::ListBox::outlineColourId,
        juce::Colour(
            StudioColours::border));
    index.setOutlineThickness(1);

    addAndMakeVisible(breadcrumb);
    breadcrumb.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));
    breadcrumb.setFont(
        juce::Font(
            juce::FontOptions(12.0f)));

    addAndMakeVisible(pageTitle);
    pageTitle.setFont(
        juce::Font(
            juce::FontOptions(
                26.0f,
                juce::Font::bold)));

    addAndMakeVisible(search);
    search.setTextToShowWhenEmpty(
        "Search this page",
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
        "Find the next matching phrase on this page");
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

    addAndMakeVisible(previousButton);
    previousButton.setTooltip(
        "Open the previous guide page");
    previousButton.onClick = [this]
    {
        showPage(currentPage - 1);
    };

    addAndMakeVisible(pagePosition);
    pagePosition.setJustificationType(
        juce::Justification::centred);
    pagePosition.setColour(
        juce::Label::textColourId,
        juce::Colour(
            StudioColours::secondaryText));

    addAndMakeVisible(nextButton);
    nextButton.setTooltip(
        "Open the next guide page");
    nextButton.onClick = [this]
    {
        showPage(currentPage + 1);
    };

    index.updateContent();
    showPage(0);
    setSize(1040, 720);
}

UserGuideComponent::~UserGuideComponent()
{
    index.setModel(nullptr);
    setLookAndFeel(nullptr);
}

void UserGuideComponent::paint(
    juce::Graphics& graphics)
{
    graphics.fillAll(
        juce::Colour(
            StudioColours::panel));
    graphics.setColour(
        juce::Colour(
            StudioColours::border));
    graphics.drawVerticalLine(
        274,
        16.0f,
        static_cast<float>(
            getHeight() - 16));
}

void UserGuideComponent::resized()
{
    auto bounds =
        getLocalBounds().reduced(18);
    auto sidebar =
        bounds.removeFromLeft(246);
    indexTitle.setBounds(
        sidebar.removeFromTop(32));
    indexSubtitle.setBounds(
        sidebar.removeFromTop(24));
    sidebar.removeFromTop(6);
    index.setBounds(sidebar);

    bounds.removeFromLeft(24);
    breadcrumb.setBounds(
        bounds.removeFromTop(22));
    pageTitle.setBounds(
        bounds.removeFromTop(42));
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
    auto navigation =
        bounds.removeFromBottom(38);
    previousButton.setBounds(
        navigation.removeFromLeft(120)
            .reduced(2));
    nextButton.setBounds(
        navigation.removeFromRight(120)
            .reduced(2));
    pagePosition.setBounds(navigation);
    bounds.removeFromBottom(8);
    guide.setBounds(bounds);
}

void UserGuideComponent::lookAndFeelChanged()
{
    if (!pages.empty())
        showPage(currentPage);
    index.repaint();
}

int UserGuideComponent::getNumRows()
{
    return static_cast<int>(pages.size());
}

void UserGuideComponent::paintListBoxItem(
    int row,
    juce::Graphics& graphics,
    int width,
    int height,
    bool selected)
{
    if (row < 0
        || row >= static_cast<int>(
            pages.size()))
        return;
    if (selected)
    {
        graphics.setColour(
            juce::Colour(
                StudioColours::orange)
                .withAlpha(0.18f));
        graphics.fillRoundedRectangle(
            juce::Rectangle<float>(
                4.0f,
                3.0f,
                static_cast<float>(width - 8),
                static_cast<float>(height - 6)),
            4.0f);
    }
    graphics.setColour(
        juce::Colour(
            selected
                ? StudioColours::orange
                : StudioColours::secondaryText));
    graphics.setFont(
        juce::Font(
            juce::FontOptions(
                10.5f,
                juce::Font::bold)));
    graphics.drawText(
        row == 0
            ? "START"
            : juce::String(row)
                  .paddedLeft('0', 2),
        12,
        0,
        42,
        height,
        juce::Justification::centredLeft);
    graphics.setColour(
        juce::Colour(
            StudioColours::text));
    graphics.setFont(
        juce::Font(
            juce::FontOptions(
                13.0f,
                selected
                    ? juce::Font::bold
                    : juce::Font::plain)));
    graphics.drawFittedText(
        pages[static_cast<std::size_t>(
                  row)]
            .title,
        56,
        0,
        width - 64,
        height,
        juce::Justification::centredLeft,
        2,
        0.8f);
}

void UserGuideComponent::selectedRowsChanged(
    int lastRowSelected)
{
    if (lastRowSelected != currentPage)
        showPage(lastRowSelected);
}

void UserGuideComponent::showPage(int page)
{
    if (page < 0
        || page >= static_cast<int>(
            pages.size()))
        return;
    currentPage = page;
    const auto& selected =
        pages[static_cast<std::size_t>(
            currentPage)];
    breadcrumb.setText(
        "User Guide  /  " + selected.title,
        juce::dontSendNotification);
    pageTitle.setText(
        selected.title,
        juce::dontSendNotification);
    renderMarkdown(
        guide,
        selected.body);
    guide.setCaretPosition(0);
    guide.setHighlightedRegion({});
    searchStatus.setText(
        {},
        juce::dontSendNotification);
    pagePosition.setText(
        juce::String(currentPage + 1)
            + " / "
            + juce::String(
                static_cast<int>(
                    pages.size())),
        juce::dontSendNotification);
    previousButton.setEnabled(
        currentPage > 0);
    nextButton.setEnabled(
        currentPage + 1
        < static_cast<int>(
              pages.size()));
    index.selectRow(
        currentPage,
        false,
        false);
    index.scrollToEnsureRowIsOnscreen(
        currentPage);
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
            "No matches found on this page.",
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
        offset >= 0
            ? "Match found."
            : "Wrapped to the first match.",
        juce::dontSendNotification);
}
}

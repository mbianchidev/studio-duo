#include "InfoPanelComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <memory>

namespace studio
{
namespace
{
class HistoryPanel final : public juce::Component
{
public:
    explicit HistoryPanel(InfoPanelComponent& ownerToUse)
        : owner(ownerToUse),
          clearAll(
              StudioIcon::deleteItem,
              "Clear all messages",
              "Clear all status and error messages")
    {
        addAndMakeVisible(clearAll);
        clearAll.onClick = [this]
        {
            owner.clearHistory();
            rebuild();
        };
        rebuild();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(
            juce::Colour(StudioColours::panel));
        graphics.setColour(
            juce::Colour(StudioColours::text));
        graphics.setFont(
            juce::Font(
                juce::FontOptions(15.0f,
                                  juce::Font::bold)));
        graphics.drawText(
            "Status history",
            16,
            8,
            getWidth() - 64,
            28,
            juce::Justification::centredLeft);

        graphics.setColour(
            juce::Colour(StudioColours::border));
        graphics.drawHorizontalLine(
            42,
            0.0f,
            static_cast<float>(getWidth()));

        auto y = 48;
        const auto& history = owner.history();
        if (history.empty())
        {
            graphics.setColour(
                juce::Colour(
                    StudioColours::secondaryText));
            graphics.setFont(11.0f);
            graphics.drawText(
                "No status messages.",
                16,
                y,
                getWidth() - 32,
                36,
                juce::Justification::centredLeft);
            return;
        }

        for (auto index = history.size();
             index > 0;
             --index)
        {
            const auto& entry = history[index - 1];
            const juce::Rectangle<int> row(
                10,
                y,
                getWidth() - 20,
                52);
            graphics.setColour(
                juce::Colour(
                    entry.error
                        ? 0xff35211f
                        : StudioColours::raised));
            graphics.fillRoundedRectangle(
                row.toFloat(),
                4.0f);
            graphics.setColour(
                entry.error
                    ? juce::Colour(StudioColours::orange)
                    : juce::Colour(
                        StudioColours::secondaryText));
            graphics.fillRoundedRectangle(
                static_cast<float>(row.getX() + 6),
                static_cast<float>(row.getY() + 9),
                4.0f,
                34.0f,
                2.0f);
            graphics.setColour(
                juce::Colour(StudioColours::text));
            graphics.setFont(10.5f);
            graphics.drawFittedText(
                entry.message,
                row.withTrimmedLeft(16)
                    .withTrimmedRight(40)
                    .withHeight(30),
                juce::Justification::centredLeft,
                2);
            graphics.setColour(
                juce::Colour(
                    StudioColours::secondaryText));
            graphics.setFont(8.5f);
            graphics.drawText(
                entry.timestamp,
                row.withTrimmedLeft(16)
                    .withTrimmedRight(40)
                    .withTrimmedTop(32),
                juce::Justification::centredLeft);
            y += 58;
        }
    }

    void resized() override
    {
        clearAll.setBounds(
            getWidth() - 44,
            8,
            32,
            28);
        auto y = 58;
        for (auto& button : removeButtons)
        {
            button->setBounds(
                getWidth() - 48,
                y,
                28,
                28);
            y += 58;
        }
    }

private:
    void rebuild()
    {
        removeButtons.clear();
        const auto& history = owner.history();
        for (auto index = history.size();
             index > 0;
             --index)
        {
            const auto id = history[index - 1].id;
            auto button =
                std::make_unique<StudioIconButton>(
                    StudioIcon::close,
                    "Clear message",
                    "Remove this status message");
            button->onClick = [this, id]
            {
                owner.removeEntry(id);
                rebuild();
            };
            addAndMakeVisible(*button);
            removeButtons.push_back(std::move(button));
        }
        const auto rows = juce::jmax(
            1,
            static_cast<int>(history.size()));
        setSize(500, juce::jmin(520, 54 + rows * 58));
        resized();
        repaint();
    }

    InfoPanelComponent& owner;
    StudioIconButton clearAll;
    std::vector<std::unique_ptr<StudioIconButton>>
        removeButtons;
};
}

InfoPanelComponent::InfoPanelComponent()
{
    setWantsKeyboardFocus(true);
    setName("Status history");
    setTitle("Show status and error history");
}

void InfoPanelComponent::pushMessage(
    juce::String message,
    bool error)
{
    message = message.trim();
    if (message.isEmpty())
        return;
    if (!entries.empty()
        && entries.back().message == message
        && entries.back().error == error)
        return;
    entries.push_back({
        nextId++,
        std::move(message),
        juce::Time::getCurrentTime().toString(
            false,
            true,
            true,
            true),
        error
    });
    if (entries.size() > 200)
        entries.erase(entries.begin());
    repaint();
}

juce::String InfoPanelComponent::latestMessage() const
{
    return entries.empty()
        ? juce::String()
        : entries.back().message;
}

bool InfoPanelComponent::latestIsError() const noexcept
{
    return !entries.empty() && entries.back().error;
}

std::size_t InfoPanelComponent::historySize() const noexcept
{
    return entries.size();
}

const std::vector<InfoPanelComponent::Entry>&
InfoPanelComponent::history() const noexcept
{
    return entries;
}

void InfoPanelComponent::removeEntry(std::uint64_t id)
{
    std::erase_if(entries, [id](const auto& entry)
    {
        return entry.id == id;
    });
    repaint();
}

void InfoPanelComponent::clearHistory()
{
    entries.clear();
    repaint();
}

void InfoPanelComponent::paint(juce::Graphics& graphics)
{
    const auto bounds =
        getLocalBounds().toFloat().reduced(0.5f);
    const auto error = latestIsError();
    graphics.setColour(
        juce::Colour(
            error
                ? 0xff35211f
                : StudioColours::raised));
    graphics.fillRoundedRectangle(bounds, 4.0f);
    graphics.setColour(
        juce::Colour(
            error
                ? StudioColours::orange
                : StudioColours::border));
    graphics.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    drawStudioIcon(
        graphics,
        StudioIcon::info,
        {
            8.0f,
            bounds.getCentreY() - 8.0f,
            16.0f,
            16.0f
        },
        error
            ? juce::Colour(StudioColours::orange)
            : juce::Colour(StudioColours::secondaryText),
        1.4f);

    graphics.setColour(
        juce::Colour(
            error
                ? StudioColours::orange
                : StudioColours::secondaryText));
    graphics.setFont(10.0f);
    graphics.drawFittedText(
        latestMessage().isNotEmpty()
            ? latestMessage()
            : "No status messages",
        getLocalBounds()
            .withTrimmedLeft(30)
            .withTrimmedRight(26),
        juce::Justification::centredRight,
        1);

    if (entries.size() > 1)
    {
        graphics.setColour(
            juce::Colour(StudioColours::transportRaised));
        graphics.fillEllipse(
            static_cast<float>(getWidth() - 22),
            static_cast<float>(getHeight() / 2 - 8),
            16.0f,
            16.0f);
        graphics.setColour(
            juce::Colour(StudioColours::text));
        graphics.setFont(
            juce::Font(
                juce::FontOptions(8.0f,
                                  juce::Font::bold)));
        graphics.drawText(
            juce::String(
                static_cast<int>(entries.size())),
            getWidth() - 22,
            getHeight() / 2 - 8,
            16,
            16,
            juce::Justification::centred);
    }
    if (hasKeyboardFocus(true))
    {
        graphics.setColour(
            juce::Colour(StudioColours::orange));
        graphics.drawRoundedRectangle(
            bounds.reduced(1.5f),
            3.0f,
            1.5f);
    }
}

void InfoPanelComponent::mouseDown(
    const juce::MouseEvent&)
{
    showHistory();
}

bool InfoPanelComponent::keyPressed(
    const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey
        || key == juce::KeyPress::spaceKey)
    {
        showHistory();
        return true;
    }
    return false;
}

void InfoPanelComponent::showHistory()
{
    auto panel = std::make_unique<HistoryPanel>(*this);
    juce::CallOutBox::launchAsynchronously(
        std::move(panel),
        getScreenBounds(),
        nullptr);
}
}

#include "InfoPanelComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <memory>

namespace studio
{
namespace
{
class HistoryPanel final : public juce::Component,
                           private juce::ListBoxModel,
                           private juce::ChangeListener
{
public:
    explicit HistoryPanel(InfoPanelComponent& ownerToUse)
        : owner(&ownerToUse),
          clearAll(
              StudioIcon::deleteItem,
              "Clear all messages",
              "Clear all status and error messages"),
          clearSelected(StudioIcon::close, "Clear selected message", "Delete clears the selected message")
    {
        setTitle("Status and error history");
        addAndMakeVisible(clearAll);
        clearAll.onClick = [this]
        {
            if (owner != nullptr)
                owner->clearHistory();
            refresh();
        };
        addAndMakeVisible(clearSelected);
        clearSelected.onClick = [this] { removeSelected(); };
        messages.setComponentID("status-history.list");
        messages.setTitle("Status and error messages");
        messages.setDescription("Newest first. Arrow keys select; Delete clears the selected message.");
        messages.setWantsKeyboardFocus(true);
        messages.setRowHeight(58);
        messages.setOutlineThickness(1);
        messages.setColour(juce::ListBox::backgroundColourId, juce::Colour(StudioColours::panel));
        messages.setColour(juce::ListBox::outlineColourId, juce::Colour(StudioColours::border));
        addAndMakeVisible(messages);
        ownerToUse.addChangeListener(this);
        refresh();
    }

    ~HistoryPanel() override
    {
        if (owner != nullptr)
            owner->removeChangeListener(this);
        messages.setModel(nullptr);
    }

    void focusMessages()
    {
        if (messages.isVisible())
            messages.grabKeyboardFocus();
        else
            clearAll.grabKeyboardFocus();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getKeyCode() != juce::KeyPress::escapeKey)
            return false;
        if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
        {
            callout->dismiss();
            const auto safeOwner = owner;
            juce::MessageManager::callAsync([safeOwner]
            {
                if (safeOwner != nullptr && safeOwner->isShowing())
                    safeOwner->grabKeyboardFocus();
            });
            return true;
        }
        return false;
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
            getWidth() - 112,
            28,
            juce::Justification::centredLeft);

        graphics.setColour(
            juce::Colour(StudioColours::border));
        graphics.drawHorizontalLine(
            42,
            0.0f,
            static_cast<float>(getWidth()));

        if (entries.empty())
        {
            graphics.setColour(
                juce::Colour(
                    StudioColours::secondaryText));
            graphics.setFont(11.0f);
            graphics.drawText(
                "No status messages.",
                16,
                54,
                getWidth() - 32,
                36,
                juce::Justification::centredLeft);
        }
    }

    int getNumRows() override { return static_cast<int>(entries.size()); }

    const InfoPanelComponent::Entry* entryForRow(int row) const
    {
        return row >= 0 && row < static_cast<int>(entries.size())
            ? &entries[entries.size() - 1 - static_cast<std::size_t>(row)] : nullptr;
    }

    juce::String getNameForRow(int row) override
    {
        const auto* entry = entryForRow(row);
        return entry != nullptr ? juce::String(entry->error ? "Error: " : "Status: ")
            + entry->message + "; " + entry->timestamp : juce::String();
    }

    void paintListBoxItem(int rowIndex, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        const auto* entry = entryForRow(rowIndex);
        if (entry == nullptr)
            return;
        const auto row = juce::Rectangle<int>(0, 2, width, height - 4).reduced(4, 0);
        graphics.setColour(juce::Colour(StudioColours::raised));
        graphics.fillRoundedRectangle(row.toFloat(), 4.0f);
        if (selected)
        {
            graphics.setColour(juce::Colour(StudioColours::orange));
            graphics.drawRoundedRectangle(row.toFloat().reduced(1.0f), 4.0f, 2.0f);
        }
        graphics.setColour(juce::Colour(StudioColours::text));
        graphics.setFont(13.0f);
        graphics.drawFittedText(juce::String(entry->error ? "Error: " : "Status: ") + entry->message,
            row.reduced(8, 2).withHeight(30), juce::Justification::centredLeft, 2);
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(11.0f);
        graphics.drawText(entry->timestamp, row.reduced(8, 0).withTrimmedTop(32),
            juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int) override
    {
        clearSelected.setEnabled(entryForRow(messages.getSelectedRow()) != nullptr);
    }

    void deleteKeyPressed(int) override { removeSelected(); }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    void removeSelected()
    {
        if (const auto* entry = entryForRow(messages.getSelectedRow()); entry != nullptr && owner != nullptr)
            owner->removeEntry(entry->id);
        refresh();
        if (isShowing())
            focusMessages();
    }

    void resized() override
    {
        clearAll.setBounds(
            getWidth() - 44,
            8,
            32,
            28);
        clearSelected.setBounds(getWidth() - 84, 8, 32, 28);
        messages.setBounds(8, 48, getWidth() - 16, getHeight() - 56);
    }

private:
    void refresh()
    {
        const auto hadListFocus = messages.hasKeyboardFocus(true);
        const auto selectedRow = messages.getSelectedRow();
        const auto* selected = entryForRow(selectedRow);
        const auto selectedId = selected != nullptr ? std::optional<std::uint64_t>(selected->id) : std::nullopt;
        entries = owner != nullptr ? owner->history() : std::vector<InfoPanelComponent::Entry>();
        messages.updateContent();
        auto next = std::clamp(selectedRow, 0, std::max(0, getNumRows() - 1));
        if (selectedId)
            for (int row = 0; row < getNumRows(); ++row)
                if (entryForRow(row)->id == *selectedId)
                    next = row;
        if (!entries.empty())
            messages.selectRow(next);
        else
            messages.deselectAllRows();
        messages.setVisible(!entries.empty());
        clearSelected.setEnabled(entryForRow(messages.getSelectedRow()) != nullptr);
        clearAll.setEnabled(owner != nullptr);
        const auto rows = juce::jmax(
            1,
            getNumRows());
        setSize(500, juce::jmin(520, 54 + rows * 58));
        resized();
        repaint();
        if (hadListFocus && isShowing())
            focusMessages();
    }

    juce::Component::SafePointer<InfoPanelComponent> owner;
    std::vector<InfoPanelComponent::Entry> entries;
    StudioIconButton clearAll, clearSelected;
    juce::ListBox messages { "Status messages", this };
};
}

InfoPanelComponent::InfoPanelComponent()
{
    setWantsKeyboardFocus(true);
    setName("Status history");
    setTitle("Show status and error history");
    updateAccessibleValue();
}

juce::String InfoPanelComponent::accessibleValue() const
{
    return displayedMessage.isEmpty() ? "No status messages"
        : juce::String(displayedError ? "Error: " : "Status: ") + displayedMessage;
}

void InfoPanelComponent::updateAccessibleValue()
{
    const auto value = accessibleValue();
    if (getDescription() != value)
    {
        setDescription(value);
        setTooltip(value);
        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    }
    repaint();
}

std::unique_ptr<juce::AccessibilityHandler> InfoPanelComponent::createAccessibilityHandler()
{
    class StatusValue final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit StatusValue(InfoPanelComponent& component) : owner(component) {}
        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return owner.accessibleValue(); }
        void setValueAsString(const juce::String&) override {}
    private:
        InfoPanelComponent& owner;
    };
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] { showHistory(); })
            .addAction(juce::AccessibilityActionType::focus, [this] { grabKeyboardFocus(); }),
        juce::AccessibilityHandler::Interfaces(std::make_unique<StatusValue>(*this)));
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
    displayedMessage = entries.back().message;
    displayedError = error;
    displayedEntryId = entries.back().id;
    if (entries.size() > 200)
        entries.erase(entries.begin());
    updateAccessibleValue();
    sendChangeMessage();
}

void InfoPanelComponent::setLiveMessage(
    juce::String message,
    bool error)
{
    message = message.trim();
    if (message.isEmpty())
        return;
    displayedMessage = std::move(message);
    displayedError = error;
    displayedEntryId.reset();
    updateAccessibleValue();
}

juce::String InfoPanelComponent::latestMessage() const
{
    return displayedMessage;
}

bool InfoPanelComponent::latestIsError() const noexcept
{
    return displayedError;
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
    const auto removedDisplayed =
        displayedEntryId.has_value()
        && *displayedEntryId == id;
    std::erase_if(entries, [id](const auto& entry)
    {
        return entry.id == id;
    });
    if (removedDisplayed)
    {
        if (entries.empty())
        {
            displayedMessage.clear();
            displayedError = false;
            displayedEntryId.reset();
        }
        else
        {
            displayedMessage = entries.back().message;
            displayedError = entries.back().error;
            displayedEntryId = entries.back().id;
        }
    }
    updateAccessibleValue();
    sendChangeMessage();
}

void InfoPanelComponent::clearHistory()
{
    entries.clear();
    displayedMessage.clear();
    displayedError = false;
    displayedEntryId.reset();
    updateAccessibleValue();
    sendChangeMessage();
}

void InfoPanelComponent::paint(juce::Graphics& graphics)
{
    const auto bounds =
        getLocalBounds().toFloat().reduced(0.5f);
    const auto error = latestIsError();
    graphics.setColour(
        juce::Colour(StudioColours::raised));
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
                ? StudioColours::text
                : StudioColours::secondaryText));
    graphics.setFont(10.0f);
    graphics.drawFittedText(
        accessibleValue(),
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
    const auto safe = juce::Component::SafePointer<HistoryPanel>(panel.get());
    juce::CallOutBox::launchAsynchronously(
        std::move(panel),
        getScreenBounds(),
        nullptr);
    juce::MessageManager::callAsync([safe]
    {
        if (safe != nullptr && safe->isShowing())
            safe->focusMessages();
    });
}

std::unique_ptr<juce::Component> InfoPanelComponent::createHistoryView()
{
    return std::make_unique<HistoryPanel>(*this);
}
}

#pragma once

#include "StudioIconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace studio
{
class InfoPanelComponent final : public juce::Component
{
public:
    struct Entry
    {
        std::uint64_t id = 0;
        juce::String message;
        juce::String timestamp;
        bool error = false;
    };

    InfoPanelComponent();

    void pushMessage(juce::String message, bool error);
    void setLiveMessage(juce::String message, bool error);
    [[nodiscard]] juce::String latestMessage() const;
    [[nodiscard]] bool latestIsError() const noexcept;
    [[nodiscard]] std::size_t historySize() const noexcept;
    [[nodiscard]] const std::vector<Entry>& history() const noexcept;
    void removeEntry(std::uint64_t id);
    void clearHistory();

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void showHistory();

    std::vector<Entry> entries;
    juce::String displayedMessage;
    bool displayedError = false;
    std::optional<std::uint64_t> displayedEntryId;
    std::uint64_t nextId = 1;
};
}

#include "PluginInsertPanel.h"

#include "StudioTheme.h"

#include <algorithm>

namespace studio
{
void PluginInsertPanel::setProject(const Project* value)
{
    project = value;
    repaint();
}

void PluginInsertPanel::setTrack(const juce::String& value)
{
    trackId = value;
    repaint();
}

void PluginInsertPanel::setRuntimeStatuses(
    std::vector<StudioAudioEngine::PluginRuntimeStatus> value,
    std::uint64_t lateBlocks)
{
    runtimeStatuses = std::move(value);
    lateBlockCount = lateBlocks;
    repaint();
}

void PluginInsertPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
    graphics.drawText("INSERTS",
                      0,
                      0,
                      getWidth(),
                      20,
                      juce::Justification::centredLeft);

    const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
    if (track == nullptr || track->inserts.empty())
    {
        graphics.setColour(juce::Colour(StudioColours::secondaryText));
        graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
        graphics.drawFittedText("Double-click a catalog plugin to add a sandboxed insert.",
                                0,
                                26,
                                getWidth(),
                                42,
                                juce::Justification::topLeft,
                                2);
        return;
    }

    auto y = 24;
    for (std::size_t index = 0; index < track->inserts.size(); ++index)
    {
        const auto& insert = track->inserts[index];
        const juce::Rectangle<int> row(0, y, getWidth(), 48);
        graphics.setColour(juce::Colour(insert.bypassed
                                            ? 0xff191c1f
                                            : StudioColours::raised));
        graphics.fillRoundedRectangle(row.toFloat(), 4.0f);
        graphics.setColour(juce::Colour(insert.missing
                                            ? StudioColours::orange
                                            : StudioColours::border));
        graphics.drawRoundedRectangle(row.toFloat(), 4.0f, 1.0f);

        graphics.setColour(juce::Colour(insert.bypassed
                                            ? StudioColours::secondaryText
                                            : StudioColours::text));
        graphics.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
        graphics.drawFittedText(juce::String(static_cast<int>(index + 1))
                                    + "  "
                                    + insert.name,
                                8,
                                y + 4,
                                getWidth() - 76,
                                18,
                                juce::Justification::centredLeft,
                                1);

        const auto status = std::find_if(
            runtimeStatuses.cbegin(),
            runtimeStatuses.cend(),
            [&insert](const auto& candidate)
            {
                return candidate.insertId == insert.id;
            });
        auto stateText = juce::String("SANDBOX");
        auto stateColour = juce::Colour(StudioColours::green);
        if (status != runtimeStatuses.cend())
        {
            switch (status->state)
            {
                case StudioAudioEngine::PluginRuntimeStatus::State::bypassed:
                    stateText = "BYPASSED";
                    stateColour = juce::Colour(StudioColours::amber);
                    break;
                case StudioAudioEngine::PluginRuntimeStatus::State::missing:
                    stateText = "MISSING";
                    stateColour = juce::Colour(StudioColours::orange);
                    break;
                case StudioAudioEngine::PluginRuntimeStatus::State::loading:
                    stateText = "LOADING";
                    stateColour = juce::Colour(StudioColours::amber);
                    break;
                case StudioAudioEngine::PluginRuntimeStatus::State::ready:
                    stateText = "READY";
                    break;
                case StudioAudioEngine::PluginRuntimeStatus::State::failed:
                    stateText = "CRASHED - CLICK TO RELOAD";
                    stateColour = juce::Colour(StudioColours::orange);
                    break;
            }
        }

        const auto modeText =
            insert.bridgeMode == PluginBridgeMode::sandboxed
            ? juce::String("SANDBOX")
            : insert.bridgeMode == PluginBridgeMode::araCompatibility
                ? juce::String("ARA 2 / REDUCED ISOLATION")
                : juce::String("TRUSTED IN-PROCESS");
        if (insert.recoveryDisabled)
        {
            stateText = "RECOVERY DISABLED";
            stateColour = juce::Colour(StudioColours::orange);
        }
        const auto detail = modeText
            + "  |  "
            + stateText
            + "  |  "
            + insert.format;
        graphics.setColour(stateColour);
        graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
        graphics.drawFittedText(detail,
                                8,
                                y + 24,
                                getWidth() - 76,
                                15,
                                juce::Justification::centredLeft,
                                1);

        graphics.setColour(juce::Colour(insert.bypassed
                                            ? StudioColours::amber
                                            : StudioColours::secondaryText));
        graphics.setFont(juce::Font(juce::FontOptions(9.0f, juce::Font::bold)));
        graphics.drawText("BYP",
                          getWidth() - 64,
                          y,
                          32,
                          48,
                          juce::Justification::centred);
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.drawText("X",
                          getWidth() - 30,
                          y,
                          30,
                          48,
                          juce::Justification::centred);
        y += 54;
    }

    if (lateBlockCount > 0)
    {
        graphics.setColour(juce::Colour(StudioColours::amber));
        graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
        graphics.drawText(juce::String(lateBlockCount) + " late sandbox blocks",
                          0,
                          getHeight() - 18,
                          getWidth(),
                          18,
                          juce::Justification::centredLeft);
    }
}

void PluginInsertPanel::mouseDown(const juce::MouseEvent& event)
{
    const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
    if (track == nullptr || event.position.y < 24.0f)
        return;

    const auto index = static_cast<int>((event.position.y - 24.0f) / 54.0f);
    if (index < 0 || index >= static_cast<int>(track->inserts.size()))
        return;

    const auto& insert = track->inserts[static_cast<std::size_t>(index)];
    if (event.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem(
            "Sandboxed DSP",
            true,
            insert.bridgeMode == PluginBridgeMode::sandboxed,
            [this, selectedTrackId = track->id, insertId = insert.id]
            {
                if (onModeChange)
                    onModeChange(selectedTrackId,
                                 insertId,
                                 PluginBridgeMode::sandboxed);
            });
        menu.addItem(
            "ARA 2 compatibility (reduced isolation)",
            insert.araCapable,
            insert.bridgeMode == PluginBridgeMode::araCompatibility,
            [this, selectedTrackId = track->id, insertId = insert.id]
            {
                if (onModeChange)
                    onModeChange(selectedTrackId,
                                 insertId,
                                 PluginBridgeMode::araCompatibility);
            });
        menu.addItem(
            "Trusted in-process",
            true,
            insert.bridgeMode == PluginBridgeMode::trustedInProcess,
            [this, selectedTrackId = track->id, insertId = insert.id]
            {
                if (onModeChange)
                    onModeChange(selectedTrackId,
                                 insertId,
                                 PluginBridgeMode::trustedInProcess);
            });
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this));
        return;
    }
    const auto failed = std::any_of(
        runtimeStatuses.cbegin(),
        runtimeStatuses.cend(),
        [&insert](const auto& status)
        {
            return status.insertId == insert.id
                && status.state == StudioAudioEngine::PluginRuntimeStatus::State::failed;
        });
    if (event.position.x >= static_cast<float>(getWidth() - 30))
    {
        if (onRemove)
            onRemove(track->id, insert.id);
    }
    else if (event.position.x >= static_cast<float>(getWidth() - 64))
    {
        if (onBypass)
            onBypass(track->id, insert.id, !insert.bypassed);
    }
    else if (failed && onReload)
    {
        onReload();
    }
}
}

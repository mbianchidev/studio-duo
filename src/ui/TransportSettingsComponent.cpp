#include "TransportSettingsComponent.h"

#include "NumericInput.h"
#include "StudioTheme.h"
#include "model/ProjectCommands.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <utility>

namespace studio
{
namespace
{
juce::String numberText(double value, int decimals = 12)
{
    auto text = juce::String(value, decimals);
    if (text.containsChar('.'))
        text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text;
}

std::optional<double> readNumber(
    const juce::TextEditor& editor, const juce::String& title, juce::String& error)
{
    const auto value = parseFiniteNumber(editor.getText());
    if (!value)
        error = title + " must be a complete finite number, without units or other text.";
    return value;
}

std::optional<double> readBoundedNumber(
    const juce::TextEditor& editor, double minimum, double maximum,
    const juce::String& title, juce::String& error)
{
    const auto value = readNumber(editor, title, error);
    if (!value)
        return std::nullopt;
    if (*value < minimum || *value > maximum)
    {
        error = title + " must be between " + numberText(minimum)
            + " and " + numberText(maximum) + ".";
        return std::nullopt;
    }
    return value;
}

std::optional<int> readInteger(
    const juce::String& text, int minimum, int maximum,
    const juce::String& title, juce::String& error)
{
    const auto token = text.trim().toStdString();
    int value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size()
        || value < minimum || value > maximum)
    {
        error = title + " must be a whole number between "
            + juce::String(minimum) + " and " + juce::String(maximum) + ".";
        return std::nullopt;
    }
    return value;
}

juce::String accentText(const std::vector<int>& beats)
{
    juce::StringArray tokens;
    for (const auto beat : beats)
        tokens.add(juce::String(beat));
    return tokens.joinIntoString(", ");
}

std::optional<std::vector<int>> readAccents(const juce::String& text, juce::String& error)
{
    std::vector<int> result;
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return result;
    std::array<bool, 33> seen {};
    auto start = 0;
    for (;;)
    {
        const auto separator = trimmed.indexOfChar(start, ',');
        const auto token = separator < 0 ? trimmed.substring(start)
                                        : trimmed.substring(start, separator);
        const auto beat = readInteger(token, 1, 32, "Each accent beat", error);
        if (!beat)
            return std::nullopt;
        if (seen[static_cast<size_t>(*beat)])
        {
            error = "Accent beats must be unique. Use a list such as 1, 4, or leave it empty.";
            return std::nullopt;
        }
        seen[static_cast<size_t>(*beat)] = true;
        result.push_back(*beat);
        if (separator < 0)
            return result;
        start = separator + 1;
    }
}

class ApplyButton final : public juce::TextButton
{
public:
    void paintButton(juce::Graphics& graphics, bool highlighted, bool down) override
    {
        juce::TextButton::paintButton(graphics, highlighted, down);
        if (hasKeyboardFocus(true))
        {
            graphics.setColour(juce::Colours::black);
            graphics.drawRoundedRectangle(
                getLocalBounds().toFloat().reduced(2.5f), 2.0f, 2.0f);
        }
    }
};

void styleControl(juce::ComboBox& control)
{
    control.setEditableText(false);
    control.setColour(juce::ComboBox::backgroundColourId, juce::Colour(StudioColours::raised));
    control.setColour(juce::ComboBox::textColourId, juce::Colour(StudioColours::text));
    control.setColour(juce::ComboBox::arrowColourId, juce::Colour(StudioColours::text));
    control.setColour(juce::ComboBox::outlineColourId, juce::Colour(StudioColours::border));
    control.setColour(juce::ComboBox::focusedOutlineColourId, juce::Colour(StudioColours::orange));
}

void styleControl(juce::TextEditor& control)
{
    control.setMultiLine(false);
    control.setSelectAllWhenFocused(true);
    control.setInputRestrictions(256);
    control.setFont(juce::FontOptions(14.0f));
    control.setColour(juce::TextEditor::backgroundColourId, juce::Colour(StudioColours::window));
    control.setColour(juce::TextEditor::textColourId, juce::Colour(StudioColours::text));
    control.setColour(juce::TextEditor::outlineColourId, juce::Colour(StudioColours::border));
    control.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(StudioColours::orange));
}

void styleControl(juce::ToggleButton&) {}

template <typename Control>
struct Field
{
    juce::Label label;
    Control control;

    void initialise(juce::Component& parent, const juce::String& id,
                    const juce::String& title, const juce::String& help)
    {
        label.setComponentID(id + ".label");
        label.setFont(juce::FontOptions(13.0f));
        label.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
        label.setBorderSize({});
        control.setComponentID(id);
        control.setName(title);
        control.setDescription(help);
        control.setTooltip(help);
        control.setWantsKeyboardFocus(true);
        setTitle(title);
        styleControl(control);
        parent.addAndMakeVisible(label);
        parent.addAndMakeVisible(control);
    }

    void setTitle(const juce::String& title)
    {
        label.setText(title, juce::dontSendNotification);
        control.setTitle(title);
    }

    void setVisible(bool visible)
    {
        label.setVisible(visible);
        control.setVisible(visible);
    }

    void setEnabled(bool enabled)
    {
        label.setEnabled(enabled);
        control.setEnabled(enabled);
    }
};

using ComboField = Field<juce::ComboBox>;
using NumberField = Field<juce::TextEditor>;
using ToggleField = Field<juce::ToggleButton>;

void initialiseLabel(juce::Component& parent, juce::Label& label,
                     const juce::String& id, const juce::String& text,
                     float size = 13.0f, bool heading = false)
{
    label.setComponentID(id);
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(size, heading ? juce::Font::bold : juce::Font::plain));
    label.setColour(juce::Label::textColourId,
                    juce::Colour(heading ? StudioColours::text : StudioColours::secondaryText));
    label.setBorderSize({});
    parent.addAndMakeVisible(label);
}

struct FormLayout
{
    int fullWidth;
    int x;
    int width;
    int y = 12;
    bool stacked;

    template <typename Control>
    void add(Field<Control>& field)
    {
        if (!field.control.isVisible())
            return;
        if (stacked)
        {
            field.label.setBounds(x, y, width, 18);
            field.control.setBounds(x, y + 20, width, 28);
            y += 57;
        }
        else
        {
            const auto labelWidth = juce::jmin(164, width / 3);
            field.label.setBounds(x, y, labelWidth, 28);
            field.control.setBounds(x + labelWidth + 12, y, width - labelWidth - 12, 28);
            y += 38;
        }
    }

    void add(juce::Label& label, int height)
    {
        if (label.isVisible())
        {
            label.setBounds(x, y, width, height);
            y += height + 12;
        }
    }
};

struct DialogFrame
{
    DialogFrame(juce::Component& component, const juce::String& prefix,
                const juce::String& name, const juce::String& description)
        : owner(component), tooltip(&component, 650)
    {
        owner.setLookAndFeel(&theme);
        owner.setName(name);
        owner.setTitle(name);
        owner.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
        initialiseLabel(owner, title, prefix + ".title", name, 21.0f, true);
        initialiseLabel(owner, subtitle, prefix + ".subtitle", description);
        content.setComponentID(prefix + ".content");
        content.setWantsKeyboardFocus(false);
        viewport.setComponentID(prefix + ".viewport");
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setScrollBarThickness(12);
        viewport.setWantsKeyboardFocus(false);
        owner.addAndMakeVisible(viewport);
        initialiseLabel(owner, status, prefix + ".error", {}, 12.0f);
        status.setTitle("Settings validation");
        cancel.setButtonText("Cancel");
        cancel.setComponentID(prefix + ".cancel");
        cancel.setTitle("Cancel settings");
        cancel.setTooltip("Close without applying. Escape also cancels.");
        apply.setButtonText("Apply");
        apply.setComponentID(prefix + ".apply");
        apply.setTitle("Apply settings");
        apply.setTooltip("Validate and apply these settings. Playback is not started.");
        apply.setColour(juce::TextButton::buttonColourId, juce::Colour(StudioColours::orange));
        apply.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
        owner.addAndMakeVisible(cancel);
        owner.addAndMakeVisible(apply);
    }

    ~DialogFrame() { viewport.setViewedComponent(nullptr, false); }

    void initialiseSummary(juce::Label& summary, const juce::String& id)
    {
        initialiseLabel(content, summary, id, {}, 13.0f, true);
        summary.setFont(juce::FontOptions(
            juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        summary.setColour(juce::Label::backgroundColourId, juce::Colour(StudioColours::window));
        summary.setColour(juce::Label::outlineColourId, juce::Colour(StudioColours::border));
        summary.setBorderSize(juce::BorderSize<int>(10, 12, 10, 12));
    }

    void paint(juce::Graphics& graphics) const
    {
        graphics.fillAll(juce::Colour(StudioColours::panel));
        graphics.setColour(juce::Colour(StudioColours::border));
        graphics.drawHorizontalLine(71, 0.0f, static_cast<float>(owner.getWidth()));
        graphics.drawHorizontalLine(
            owner.getHeight() - 108, 0.0f, static_cast<float>(owner.getWidth()));
    }

    FormLayout beginLayout()
    {
        auto bounds = owner.getLocalBounds();
        const auto margin = juce::jmin(20, bounds.getWidth() / 12);
        auto header = bounds.removeFromTop(72).reduced(margin, 8);
        title.setBounds(header.removeFromTop(28));
        subtitle.setBounds(header);
        auto footer = bounds.removeFromBottom(108).reduced(margin, 8);
        auto buttons = footer.removeFromBottom(32);
        apply.setBounds(buttons.removeFromRight(juce::jmin(120, buttons.getWidth() / 2)));
        buttons.removeFromRight(10);
        cancel.setBounds(buttons.removeFromRight(juce::jmin(92, buttons.getWidth())));
        footer.removeFromBottom(8);
        status.setBounds(footer);
        viewport.setBounds(bounds.reduced(8, 0));
        const auto width = juce::jmax(1, viewport.getWidth() - viewport.getScrollBarThickness());
        const auto inset = juce::jmin(12, width / 12);
        return { width, inset, juce::jmax(1, width - 2 * inset), 12, width < 450 };
    }

    void finishLayout(const FormLayout& layout)
    {
        content.setSize(layout.fullWidth, layout.y + 8);
    }

    void setError(const juce::String& error)
    {
        status.setText(error.isEmpty() ? "Apply saves these settings; playback is unchanged." : error,
                       juce::dontSendNotification);
        status.setDescription(error);
        status.setTooltip(error);
        status.setColour(juce::Label::textColourId,
                         juce::Colour(error.isEmpty() ? StudioColours::secondaryText : StudioColours::amber));
    }

    void reject(const juce::String& error)
    {
        attempted = true;
        setError(error);
        if (owner.getPeer() != nullptr && owner.isShowing())
            juce::AccessibilityHandler::postAnnouncement(
                error, juce::AccessibilityHandler::AnnouncementPriority::high);
    }

    void dismiss()
    {
        if (finished)
            return;
        finished = true;
        if (auto* window = owner.findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
        else
            owner.setVisible(false);
    }

    void scrollToFocus()
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        if (focused == nullptr || !content.isParentOf(focused))
            return;
        const auto padding = juce::BorderSize<int>(content.getWidth() < 450 ? 24 : 6, 0, 6, 0);
        const auto bounds = padding.addedTo(content.getLocalArea(focused, focused->getLocalBounds()));
        const auto top = viewport.getViewPositionY();
        if (bounds.getY() < top)
            viewport.setViewPosition(0, juce::jmax(0, bounds.getY()));
        else if (bounds.getBottom() > top + viewport.getViewHeight())
            viewport.setViewPosition(0, bounds.getBottom() - viewport.getViewHeight());
    }

    template <typename ComponentType, typename Settings>
    void complete(ComponentType& component, Settings value)
    {
        if (finished)
            return;
        finished = true;
        apply.setEnabled(false);
        auto completion = component.onApply;
        if (auto* window = component.template findParentComponentOfClass<juce::DialogWindow>())
        {
            // Post from modal completion so the owned dialog is gone before another chooser opens.
            juce::ModalComponentManager::getInstance()->attachCallback(
                window, juce::ModalCallbackFunction::create(
                    [modalCompletion = std::move(completion), value](int result) mutable
                    {
                        if (result == 1 && modalCompletion)
                            juce::MessageManager::callAsync(
                                [dispatchCompletion = std::move(modalCompletion), value]() mutable
                                {
                                    dispatchCompletion(value);
                                });
                    }));
            window->exitModalState(1);
            return;
        }
        const auto safe = juce::Component::SafePointer<ComponentType>(&component);
        juce::MessageManager::callAsync(
            [safe, localCompletion = std::move(completion), value]() mutable
            {
                if (safe != nullptr && localCompletion)
                    localCompletion(value);
            });
    }

    juce::Component& owner;
    StudioTheme theme;
    juce::Viewport viewport;
    juce::Component content;
    juce::Label title, subtitle, status;
    juce::TextButton cancel;
    ApplyButton apply;
    juce::TooltipWindow tooltip;
    bool attempted = false;
    bool finished = false;
};

template <typename ComponentType, typename Settings>
void launchDialog(std::unique_ptr<ComponentType> content, juce::Component& centreAround,
                  std::function<void(Settings)> handler, const juce::String& firstControl)
{
    const auto safeOwner = juce::Component::SafePointer<juce::Component>(&centreAround);
    content->onApply = [safeOwner, applyToOwner = std::move(handler)](Settings value) mutable
    {
        if (safeOwner != nullptr && applyToOwner)
            applyToOwner(std::move(value));
    };
    auto workArea = juce::Rectangle<int>(0, 0, 1280, 800);
    if (const auto* display = juce::Desktop::getInstance().getDisplays()
                                  .getDisplayForRect(centreAround.getScreenBounds()))
        workArea = display->userBounds.getLargestIntegerWithin();
    auto clientArea = workArea.reduced(16).withTrimmedTop(36);
    if (clientArea.isEmpty())
        clientArea = { workArea.getX(), workArea.getY(), 1, 1 };
    content->setSize(juce::jmin(content->getWidth(), clientArea.getWidth()),
                     juce::jmin(content->getHeight(), clientArea.getHeight()));
    const auto safeContent = juce::Component::SafePointer<ComponentType>(content.get());
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = content->getName();
    options.dialogBackgroundColour = juce::Colour(StudioColours::panel);
    options.content.setOwned(content.release());
    options.componentToCentreAround = &centreAround;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.useBottomRightCornerResizer = false;
    if (auto* window = options.launchAsync())
    {
        window->setResizeLimits(
            juce::jmin(400, clientArea.getWidth()), juce::jmin(350, clientArea.getHeight()),
            clientArea.getWidth(), clientArea.getHeight());
        window->setBounds(window->getBounds().constrainedWithin(clientArea));
        juce::MessageManager::callAsync([safeContent, firstControl]
        {
            if (safeContent == nullptr || !safeContent->isShowing())
                return;
            if (auto* viewport = safeContent->findChildWithID(firstControl.upToFirstOccurrenceOf(".", false, false)
                                                              + ".viewport"))
                if (auto* scroller = dynamic_cast<juce::Viewport*>(viewport))
                    if (auto* body = scroller->getViewedComponent())
                        if (auto* control = body->findChildWithID(firstControl))
                            control->grabKeyboardFocus();
        });
    }
}
}

struct LoopSettingsComponent::Impl
{
    struct Marker
    {
        juce::String id;
        juce::String label;
        double seconds = 0.0;
    };

    Impl(LoopSettingsComponent& component, Project snapshot, double rate,
         std::optional<juce::Range<double>> selected)
        : owner(component), project(std::move(snapshot)), sampleRate(rate), selectedRange(selected),
          frame(component, "loop", "Loop settings", "Choose fixed loop boundaries without starting playback.")
    {
        auto& body = frame.content;
        enabled.initialise(body, "loop.enabled", "Loop playback",
                           "Enable or disable looping without starting playback. The range remains editable.");
        enabled.control.setButtonText("Enabled");
        enabled.control.setToggleState(project.loopEnabled, juce::dontSendNotification);
        preset.initialise(body, "loop.preset", "Range preset",
                          "Replace the boundaries with the whole project or the selected clip. Loop enable is preserved.");
        preset.control.addItem("Whole project", 1);
        preset.control.addItem("Selected clip", 2);
        preset.control.setItemEnabled(2, selectedRange.has_value());
        preset.control.setTextWhenNothingSelected("Choose a range preset");
        mode.initialise(body, "loop.mode", "Position format",
                        "Convert valid boundaries between seconds and the current tempo/meter map. "
                        "Musical positions use 960 ticks per beat.");
        mode.control.addItem("Timeline seconds", 1);
        mode.control.addItem("Musical position", 2);
        mode.control.addItem("Use marker positions", 3);
        start.initialise(body, "loop.start", "Start",
                         "Enter seconds, or bar:beat:tick in musical mode. Bars and beats start at 1.");
        end.initialise(body, "loop.end", "End",
                       "Enter an end after the start. Musical positions accept bar, bar:beat or bar:beat:tick.");
        startMarker.initialise(body, "loop.startMarker", "Start marker",
                               "Choose the stable marker ID used to resolve the loop start.");
        endMarker.initialise(body, "loop.endMarker", "End marker",
                             "Choose a marker later than the start. Names may repeat; IDs do not.");
        initialiseLabel(body, hint, "loop.hint",
                        "Musical and marker positions are resolved now and applied as fixed timeline seconds. "
                        "Later map or marker edits do not move these loop boundaries.", 12.0f);
        frame.initialiseSummary(summary, "loop.summary");
        initialiseMarkers();
        if (!hasMarkerPair)
            hint.setText(
                hint.getText() + "\nMarker mode needs at least two distinct named markers.",
                juce::dontSendNotification);
        const LoopRangeSettings initial { project.loopEnabled, project.loopStartSeconds, project.loopEndSeconds };
        juce::String ignored;
        if (!setTextRange(initial, 2, true, ignored))
            setTextRange(initial, 1, false, ignored);
        connectCallbacks();
    }

    void initialiseMarkers()
    {
        std::set<juce::String> ids;
        for (const auto& section : project.sections)
        {
            const auto time = std::isfinite(section.timeSeconds)
                ? juce::String(section.timeSeconds, 3) + " s" : "invalid time";
            markers.push_back({
                section.id, (section.name.isEmpty() ? "Unnamed marker" : section.name) + " - " + time,
                section.timeSeconds
            });
            if (section.id.isNotEmpty())
                ids.insert(section.id);
        }
        std::stable_sort(markers.begin(), markers.end(), [](const auto& first, const auto& second)
        {
            if (std::isfinite(first.seconds) != std::isfinite(second.seconds))
                return std::isfinite(first.seconds);
            return std::isfinite(first.seconds) && first.seconds < second.seconds;
        });
        hasMarkerPair = ids.size() >= 2;
        mode.control.setItemEnabled(3, hasMarkerPair);
        for (size_t index = 0; index < markers.size(); ++index)
        {
            auto text = markers[index].label;
            if (std::count_if(markers.begin(), markers.end(), [&text](const auto& other)
                { return other.label == text; }) > 1)
                text += " [" + juce::String(static_cast<int>(index) + 1) + "]";
            startMarker.control.addItem(text, static_cast<int>(index) + 1);
            endMarker.control.addItem(text, static_cast<int>(index) + 1);
        }
        startMarker.control.setTextWhenNothingSelected("Choose a start marker");
        endMarker.control.setTextWhenNothingSelected("Choose an end marker");
    }

    juce::String markerId(const juce::ComboBox& selector) const
    {
        const auto index = selector.getSelectedId() - 1;
        return juce::isPositiveAndBelow(index, static_cast<int>(markers.size()))
            ? markers[static_cast<size_t>(index)].id : juce::String();
    }

    int markerAt(double seconds, const juce::String& previousId) const
    {
        auto match = 0;
        const auto tolerance = 0.5 / sampleRate;
        const auto preferred = std::find_if(markers.begin(), markers.end(), [&](const auto& marker)
        {
            return previousId.isNotEmpty() && marker.id == previousId
                && std::isfinite(marker.seconds) && std::abs(marker.seconds - seconds) <= tolerance;
        });
        if (preferred != markers.end())
            return static_cast<int>(std::distance(markers.begin(), preferred)) + 1;
        for (size_t index = 0; index < markers.size(); ++index)
        {
            if (markers[index].id.isEmpty() || !std::isfinite(markers[index].seconds)
                || std::abs(markers[index].seconds - seconds) > tolerance)
                continue;
            if (match != 0)
                return 0;
            match = static_cast<int>(index) + 1;
        }
        return match;
    }

    std::optional<LoopRangeSettings> readSettings(juce::String& error) const
    {
        error.clear();
        LoopRangeSettings value;
        value.enabled = enabled.control.getToggleState();
        if (activeMode == 1)
        {
            const auto first = readNumber(start.control, "Loop start", error);
            if (!first)
                return std::nullopt;
            const auto last = readNumber(end.control, "Loop end", error);
            if (!last)
                return std::nullopt;
            value.startSeconds = *first;
            value.endSeconds = *last;
        }
        else if (activeMode == 2)
        {
            const auto first = TransportEditing::secondsAtMusicalPosition(project, start.control.getText(), error);
            if (!first)
            {
                error = "Start position: " + error;
                return std::nullopt;
            }
            const auto last = TransportEditing::secondsAtMusicalPosition(project, end.control.getText(), error);
            if (!last)
            {
                error = "End position: " + error;
                return std::nullopt;
            }
            value.startSeconds = *first;
            value.endSeconds = *last;
        }
        else if (activeMode == 3)
        {
            if (!hasMarkerPair)
            {
                error = "Create at least two distinct named markers in Tracking Setup or the timeline marker lane.";
                return std::nullopt;
            }
            const auto range = TransportEditing::markerRange(
                project, markerId(startMarker.control), markerId(endMarker.control), error);
            if (!range)
                return std::nullopt;
            value.startSeconds = range->getStart();
            value.endSeconds = range->getEnd();
        }
        else
        {
            error = "Choose a loop position format.";
            return std::nullopt;
        }
        const auto validation = TransportEditing::validateLoopRange(value, sampleRate);
        if (validation.failed())
        {
            error = validation.getErrorMessage();
            return std::nullopt;
        }
        return value;
    }

    bool setTextRange(const LoopRangeSettings& value, int requestedMode, bool exact, juce::String& error)
    {
        auto startText = numberText(value.startSeconds);
        auto endText = numberText(value.endSeconds);
        if (requestedMode == 2)
        {
            if (TransportEditing::validateLoopRange(value, sampleRate).failed())
                return false;
            startText = TransportEditing::musicalPositionText(project, value.startSeconds);
            endText = TransportEditing::musicalPositionText(project, value.endSeconds);
            const auto first = TransportEditing::secondsAtMusicalPosition(project, startText, error);
            const auto last = TransportEditing::secondsAtMusicalPosition(project, endText, error);
            if (!first || !last
                || TransportEditing::validateLoopRange({ value.enabled, *first, *last }, sampleRate).failed()
                || (exact && (std::llround(*first * sampleRate) != std::llround(value.startSeconds * sampleRate)
                              || std::llround(*last * sampleRate) != std::llround(value.endSeconds * sampleRate))))
            {
                error = "These boundaries cannot be represented safely as musical positions. Use timeline seconds.";
                return false;
            }
        }
        activeMode = requestedMode;
        mode.control.setSelectedId(activeMode, juce::dontSendNotification);
        start.control.setText(startText, false);
        end.control.setText(endText, false);
        pendingMarkerRange.reset();
        return true;
    }

    void modeChanged()
    {
        const auto requested = mode.control.getSelectedId();
        if (requested == activeMode)
            return;
        juce::String error;
        auto current = readSettings(error);
        if (!current && activeMode == 3 && pendingMarkerRange && !markersEdited)
            current = pendingMarkerRange;
        if (!current || requested < 1 || requested > 3 || (requested == 3 && !hasMarkerPair))
        {
            mode.control.setSelectedId(activeMode, juce::dontSendNotification);
            actionError = !current ? error
                : requested == 3 ? "Marker mode needs at least two distinct named markers."
                                 : "Choose a loop position format.";
            refresh();
            return;
        }
        actionError.clear();
        if (requested == 3)
        {
            const auto previousStart = markerId(startMarker.control);
            const auto previousEnd = markerId(endMarker.control);
            startMarker.control.setSelectedId(markerAt(current->startSeconds, previousStart), juce::dontSendNotification);
            endMarker.control.setSelectedId(markerAt(current->endSeconds, previousEnd), juce::dontSendNotification);
            pendingMarkerRange = current;
            markersEdited = false;
            activeMode = 3;
        }
        else if (!setTextRange(*current, requested, true, error))
        {
            mode.control.setSelectedId(activeMode, juce::dontSendNotification);
            actionError = error;
        }
        preset.control.setSelectedId(0, juce::dontSendNotification);
        refresh();
    }

    void applyPreset()
    {
        const auto choice = preset.control.getSelectedId();
        if (choice == 0)
            return;
        preset.control.setSelectedId(0, juce::dontSendNotification);
        LoopRangeSettings value { enabled.control.getToggleState(), 0.0, project.lengthSeconds() };
        if (choice == 2)
        {
            if (!selectedRange)
            {
                actionError = "Select a clip before using the selected-clip preset.";
                refresh();
                return;
            }
            value.startSeconds = selectedRange->getStart();
            value.endSeconds = selectedRange->getEnd();
        }
        else if (choice != 1)
        {
            actionError = "Choose the whole-project or selected-clip preset.";
            refresh();
            return;
        }
        const auto valid = TransportEditing::validateLoopRange(value, sampleRate);
        if (valid.failed())
        {
            actionError = valid.getErrorMessage();
            refresh();
            return;
        }
        juce::String error;
        if (activeMode == 1 || !setTextRange(value, 2, true, error))
            setTextRange(value, 1, false, error);
        actionError.clear();
        refresh();
    }

    void changed(bool markerEdit = false)
    {
        if (markerEdit)
            markersEdited = true;
        actionError.clear();
        preset.control.setSelectedId(0, juce::dontSendNotification);
        refresh();
    }

    void connectCallbacks()
    {
        const auto safe = juce::Component::SafePointer<LoopSettingsComponent>(&owner);
        mode.control.onChange = [safe] { if (safe != nullptr) safe->impl->modeChanged(); };
        preset.control.onChange = [safe] { if (safe != nullptr) safe->impl->applyPreset(); };
        enabled.control.onClick = [safe] { if (safe != nullptr) safe->impl->changed(); };
        for (auto* field : { &startMarker, &endMarker })
            field->control.onChange = [safe] { if (safe != nullptr) safe->impl->changed(true); };
        for (auto* field : { &start, &end })
        {
            field->control.onTextChange = [safe] { if (safe != nullptr) safe->impl->changed(); };
            field->control.onReturnKey = [safe] { if (safe != nullptr) safe->impl->submit(); };
            field->control.onEscapeKey = [safe] { if (safe != nullptr) safe->impl->frame.dismiss(); };
        }
        frame.apply.onClick = [safe] { if (safe != nullptr) safe->impl->submit(); };
        frame.cancel.onClick = [safe] { if (safe != nullptr) safe->impl->frame.dismiss(); };
    }

    void refresh()
    {
        start.setVisible(activeMode != 3);
        end.setVisible(activeMode != 3);
        startMarker.setVisible(activeMode == 3);
        endMarker.setVisible(activeMode == 3);
        start.setTitle(activeMode == 2 ? "Start (bar:beat:tick)" : "Start (seconds)");
        end.setTitle(activeMode == 2 ? "End (bar:beat:tick)" : "End (seconds)");
        juce::String error;
        const auto value = readSettings(error);
        if (value)
        {
            summary.setText(
                "Start " + numberText(value->startSeconds, 6) + " s  /  "
                    + TransportEditing::musicalPositionText(project, value->startSeconds) + "\n"
                    + "End   " + numberText(value->endSeconds, 6) + " s  /  "
                    + TransportEditing::musicalPositionText(project, value->endSeconds) + "\n"
                    + "Duration " + numberText(value->endSeconds - value->startSeconds, 6) + " s",
                juce::dontSendNotification);
        }
        else
        {
            auto message = error;
            if (activeMode == 3 && pendingMarkerRange && !markersEdited)
                message += "\nPrevious range " + numberText(pendingMarkerRange->startSeconds, 6)
                    + " to " + numberText(pendingMarkerRange->endSeconds, 6)
                    + " s is preserved until you choose markers.";
            summary.setText(message, juce::dontSendNotification);
        }
        frame.setError(actionError.isNotEmpty() ? actionError : frame.attempted ? error : juce::String());
        layout();
    }

    void layout()
    {
        auto rows = frame.beginLayout();
        rows.add(enabled);
        rows.add(preset);
        rows.add(mode);
        rows.add(start);
        rows.add(end);
        rows.add(startMarker);
        rows.add(endMarker);
        rows.add(hint, rows.stacked ? 86 : 50);
        rows.add(summary, rows.stacked ? 142 : 98);
        frame.finishLayout(rows);
    }

    void submit()
    {
        if (frame.finished)
            return;
        juce::String error;
        const auto value = readSettings(error);
        if (!value)
        {
            frame.reject(error);
            return;
        }
        frame.complete(owner, *value);
    }

    LoopSettingsComponent& owner;
    const Project project;
    const double sampleRate;
    const std::optional<juce::Range<double>> selectedRange;
    DialogFrame frame;
    ToggleField enabled;
    ComboField preset, mode, startMarker, endMarker;
    NumberField start, end;
    juce::Label hint, summary;
    std::vector<Marker> markers;
    std::optional<LoopRangeSettings> pendingMarkerRange;
    juce::String actionError;
    int activeMode = 1;
    bool hasMarkerPair = false;
    bool markersEdited = false;
};

struct SectionSettingsComponent::Impl
{
    struct ClickDraft
    {
        bool enabled = true;
        int subdivision = 1;
        juce::String level, accentLevel, accents;
    };

    Impl(SectionSettingsComponent& component, Project snapshot, juce::String id)
        : owner(component), project(std::move(snapshot)), sectionId(std::move(id)),
          frame(component, "section", "Section settings", "Tempo, time signature and click overrides at this marker.")
    {
        readInitialValues();
        auto& body = frame.content;
        initialiseLabel(body, transportHeading, "section.transportHeading", "TEMPO AND TIME SIGNATURE", 12.0f, true);
        overrideTempo.initialise(body, "section.overrideTempo", "Tempo override",
                                 "Unchecked inherits the tempo map with this marker's owned tempo point removed.");
        overrideTempo.control.setButtonText("Set tempo at this marker");
        bpm.initialise(body, "section.bpm", "Tempo (BPM)", "A finite tempo from 20 to 400 BPM.");
        ramp.initialise(body, "section.ramp", "Tempo transition",
                        "Ramp from the previous tempo point to this marker's tempo.");
        ramp.control.setButtonText("Ramp from previous point");
        overrideMeter.initialise(body, "section.overrideMeter", "Meter override",
                                 "Unchecked inherits the preceding manual or section-owned time signature.");
        overrideMeter.control.setButtonText("Set time signature");
        numerator.initialise(body, "section.numerator", "Beats per bar",
                             "A whole-number numerator from 1 to 32. Decimal values are not accepted.");
        denominator.initialise(body, "section.denominator", "Beat unit",
                               "Time-signature denominator: 1, 2, 4, 8, 16 or 32.");
        for (const auto value : { 1, 2, 4, 8, 16, 32 })
            denominator.control.addItem(juce::String(value), value);
        initialiseLabel(body, clickHeading, "section.clickHeading", "SECTION CLICK", 12.0f, true);
        overrideClick.initialise(body, "section.overrideClick", "Click override",
                                 "Unchecked inherits the last earlier explicit section click pattern, or project defaults.");
        overrideClick.control.setButtonText("Set a section click pattern");
        clickEnabled.initialise(body, "section.clickEnabled", "Section click",
                                "Mute this section pattern without disabling editing. The global CLICK switch remains the master gate.");
        clickEnabled.control.setButtonText("Enabled");
        subdivision.initialise(body, "section.subdivision", "Subdivisions per beat",
                               "Choose any whole subdivision from 1 to 8, including 5 and 7.");
        for (int value = 1; value <= 8; ++value)
            subdivision.control.addItem(juce::String(value), value);
        normalLevel.initialise(body, "section.level", "Normal level (%)",
                               "Normal click level from 0 to 100 percent, editable even when the section click is muted.");
        accentLevel.initialise(body, "section.accentLevel", "Accent level (%)",
                               "Accented click level from 0 to 100 percent.");
        accents.initialise(body, "section.accentBeats", "Accent beats",
                           "Comma-separated unique whole beat numbers 1..32, such as 1, 4 for 6/8. Empty means no accents.");
        initialiseLabel(body, clickHint, "section.clickHint",
                        project.metronomeEnabled
                            ? "Accent beats use 1..32, including beats used by later meter changes. "
                              "An empty list means no accents. Generic markers do not reset this pattern."
                            : "Global CLICK is off. These local settings are saved but stay silent until global CLICK is enabled. "
                              "Accent beats use 1..32; an empty list means no accents.",
                        12.0f);
        frame.initialiseSummary(summary, "section.summary");

        tempoOn = ownSettings.tempoBpm.has_value();
        meterOn = ownSettings.timeSignature.has_value();
        clickOn = ownSettings.clickSettings.has_value();
        overrideTempo.control.setToggleState(tempoOn, juce::dontSendNotification);
        overrideMeter.control.setToggleState(meterOn, juce::dontSendNotification);
        overrideClick.control.setToggleState(clickOn, juce::dontSendNotification);
        tempoDraft = numberText(ownSettings.tempoBpm.value_or(inheritedTempo));
        rampDraft = ownSettings.rampFromPrevious;
        const auto meter = ownSettings.timeSignature.value_or(inheritedMeter);
        numeratorDraft = juce::String(meter.numerator);
        denominatorDraft = meter.denominator;
        clickDraft = makeClickDraft(ownSettings.clickSettings.value_or(inheritedClick));
        displayOverrides();
        connectCallbacks();
    }

    static ClickDraft makeClickDraft(const SectionClickSettings& click)
    {
        return { click.enabled, click.subdivision, numberText(click.level * 100.0f, 4),
                 numberText(click.accentLevel * 100.0f, 4), accentText(click.accentBeats) };
    }

    void readInitialValues()
    {
        const auto* section = project.findSection(sectionId);
        if (section == nullptr)
        {
            initialError = "The selected marker is missing. Close this dialog and choose an existing marker.";
            frame.subtitle.setText("Selected marker unavailable", juce::dontSendNotification);
            return;
        }
        const auto description = section->name + " - " + juce::String(section->timeSeconds, 3) + " s"
            + "  (" + TransportEditing::musicalPositionText(project, section->timeSeconds) + ")";
        frame.subtitle.setText(description, juce::dontSendNotification);
        frame.subtitle.setTooltip(description);
        frame.subtitle.setTitle(description);
        const auto owned = project.sectionTransportSettings(sectionId, initialError);
        if (!owned)
        {
            if (initialError.isEmpty())
                initialError = "The selected section's transport settings could not be read.";
            return;
        }
        ownSettings = *owned;

        // This one-time copy removes only this section's owned settings from the preview.
        auto inheritedProject = project;
        SetSectionTransportCommand clearOverrides(sectionId, {});
        if (!clearOverrides.perform(inheritedProject, initialError))
        {
            if (initialError.isEmpty())
                initialError = "Inherited settings could not be resolved for this section.";
            return;
        }
        inheritedTempo = inheritedProject.tempoAt(section->timeSeconds);
        const auto meter = inheritedProject.meterAt(section->timeSeconds);
        inheritedMeter = { meter.numerator, meter.denominator };
        inheritedClick = inheritedProject.clickSettingsAt(section->timeSeconds);
    }

    void displayOverrides()
    {
        const juce::ScopedValueSetter<bool> guard(updating, true);
        bpm.control.setText(tempoOn ? tempoDraft : numberText(inheritedTempo), false);
        ramp.control.setToggleState(tempoOn && rampDraft, juce::dontSendNotification);
        numerator.control.setText(meterOn ? numeratorDraft : juce::String(inheritedMeter.numerator), false);
        denominator.control.setSelectedId(meterOn ? denominatorDraft : inheritedMeter.denominator,
                                          juce::dontSendNotification);
        const auto click = clickOn ? clickDraft : makeClickDraft(inheritedClick);
        clickEnabled.control.setToggleState(click.enabled, juce::dontSendNotification);
        subdivision.control.setSelectedId(click.subdivision, juce::dontSendNotification);
        normalLevel.control.setText(click.level, false);
        accentLevel.control.setText(click.accentLevel, false);
        accents.control.setText(click.accents, false);
    }

    void overridesChanged()
    {
        if (updating)
            return;
        if (tempoOn)
        {
            tempoDraft = bpm.control.getText();
            rampDraft = ramp.control.getToggleState();
        }
        if (meterOn)
        {
            numeratorDraft = numerator.control.getText();
            denominatorDraft = denominator.control.getSelectedId();
        }
        if (clickOn)
            clickDraft = { clickEnabled.control.getToggleState(), subdivision.control.getSelectedId(),
                           normalLevel.control.getText(), accentLevel.control.getText(), accents.control.getText() };
        tempoOn = overrideTempo.control.getToggleState();
        meterOn = overrideMeter.control.getToggleState();
        clickOn = overrideClick.control.getToggleState();
        displayOverrides();
        refresh();
    }

    std::optional<SectionTransportSettings> readSettings(juce::String& error, bool validateCommand) const
    {
        error.clear();
        if (initialError.isNotEmpty())
        {
            error = initialError;
            return std::nullopt;
        }
        if (project.findSection(sectionId) == nullptr)
        {
            error = "The selected marker is missing.";
            return std::nullopt;
        }
        SectionTransportSettings value;
        if (overrideTempo.control.getToggleState())
        {
            value.tempoBpm = readBoundedNumber(bpm.control, 20.0, 400.0, "Tempo (BPM)", error);
            if (!value.tempoBpm)
                return std::nullopt;
            value.rampFromPrevious = ramp.control.getToggleState();
        }
        if (overrideMeter.control.getToggleState())
        {
            const auto beats = readInteger(numerator.control.getText(), 1, 32, "Beats per bar", error);
            if (!beats)
                return std::nullopt;
            const auto unit = denominator.control.getSelectedId();
            constexpr std::array units { 1, 2, 4, 8, 16, 32 };
            if (std::find(units.begin(), units.end(), unit) == units.end())
            {
                error = "Choose a beat unit of 1, 2, 4, 8, 16 or 32.";
                return std::nullopt;
            }
            value.timeSignature = SectionTimeSignature { *beats, unit };
        }
        if (overrideClick.control.getToggleState())
        {
            SectionClickSettings click;
            click.enabled = clickEnabled.control.getToggleState();
            click.subdivision = subdivision.control.getSelectedId();
            const auto level = readBoundedNumber(normalLevel.control, 0.0, 100.0, "Normal level (%)", error);
            if (!level)
                return std::nullopt;
            const auto accent = readBoundedNumber(accentLevel.control, 0.0, 100.0, "Accent level (%)", error);
            if (!accent)
                return std::nullopt;
            const auto beats = readAccents(accents.control.getText(), error);
            if (!beats)
                return std::nullopt;
            click.level = static_cast<float>(*level / 100.0);
            click.accentLevel = static_cast<float>(*accent / 100.0);
            click.accentBeats = *beats;
            if (!click.validate(error))
                return std::nullopt;
            value.clickSettings = std::move(click);
        }
        if (validateCommand)
        {
            auto validationProject = project;
            SetSectionTransportCommand command(sectionId, value);
            if (!command.perform(validationProject, error))
                return std::nullopt;
        }
        return value;
    }

    void connectCallbacks()
    {
        const auto safe = juce::Component::SafePointer<SectionSettingsComponent>(&owner);
        const auto change = [safe]
        {
            if (safe != nullptr && !safe->impl->updating)
                safe->impl->refresh();
        };
        for (auto* field : { &overrideTempo, &overrideMeter, &overrideClick })
            field->control.onClick = [safe] { if (safe != nullptr) safe->impl->overridesChanged(); };
        ramp.control.onClick = change;
        clickEnabled.control.onClick = change;
        denominator.control.onChange = change;
        subdivision.control.onChange = change;
        for (auto* field : { &bpm, &numerator, &normalLevel, &accentLevel, &accents })
        {
            field->control.onTextChange = change;
            field->control.onReturnKey = [safe] { if (safe != nullptr) safe->impl->submit(); };
            field->control.onEscapeKey = [safe] { if (safe != nullptr) safe->impl->frame.dismiss(); };
        }
        frame.apply.onClick = [safe] { if (safe != nullptr) safe->impl->submit(); };
        frame.cancel.onClick = [safe] { if (safe != nullptr) safe->impl->frame.dismiss(); };
    }

    void refresh()
    {
        const auto available = initialError.isEmpty();
        overrideTempo.setEnabled(available);
        overrideMeter.setEnabled(available);
        overrideClick.setEnabled(available);
        bpm.setEnabled(available && tempoOn);
        ramp.setEnabled(available && tempoOn);
        numerator.setEnabled(available && meterOn);
        denominator.setEnabled(available && meterOn);
        clickEnabled.setEnabled(available && clickOn);
        subdivision.setEnabled(available && clickOn);
        normalLevel.setEnabled(available && clickOn);
        accentLevel.setEnabled(available && clickOn);
        accents.setEnabled(available && clickOn);

        juce::String error;
        const auto value = readSettings(error, false);
        if (value)
        {
            const auto tempo = value->tempoBpm.value_or(inheritedTempo);
            const auto meter = value->timeSignature.value_or(inheritedMeter);
            const auto click = value->clickSettings.value_or(inheritedClick);
            auto text = "Tempo " + numberText(tempo, 3) + " BPM"
                + (value->tempoBpm ? value->rampFromPrevious ? " (ramp)" : " (override)" : " (inherited)")
                + "\nMeter " + juce::String(meter.numerator) + "/" + juce::String(meter.denominator)
                + (value->timeSignature ? " (override)" : " (inherited)")
                + "\nClick " + (click.enabled ? juce::String("on") : juce::String("off"))
                + ", " + juce::String(click.subdivision) + " per beat"
                + (value->clickSettings ? " (override)" : " (inherited)")
                + "\nAccents " + (click.accentBeats.empty() ? juce::String("none") : accentText(click.accentBeats));
            if (!project.metronomeEnabled)
                text += " / global CLICK off";
            summary.setText(text, juce::dontSendNotification);
        }
        else
        {
            summary.setText(error, juce::dontSendNotification);
        }
        frame.setError(initialError.isNotEmpty() ? initialError : frame.attempted ? error : juce::String());
        layout();
    }

    void layout()
    {
        auto rows = frame.beginLayout();
        rows.add(transportHeading, 22);
        rows.add(overrideTempo);
        rows.add(bpm);
        rows.add(ramp);
        rows.add(overrideMeter);
        rows.add(numerator);
        rows.add(denominator);
        rows.add(clickHeading, 22);
        rows.add(overrideClick);
        rows.add(clickEnabled);
        rows.add(subdivision);
        rows.add(normalLevel);
        rows.add(accentLevel);
        rows.add(accents);
        rows.add(clickHint, rows.stacked ? 92 : 58);
        rows.add(summary, rows.stacked ? 142 : 110);
        frame.finishLayout(rows);
    }

    void submit()
    {
        if (frame.finished)
            return;
        juce::String error;
        const auto value = readSettings(error, true);
        if (!value)
        {
            frame.reject(error);
            return;
        }
        frame.complete(owner, *value);
    }

    SectionSettingsComponent& owner;
    const Project project;
    const juce::String sectionId;
    DialogFrame frame;
    ToggleField overrideTempo, ramp, overrideMeter, overrideClick, clickEnabled;
    NumberField bpm, numerator, normalLevel, accentLevel, accents;
    ComboField denominator, subdivision;
    juce::Label transportHeading, clickHeading, clickHint, summary;
    SectionTransportSettings ownSettings;
    SectionTimeSignature inheritedMeter;
    SectionClickSettings inheritedClick;
    double inheritedTempo = 120.0;
    juce::String initialError, tempoDraft, numeratorDraft;
    int denominatorDraft = 4;
    ClickDraft clickDraft;
    bool rampDraft = false;
    bool tempoOn = false;
    bool meterOn = false;
    bool clickOn = false;
    bool updating = false;
};

LoopSettingsComponent::LoopSettingsComponent(
    Project project, double sampleRate, std::optional<juce::Range<double>> selectedRange)
{
    impl = std::make_unique<Impl>(*this, std::move(project), sampleRate, selectedRange);
    setSize(630, 620);
    impl->refresh();
}

LoopSettingsComponent::~LoopSettingsComponent() { setLookAndFeel(nullptr); }

std::optional<LoopRangeSettings> LoopSettingsComponent::settings(juce::String& error) const
{
    return impl->readSettings(error);
}

void LoopSettingsComponent::show(
    const Project& project, double sampleRate, std::optional<juce::Range<double>> selectedRange,
    juce::Component& centreAround, std::function<void(LoopRangeSettings)> handler)
{
    launchDialog(std::make_unique<LoopSettingsComponent>(project, sampleRate, selectedRange),
                 centreAround, std::move(handler), "loop.enabled");
}

void LoopSettingsComponent::paint(juce::Graphics& graphics) { impl->frame.paint(graphics); }
void LoopSettingsComponent::resized() { if (impl) impl->layout(); }

bool LoopSettingsComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
        impl->frame.dismiss();
    else if (key == juce::KeyPress::returnKey)
        impl->submit();
    else
        return false;
    return true;
}

void LoopSettingsComponent::focusOfChildComponentChanged(FocusChangeType)
{
    if (impl)
        impl->frame.scrollToFocus();
}

SectionSettingsComponent::SectionSettingsComponent(Project project, juce::String sectionId)
{
    impl = std::make_unique<Impl>(*this, std::move(project), std::move(sectionId));
    setSize(630, 740);
    impl->refresh();
}

SectionSettingsComponent::~SectionSettingsComponent() { setLookAndFeel(nullptr); }

std::optional<SectionTransportSettings> SectionSettingsComponent::settings(juce::String& error) const
{
    return impl->readSettings(error, true);
}

void SectionSettingsComponent::show(
    const Project& project, const juce::String& sectionId, juce::Component& centreAround,
    std::function<void(SectionTransportSettings)> handler)
{
    launchDialog(std::make_unique<SectionSettingsComponent>(project, sectionId),
                 centreAround, std::move(handler), "section.overrideTempo");
}

void SectionSettingsComponent::paint(juce::Graphics& graphics) { impl->frame.paint(graphics); }
void SectionSettingsComponent::resized() { if (impl) impl->layout(); }

bool SectionSettingsComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
        impl->frame.dismiss();
    else if (key == juce::KeyPress::returnKey)
        impl->submit();
    else
        return false;
    return true;
}

void SectionSettingsComponent::focusOfChildComponentChanged(FocusChangeType)
{
    if (impl)
        impl->frame.scrollToFocus();
}
}

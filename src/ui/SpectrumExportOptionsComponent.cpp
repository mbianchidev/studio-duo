#include "SpectrumExportOptionsComponent.h"

#include "StudioTheme.h"
#include "util/NumberParsing.h"

#include <array>
#include <cmath>

namespace studio
{
namespace
{
constexpr std::array<std::uint32_t, 4> barColours {
    0xffffffff, 0xff68d8e6, 0xffffc66d, 0xffd991df
};
constexpr std::array<int, 3> canvasWidths { 640, 1280, 1920 };

class SpectrumPreview final : public juce::Component
{
public:
    juce::Image image;
    void paint(juce::Graphics& graphics) override
    {
        for (int y = 0; y < getHeight(); y += 12)
            for (int x = 0; x < getWidth(); x += 12)
            {
                graphics.setColour(juce::Colour((x / 12 + y / 12) % 2 == 0 ? 0xff30353b : 0xff25292e));
                graphics.fillRect(x, y, 12, 12);
            }
        graphics.drawImageWithin(image, 0, 0, getWidth(), getHeight(),
                                 juce::RectanglePlacement::centred);
    }
};
}

struct SpectrumExportOptionsComponent::Impl
{
    template <typename Control>
    struct Field
    {
        juce::Label label;
        Control control;

        void initialise(juce::Component& parent, const char* id,
                        const char* fieldTitle, const char* description)
        {
            label.setText(fieldTitle, juce::dontSendNotification);
            label.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
            label.setFont(juce::FontOptions(13.0f));
            label.setBorderSize({});
            control.setComponentID(id);
            control.setName(fieldTitle);
            control.setTitle(fieldTitle);
            control.setDescription(description);
            control.setTooltip(description);
            control.setWantsKeyboardFocus(true);
            parent.addAndMakeVisible(label);
            parent.addAndMakeVisible(control);
        }

        void layout(int width, int& y)
        {
            if (!control.isVisible())
                return;
            const auto stacked = width < 440;
            label.setBounds(12, y, stacked ? width - 24 : 140, stacked ? 18 : 28);
            control.setBounds(stacked ? 12 : 164, y + (stacked ? 20 : 0),
                              width - (stacked ? 24 : 176), 28);
            y += stacked ? 57 : 37;
        }
    };

    Impl(SpectrumExportOptionsComponent& parent, Project context, SpectrumExportSettings initial)
        : owner(parent), project(std::move(context)), original(std::move(initial)), tooltips(&owner, 650)
    {
        owner.setLookAndFeel(&theme);
        owner.setName("Spectrum GIF export options");
        owner.setTitle(owner.getName());
        owner.setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
        title.setText("Export spectrum GIF", juce::dontSendNotification);
        title.setFont(juce::FontOptions(21.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colour(StudioColours::text));
        subtitle.setText("Frequency bars for video overlays. The GIF contains no audio.", juce::dontSendNotification);
        subtitle.setFont(juce::FontOptions(13.0f));
        subtitle.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
        owner.addAndMakeVisible(title);
        owner.addAndMakeVisible(subtitle);
        content.setComponentID("spectrum.content");
        viewport.setComponentID("spectrum.viewport");
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setWantsKeyboardFocus(false);
        owner.addAndMakeVisible(viewport);
        source.initialise(content, "spectrum.source", "Audio source",
                          "Choose the whole mix or a single active track, including its inserts and mixer gain.");
        range.initialise(content, "spectrum.range", "Export range",
                         "Choose the whole song, loop boundaries, or exact start/end positions.");
        start.initialise(content, "spectrum.start", "Start (seconds)", "The first included audio position.");
        end.initialise(content, "spectrum.end", "End (seconds)", "The excluded end position, after the start.");
        canvas.initialise(content, "spectrum.canvas", "Canvas size", "Smaller canvases reduce encoding time and file size.");
        frameRate.initialise(content, "spectrum.frameRate", "Frame rate", "10, 20, 25 or 30 frames per second.");
        colour.initialise(content, "spectrum.colour", "Bar color", "The spectrum color in the exported video overlay.");
        background.initialise(content, "spectrum.background", "Background", "Transparency for overlays, or a solid dark background.");
        source.control.addItem("Whole song mix", 1);
        trackIds.push_back({});
        for (std::size_t index = 0; index < project.tracks.size(); ++index)
        {
            const auto& track = project.tracks[index];
            if (track.parentTrackId.isNotEmpty() || !project.isTrackActive(track.id)
                || (track.type != TrackType::audio && track.type != TrackType::instrument
                    && track.type != TrackType::bus && track.type != TrackType::aux))
                continue;
            trackIds.push_back(track.id);
            source.control.addItem(juce::String(static_cast<int>(index + 1)) + ": " + track.name,
                                   static_cast<int>(trackIds.size()));
        }
        int sourceChoice = original.trackId.isEmpty() ? 1 : 0;
        for (std::size_t index = 1; index < trackIds.size(); ++index)
            if (trackIds[index] == original.trackId)
                sourceChoice = static_cast<int>(index + 1);
        source.control.setSelectedId(sourceChoice, juce::dontSendNotification);
        range.control.addItem("Whole song", 1);
        range.control.addItem("Loop boundaries", 2);
        range.control.addItem("Custom seconds", 4);
        range.control.setSelectedId(static_cast<int>(original.render.range) + 1, juce::dontSendNotification);
        start.control.setSelectAllWhenFocused(true);
        end.control.setSelectAllWhenFocused(true);
        start.control.setInputRestrictions(64);
        end.control.setInputRestrictions(64);
        start.control.setText(formatRoundTripNumber(original.render.startSeconds), false);
        end.control.setText(formatRoundTripNumber(original.render.endSeconds), false);
        for (std::size_t index = 0; index < canvasWidths.size(); ++index)
        {
            const auto width = canvasWidths[index];
            canvas.control.addItem(juce::String(width) + " x " + juce::String(width * 9 / 16),
                                   static_cast<int>(index + 1));
            if (original.gif.width == width && original.gif.height == width * 9 / 16)
                canvas.control.setSelectedId(static_cast<int>(index + 1), juce::dontSendNotification);
        }
        for (const auto rate : { 10, 20, 25, 30 })
            frameRate.control.addItem(juce::String(rate) + " fps", rate);
        frameRate.control.setSelectedId(original.gif.framesPerSecond, juce::dontSendNotification);
        int colourChoice = 0;
        for (std::size_t index = 0; index < barColours.size(); ++index)
            if (original.gif.barColour == juce::Colour(barColours[index]))
                colourChoice = static_cast<int>(index + 1);
        if (colourChoice == 0)
            if (const auto* track = project.findTrack(original.trackId);
                track != nullptr && track->colour == original.gif.barColour)
                colourChoice = 5;
        colour.control.addItem("White", 1);
        colour.control.addItem("Cyan", 2);
        colour.control.addItem("Amber", 3);
        colour.control.addItem("Violet", 4);
        colour.control.addItem("Track color", 5);
        colour.control.setSelectedId(colourChoice, juce::dontSendNotification);
        background.control.addItem("Transparent overlay", 1);
        background.control.addItem("Solid dark", 2);
        background.control.setSelectedId(original.gif.transparentBackground ? 1 : 2, juce::dontSendNotification);
        for (auto* combo : { &source.control, &range.control, &canvas.control, &frameRate.control,
                             &colour.control, &background.control })
            combo->onChange = [this] { refresh(); };
        for (auto* editor : { &start.control, &end.control })
        {
            editor->onTextChange = [this] { refresh(); };
            editor->onReturnKey = [this] { submit(); };
            editor->onEscapeKey = [this] { dismiss(); };
        }
        previewLabel.setText("STYLE PREVIEW (ILLUSTRATION)", juce::dontSendNotification);
        previewLabel.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        previewLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
        content.addAndMakeVisible(previewLabel);
        preview.setName("Spectrum style preview");
        preview.setDescription("Illustration of the exported bars, not analysis of the current audio.");
        content.addAndMakeVisible(preview);
        status.setComponentID("spectrum.status");
        status.setName("Spectrum export validation");
        status.setFont(juce::FontOptions(13.0f));
        owner.addAndMakeVisible(status);
        exportButton.setComponentID("spectrum.export");
        exportButton.setTitle("Export spectrum GIF");
        exportButton.setWantsKeyboardFocus(true);
        cancel.setTitle("Cancel spectrum export");
        cancel.setWantsKeyboardFocus(true);
        exportButton.onClick = [this] { submit(); };
        cancel.onClick = [this] { dismiss(); };
        owner.addAndMakeVisible(cancel);
        owner.addAndMakeVisible(exportButton);
        refresh();
    }

    std::optional<SpectrumExportSettings> readSettings(juce::String& error) const
    {
        error.clear();
        const auto selectedSource = source.control.getSelectedId();
        const auto selectedCanvas = canvas.control.getSelectedId();
        const auto selectedRange = range.control.getSelectedId();
        const auto selectedColour = colour.control.getSelectedId();
        if (selectedSource < 1 || selectedSource > static_cast<int>(trackIds.size())
            || selectedCanvas < 1 || selectedCanvas > static_cast<int>(canvasWidths.size())
            || (selectedRange != 1 && selectedRange != 2 && selectedRange != 4)
            || selectedColour < 1 || selectedColour > 5
            || background.control.getSelectedId() < 1 || background.control.getSelectedId() > 2)
        {
            error = "Choose an available audio source, range, canvas, color and background.";
            return std::nullopt;
        }
        auto result = original;
        result.trackId = trackIds[static_cast<std::size_t>(selectedSource - 1)];
        result.render.audio = {};
        result.render.tailSeconds = 0.0;
        result.render.fadeInSeconds = result.render.fadeOutSeconds = 0.0;
        result.render.range = static_cast<MixExportRange>(selectedRange - 1);
        if (selectedRange == 4)
        {
            const auto first = parseFiniteNumber(start.control.getText());
            const auto last = parseFiniteNumber(end.control.getText());
            if (!first || !last)
            {
                error = "Start and end must be complete finite numbers in seconds.";
                return std::nullopt;
            }
            result.render.startSeconds = *first;
            result.render.endSeconds = *last;
        }
        result.gif.width = canvasWidths[static_cast<std::size_t>(selectedCanvas - 1)];
        result.gif.height = result.gif.width * 9 / 16;
        result.gif.framesPerSecond = frameRate.control.getSelectedId();
        result.gif.transparentBackground = background.control.getSelectedId() == 1;
        if (selectedColour == 5)
        {
            const auto* track = project.findTrack(result.trackId);
            if (track == nullptr)
            {
                error = "Select a track to use its color, or choose another bar color.";
                return std::nullopt;
            }
            result.gif.barColour = track->colour;
        }
        else
            result.gif.barColour = juce::Colour(barColours[static_cast<std::size_t>(selectedColour - 1)]);
        if (const auto valid = SpectrumGif::validate(result.gif); valid.failed())
        {
            error = valid.getErrorMessage();
            return std::nullopt;
        }
        if (!RenderEngine::resolveRange(project, result.render, error))
            return std::nullopt;
        return result;
    }

    void refresh()
    {
        const auto custom = range.control.getSelectedId() == 4;
        start.label.setVisible(custom);
        start.control.setVisible(custom);
        end.label.setVisible(custom);
        end.control.setVisible(custom);
        juce::String error;
        const auto selected = readSettings(error);
        exportButton.setEnabled(selected.has_value());
        status.setColour(juce::Label::textColourId,
                         juce::Colour(selected ? StudioColours::secondaryText : StudioColours::orange));
        status.setText(selected ? "No audio or extra tail. Align the GIF with the same range's audio in your video editor."
                                : error, juce::dontSendNotification);
        status.setTooltip(status.getText());
        if (auto* handler = status.getAccessibilityHandler())
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
        if (selected)
        {
            auto style = selected->gif;
            style.width = 640;
            style.height = 360;
            std::vector<float> levels(static_cast<std::size_t>(style.barCount));
            for (int bar = 0; bar < style.barCount; ++bar)
                levels[static_cast<std::size_t>(bar)] = static_cast<float>(
                    0.08 + 0.65 * std::exp(-0.5 * std::pow((bar - 22.0) / 9.0, 2.0))
                    + 0.28 * std::exp(-0.5 * std::pow((bar - 44.0) / 5.0, 2.0)));
            preview.image = SpectrumGif::drawFrame(levels, style);
        }
        else
            preview.image = {};
        preview.repaint();
        layout();
    }

    void layout()
    {
        if (owner.getWidth() <= 0 || owner.getHeight() <= 0)
            return;
        title.setBounds(14, 8, owner.getWidth() - 28, 30);
        subtitle.setBounds(14, 39, owner.getWidth() - 28, 36);
        viewport.setBounds(8, 80, owner.getWidth() - 16, juce::jmax(1, owner.getHeight() - 180));
        const auto width = juce::jmax(1, viewport.getWidth() - 16);
        int y = 8;
        source.layout(width, y);
        range.layout(width, y);
        start.layout(width, y);
        end.layout(width, y);
        y += 8;
        canvas.layout(width, y);
        frameRate.layout(width, y);
        colour.layout(width, y);
        background.layout(width, y);
        previewLabel.setBounds(12, y + 10, width - 24, 20);
        preview.setBounds(12, y + 34, width - 24, 112);
        content.setSize(width, y + 158);
        status.setBounds(14, owner.getHeight() - 94, owner.getWidth() - 28, 50);
        cancel.setBounds(owner.getWidth() - 250, owner.getHeight() - 40, 100, 28);
        exportButton.setBounds(owner.getWidth() - 138, owner.getHeight() - 40, 124, 28);
    }

    void submit()
    {
        if (completing)
            return;
        juce::String error;
        const auto selected = readSettings(error);
        if (!selected || !owner.onExport)
        {
            if (selected)
                error = "Spectrum export is unavailable. Close and reopen the dialog.";
            status.setText(error, juce::dontSendNotification);
            if (owner.isShowing())
                juce::AccessibilityHandler::postAnnouncement(error, juce::AccessibilityHandler::AnnouncementPriority::high);
            return;
        }
        completing = true;
        exportButton.setEnabled(false);
        auto callback = owner.onExport;
        if (auto* window = owner.findParentComponentOfClass<juce::DialogWindow>())
        {
            juce::ModalComponentManager::getInstance()->attachCallback(window,
                juce::ModalCallbackFunction::create([modalExport = std::move(callback), selection = *selected](int result)
                {
                    if (result == 1)
                        juce::MessageManager::callAsync([modalExport, selection] { modalExport(selection); });
                }));
            window->exitModalState(1);
        }
        else
        {
            const auto safe = juce::Component::SafePointer<SpectrumExportOptionsComponent>(&owner);
            juce::MessageManager::callAsync([safe, standaloneExport = std::move(callback), selection = *selected]
            {
                if (safe != nullptr)
                    standaloneExport(selection);
            });
        }
    }

    void dismiss()
    {
        if (completing)
            return;
        completing = true;
        if (auto* window = owner.findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState(0);
        else
            owner.setVisible(false);
    }

    SpectrumExportOptionsComponent& owner;
    const Project project;
    const SpectrumExportSettings original;
    StudioTheme theme;
    juce::TooltipWindow tooltips;
    juce::Viewport viewport;
    juce::Component content;
    juce::Label title, subtitle, status, previewLabel;
    Field<juce::ComboBox> source, range, canvas, frameRate, colour, background;
    Field<juce::TextEditor> start, end;
    SpectrumPreview preview;
    juce::TextButton cancel { "Cancel" }, exportButton { "Export GIF" };
    std::vector<juce::String> trackIds;
    bool completing = false;
};

SpectrumExportOptionsComponent::SpectrumExportOptionsComponent(Project project, SpectrumExportSettings initial)
{
    impl = std::make_unique<Impl>(*this, std::move(project), std::move(initial));
    setSize(640, 650);
}

SpectrumExportOptionsComponent::~SpectrumExportOptionsComponent()
{
    impl->viewport.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

std::optional<SpectrumExportSettings> SpectrumExportOptionsComponent::settings(juce::String& error) const
{
    return impl->readSettings(error);
}

void SpectrumExportOptionsComponent::show(
    const Project& project, const SpectrumExportSettings& initial, juce::Component& centre,
    std::function<void(SpectrumExportSettings)> callback)
{
    auto content = std::make_unique<SpectrumExportOptionsComponent>(project, initial);
    const auto safeOwner = juce::Component::SafePointer<juce::Component>(&centre);
    content->onExport = [safeOwner, onExported = std::move(callback)](SpectrumExportSettings value)
    {
        if (safeOwner != nullptr && onExported)
            onExported(std::move(value));
    };
    auto area = juce::Rectangle<int>(0, 0, 1280, 800);
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(centre.getScreenBounds()))
        area = display->userBounds.getLargestIntegerWithin();
    area = area.reduced(16).withTrimmedTop(36);
    if (area.isEmpty())
        area = { 0, 0, 1, 1 };
    content->setSize(juce::jmin(640, area.getWidth()), juce::jmin(650, area.getHeight()));
    const auto safe = juce::Component::SafePointer<SpectrumExportOptionsComponent>(content.get());
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Export spectrum GIF";
    options.dialogBackgroundColour = juce::Colour(StudioColours::panel);
    options.content.setOwned(content.release());
    options.componentToCentreAround = &centre;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (auto* window = options.launchAsync())
    {
        window->setResizeLimits(juce::jmin(360, area.getWidth()), juce::jmin(350, area.getHeight()),
                               area.getWidth(), area.getHeight());
        window->setBounds(window->getBounds().constrainedWithin(area));
        juce::MessageManager::callAsync([safe]
        {
            if (safe != nullptr && safe->isShowing())
                safe->impl->source.control.grabKeyboardFocus();
        });
    }
}

void SpectrumExportOptionsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void SpectrumExportOptionsComponent::resized()
{
    if (impl)
        impl->layout();
}

bool SpectrumExportOptionsComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
        impl->dismiss();
    else if (key == juce::KeyPress::returnKey)
        impl->submit();
    else
        return false;
    return true;
}

void SpectrumExportOptionsComponent::focusOfChildComponentChanged(FocusChangeType)
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (impl && focused != nullptr && impl->content.isParentOf(focused))
    {
        const auto bounds = impl->content.getLocalArea(focused, focused->getLocalBounds()).expanded(0, 8);
        const auto view = impl->viewport.getViewArea();
        if (bounds.getY() < view.getY())
            impl->viewport.setViewPosition(0, bounds.getY());
        else if (bounds.getBottom() > view.getBottom())
            impl->viewport.setViewPosition(0, bounds.getBottom() - view.getHeight());
    }
}
}

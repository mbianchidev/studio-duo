#include "AudioExportOptionsComponent.h"

#include "StudioTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <sstream>
#include <utility>

namespace studio
{
namespace
{
constexpr std::array exportFormats {
    AudioExportFormat::wav,
    AudioExportFormat::aiff,
    AudioExportFormat::flac,
    AudioExportFormat::oggVorbis,
    AudioExportFormat::mp3
};

constexpr std::array mp3Bitrates { 64, 96, 128, 160, 192, 224, 256, 320 };

class ExportActionButton final : public juce::TextButton
{
public:
    void paintButton(juce::Graphics& graphics, bool highlighted, bool down) override
    {
        juce::TextButton::paintButton(graphics, highlighted, down);
        if (hasKeyboardFocus(true))
        {
            graphics.setColour(findColour(juce::TextButton::textColourOffId));
            graphics.drawRoundedRectangle(
                getLocalBounds().toFloat().reduced(2.5f), 2.0f, 2.0f);
        }
    }
};

bool isLossy(AudioExportFormat format)
{
    return format == AudioExportFormat::mp3
        || format == AudioExportFormat::oggVorbis;
}

juce::String numberText(double value)
{
    auto text = juce::String(value, 9);
    if (text.containsChar('.'))
        text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text;
}

std::optional<double> parseNumber(const juce::String& text)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return std::nullopt;

    std::istringstream input(trimmed.toStdString());
    input.imbue(std::locale::classic());
    double value = 0.0;
    input >> std::noskipws >> value;
    if (input.fail() || !input.eof() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

bool readNumber(
    const juce::TextEditor& editor,
    double minimum,
    std::optional<double> maximum,
    const juce::String& name,
    double& value,
    juce::String& error)
{
    const auto parsed = parseNumber(editor.getText());
    if (!parsed)
    {
        error = name + " must be a finite number, without units or other text.";
        return false;
    }
    if (*parsed < minimum || (maximum && *parsed > *maximum))
    {
        error = maximum
            ? name + " must be between " + numberText(minimum)
                + " and " + numberText(*maximum) + "."
            : name + " must be a non-negative finite number.";
        return false;
    }
    value = *parsed;
    return true;
}

template <typename Control>
struct Field
{
    juce::Label label;
    Control control;

    void initialise(
        juce::Component& parent,
        const juce::String& id,
        const juce::String& title,
        const juce::String& tooltip)
    {
        label.setText(title, juce::dontSendNotification);
        label.setComponentID(id + ".label");
        label.setFont(juce::FontOptions(13.0f));
        label.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
        label.setBorderSize({});
        parent.addAndMakeVisible(label);

        control.setComponentID(id);
        control.setName(title);
        control.setTitle(title);
        control.setDescription(tooltip);
        control.setTooltip(tooltip);
        control.setWantsKeyboardFocus(true);
        parent.addAndMakeVisible(control);
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

    void layout(int x, int& y, int width, bool stacked)
    {
        if (!control.isVisible())
            return;

        if (stacked)
        {
            label.setBounds(x, y, width, 18);
            control.setBounds(x, y + 20, width, 28);
            y += 57;
        }
        else
        {
            const auto labelWidth = juce::jmin(152, width / 3);
            label.setBounds(x, y, labelWidth, 28);
            control.setBounds(
                x + labelWidth + 12,
                y,
                juce::jmax(1, width - labelWidth - 12),
                28);
            y += 37;
        }
    }
};

void initialiseLabel(
    juce::Component& parent,
    juce::Label& label,
    const juce::String& id,
    const juce::String& text,
    float fontSize,
    juce::Colour colour,
    bool bold = false)
{
    label.setComponentID(id);
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(
        fontSize,
        bold ? juce::Font::bold : juce::Font::plain));
    label.setColour(juce::Label::textColourId, colour);
    label.setBorderSize({});
    parent.addAndMakeVisible(label);
}
}

struct AudioExportOptionsComponent::Impl
{
    using ComboField = Field<juce::ComboBox>;
    using NumberField = Field<juce::TextEditor>;

    struct MarkerChoice
    {
        juce::String id;
        juce::String name;
        double seconds = 0.0;
    };

    struct TailChoice
    {
        int mode = 2;
        juce::String seconds { "2" };
    };

    Impl(
        AudioExportOptionsComponent& ownerIn,
        std::optional<Project> projectIn,
        const MixExportSettings& initial)
        : owner(ownerIn),
          project(std::move(projectIn)),
          initialAudio(initial.audio),
          tooltipWindow(&owner, 650)
    {
        owner.setLookAndFeel(&theme);
        owner.setName(project ? "Mix export options" : "Mastering export options");
        owner.setTitle(owner.getName());
        owner.setFocusContainerType(
            juce::Component::FocusContainerType::keyboardFocusContainer);

        initialiseLabel(
            owner, title, "export.title",
            project ? "Export mix" : "Export master",
            21.0f, juce::Colour(StudioColours::text), true);
        initialiseLabel(
            owner, subtitle, "export.subtitle",
            project ? "Choose the audio format and export range."
                    : "Choose the format for the mastering render.",
            13.0f, juce::Colour(StudioColours::secondaryText));

        content.setComponentID("export.scrollContent");
        content.setWantsKeyboardFocus(false);
        viewport.setComponentID("export.viewport");
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setScrollBarThickness(12);
        viewport.setWantsKeyboardFocus(false);
        owner.addAndMakeVisible(viewport);

        const auto secondary = juce::Colour(StudioColours::secondaryText);
        initialiseLabel(content, fileHeading, "export.fileHeading",
                        "FILE FORMAT", 12.0f, secondary, true);
        initialiseLabel(content, processingHeading, "export.processingHeading",
                        "LEVEL PROCESSING", 12.0f, secondary, true);
        initialiseLabel(content, rangeHeading, "export.rangeHeading",
                        "RANGE AND FINISH", 12.0f, secondary, true);
        initialiseLabel(content, formatHint, "export.formatHint",
                        {}, 12.0f, secondary);
        initialiseLabel(content, markerHint, "export.markerHint",
                        {}, 12.0f, secondary);
        initialiseLabel(content, rangeSummary, "export.rangeSummary",
                        {}, 13.0f, juce::Colour(StudioColours::text));
        rangeSummary.setFont(juce::FontOptions(
            juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        rangeSummary.setColour(
            juce::Label::backgroundColourId,
            juce::Colour(StudioColours::window));
        rangeSummary.setColour(
            juce::Label::outlineColourId,
            juce::Colour(StudioColours::border));
        rangeSummary.setBorderSize(juce::BorderSize<int>(10, 12, 10, 12));

        preset.initialise(content, "export.preset", "Preset",
                          "Apply a complete set of audio format and level settings. "
                          "The export range, tail and fades are unchanged.");
        format.initialise(content, "export.format", "Format",
                          "Choose WAV, AIFF, FLAC, Ogg Vorbis or MP3.");
        sampleRate.initialise(content, "export.sampleRate", "Sample rate",
                              "The output sample rate. Choices depend on the format.");
        bitDepth.initialise(content, "export.bitDepth", "Bit depth",
                            "Integer PCM precision, or 32-bit floating point for WAV.");
        channels.initialise(content, "export.channels", "Channels",
                            "Export stereo or a mono downmix.");
        mp3Mode.initialise(content, "export.mp3Mode", "MP3 bitrate mode",
                           "Constant bitrate fixes the bitrate. Variable bitrate targets quality.");
        mp3Bitrate.initialise(content, "export.mp3Bitrate", "MP3 bitrate",
                              "Constant bitrate in kilobits per second.");
        mp3Quality.initialise(content, "export.mp3Quality", "MP3 VBR quality",
                              "Variable-bitrate quality: 0 is best, 9 produces the smallest files.");
        oggQuality.initialise(content, "export.oggQuality", "Vorbis quality",
                              "Ogg Vorbis quality: 0 is smallest, 10 is highest quality.");
        flacCompression.initialise(content, "export.flacCompression", "FLAC compression",
                                   "1 encodes fastest; 8 produces the smallest lossless files.");
        dither.initialise(content, "export.dither", "Dither",
                          "TPDF adds low-level noise when quantizing integer PCM. "
                          "It is not applied to floating-point or lossy formats.");
        normalize.initialise(content, "export.normalize", "Peak normalization",
                             "Adjust the rendered sample peak to the target. This is not loudness normalization.");
        normalize.control.setButtonText("Normalize peak");
        normalizeTarget.initialise(content, "export.normalizeTarget", "Target (dBFS)",
                                   "Peak normalization target, from -24 to 0 dBFS.");

        range.initialise(content, "export.range", "Export range",
                         "Export the entire project, loop boundaries, two named markers or custom seconds.");
        startMarker.initialise(content, "export.startMarker", "Start marker",
                               "Start at this named marker. Selections use stable marker IDs, not names.");
        endMarker.initialise(content, "export.endMarker", "End marker",
                             "End at this named marker, which must follow the start marker.");
        start.initialise(content, "export.startSeconds", "Start (seconds)",
                         "An exact, non-negative start time in seconds.");
        end.initialise(content, "export.endSeconds", "End (seconds)",
                       "An exact end time in seconds, later than the start.");
        tailMode.initialise(content, "export.tailMode", "Effect tail",
                            "Automatic uses the reported plug-in tail, up to 30 seconds. "
                            "None stops at the range end. Custom adds a fixed duration.");
        tail.initialise(content, "export.tailSeconds", "Tail (seconds)",
                        "Extra time after the selected range, from 0 to 30 seconds.");
        fadeIn.initialise(content, "export.fadeInSeconds", "Fade in (seconds)",
                          "A fade at the start, from 0 to 30 seconds, no longer than the export.");
        fadeOut.initialise(content, "export.fadeOutSeconds", "Fade out (seconds)",
                           "A fade at the very end, including any tail, from 0 to 30 seconds.");

        preset.control.addItem("Current settings", 1);
        preset.control.addItem("Audio CD - WAV 44.1 kHz / 16-bit", 2);
        preset.control.addItem("High-resolution - WAV 96 kHz / 24-bit", 3);
        preset.control.addItem("MP3 reference - 44.1 kHz / 320 kbps", 4);
        preset.control.addItem("Lossless - FLAC 48 kHz / 24-bit", 5);
        preset.control.setTextWhenNothingSelected("Custom settings");
        for (size_t index = 0; index < exportFormats.size(); ++index)
            format.control.addItem(
                AudioExport::formatName(exportFormats[index]),
                static_cast<int>(index) + 1);
        channels.control.addItem("Stereo", 2);
        channels.control.addItem("Mono", 1);
        mp3Mode.control.addItem("Constant bitrate (CBR)", 1);
        mp3Mode.control.addItem("Variable bitrate (VBR)", 2);
        for (const auto bitrate : mp3Bitrates)
            mp3Bitrate.control.addItem(juce::String(bitrate) + " kbps", bitrate);
        for (int quality = 0; quality <= 9; ++quality)
            mp3Quality.control.addItem(
                juce::String(quality)
                    + (quality == 0 ? " - best" : quality == 9 ? " - smallest" : ""),
                quality + 1);
        for (int quality = 0; quality <= 10; ++quality)
            oggQuality.control.addItem(
                juce::String(quality)
                    + (quality == 0 ? " - smallest" : quality == 10 ? " - best" : ""),
                quality + 1);
        for (int level = 1; level <= 8; ++level)
            flacCompression.control.addItem(
                juce::String(level)
                    + (level == 1 ? " - fastest" : level == 8 ? " - smallest" : ""),
                level + 1);
        dither.control.addItem("None", 1);
        dither.control.addItem("TPDF", 2);
        range.control.addItem("Entire project", 1);
        range.control.addItem("Loop boundaries", 2);
        range.control.addItem("Between named markers", 3);
        range.control.addItem("Custom seconds", 4);
        tailMode.control.addItem("Automatic - reported plug-in tail", 1);
        tailMode.control.addItem("None - exact range", 2);
        tailMode.control.addItem("Custom duration", 3);

        initialiseMarkers(initial);
        activeRange = juce::jlimit(1, 4, static_cast<int>(initial.range) + 1);
        range.control.setSelectedId(activeRange, juce::dontSendNotification);
        tailChoices[0].mode = 1;
        auto& initialTail = tailChoices[static_cast<size_t>(activeRange - 1)];
        initialTail.mode = !initial.tailSeconds ? 1 : *initial.tailSeconds == 0.0 ? 2 : 3;
        if (initial.tailSeconds)
            initialTail.seconds = numberText(*initial.tailSeconds);
        tailMode.control.setSelectedId(initialTail.mode, juce::dontSendNotification);
        tail.control.setText(initialTail.seconds, false);
        start.control.setText(numberText(initial.startSeconds), false);
        end.control.setText(numberText(initial.endSeconds), false);
        fadeIn.control.setText(numberText(initial.fadeInSeconds), false);
        fadeOut.control.setText(numberText(initial.fadeOutSeconds), false);

        for (auto* field : { &normalizeTarget, &start, &end, &tail, &fadeIn, &fadeOut })
        {
            auto& editor = field->control;
            editor.setMultiLine(false);
            editor.setSelectAllWhenFocused(true);
            editor.setInputRestrictions(64);
            editor.setFont(juce::FontOptions(14.0f));
            editor.setColour(juce::TextEditor::backgroundColourId,
                             juce::Colour(StudioColours::window));
            editor.setColour(juce::TextEditor::textColourId,
                             juce::Colour(StudioColours::text));
            editor.setColour(juce::TextEditor::outlineColourId,
                             juce::Colour(StudioColours::border));
            editor.setColour(juce::TextEditor::focusedOutlineColourId,
                             juce::Colour(StudioColours::orange));
        }
        for (auto* field : { &preset, &format, &sampleRate, &bitDepth, &channels,
                             &mp3Mode, &mp3Bitrate, &mp3Quality, &oggQuality,
                             &flacCompression, &dither, &range, &startMarker,
                             &endMarker, &tailMode })
        {
            auto& combo = field->control;
            combo.setEditableText(false);
            combo.setColour(juce::ComboBox::backgroundColourId,
                            juce::Colour(StudioColours::raised));
            combo.setColour(juce::ComboBox::textColourId,
                            juce::Colour(StudioColours::text));
            combo.setColour(juce::ComboBox::outlineColourId,
                            juce::Colour(StudioColours::border));
            combo.setColour(juce::ComboBox::focusedOutlineColourId,
                            juce::Colour(StudioColours::orange));
            combo.setColour(juce::ComboBox::arrowColourId,
                            juce::Colour(StudioColours::text));
        }

        initialiseLabel(owner, status, "export.validation",
                        {}, 12.0f, secondary);
        status.setTitle("Export validation");
        cancel.setButtonText("Cancel");
        cancel.setComponentID("export.cancel");
        cancel.setTitle("Cancel export");
        cancel.setTooltip("Close without exporting. Escape also cancels.");
        exportButton.setButtonText("Export...");
        exportButton.setComponentID("export.submit");
        exportButton.setTitle("Export audio");
        exportButton.setTooltip("Validate these settings, then choose a destination file.");
        exportButton.setColour(
            juce::TextButton::buttonColourId,
            juce::Colour(StudioColours::orange));
        exportButton.setColour(
            juce::TextButton::textColourOffId, juce::Colours::black);
        owner.addAndMakeVisible(cancel);
        owner.addAndMakeVisible(exportButton);

        applyAudioSettings(initial.audio);
        preset.control.setSelectedId(1, juce::dontSendNotification);
        connectCallbacks();
    }

    std::optional<AudioExportFormat> selectedFormat() const
    {
        const auto index = format.control.getSelectedId() - 1;
        if (!juce::isPositiveAndBelow(index, static_cast<int>(exportFormats.size())))
            return std::nullopt;
        return exportFormats[static_cast<size_t>(index)];
    }

    std::optional<double> selectedSampleRate() const
    {
        const auto index = sampleRate.control.getSelectedId() - 1;
        if (!juce::isPositiveAndBelow(index, static_cast<int>(sampleRates.size())))
            return std::nullopt;
        return sampleRates[static_cast<size_t>(index)];
    }

    bool integerOutput() const
    {
        const auto selected = selectedFormat();
        const auto depth = bitDepth.control.getSelectedId();
        return selected && !isLossy(*selected) && (depth == 16 || depth == 24);
    }

    void rebuildFormatOptions(double preferredRate, int preferredDepth)
    {
        const auto selected = selectedFormat();
        if (!selected)
            return;
        sampleRates = AudioExport::sampleRates(*selected);
        sampleRate.control.clear(juce::dontSendNotification);
        auto rateIndex = 0;
        const auto preferred = std::find(sampleRates.begin(), sampleRates.end(), preferredRate);
        const auto fallback = std::find(sampleRates.begin(), sampleRates.end(), 48000.0);
        if (preferred != sampleRates.end())
            rateIndex = static_cast<int>(std::distance(sampleRates.begin(), preferred));
        else if (fallback != sampleRates.end())
            rateIndex = static_cast<int>(std::distance(sampleRates.begin(), fallback));
        for (size_t index = 0; index < sampleRates.size(); ++index)
            sampleRate.control.addItem(
                numberText(sampleRates[index] / 1000.0) + " kHz",
                static_cast<int>(index) + 1);
        sampleRate.control.setSelectedId(
            sampleRates.empty() ? 0 : rateIndex + 1, juce::dontSendNotification);

        const auto depths = AudioExport::bitDepths(*selected);
        bitDepth.control.clear(juce::dontSendNotification);
        for (const auto depth : depths)
            bitDepth.control.addItem(
                juce::String(depth) + (depth == 32 ? "-bit float" : "-bit"),
                depth);
        auto depth = preferredDepth;
        if (std::find(depths.begin(), depths.end(), depth) == depths.end())
            depth = std::find(depths.begin(), depths.end(), 24) != depths.end()
                ? 24 : depths.empty() ? 0 : depths.front();
        if (isLossy(*selected))
        {
            if (std::find(depths.begin(), depths.end(), 32) == depths.end())
                bitDepth.control.addItem("32-bit encoder input", 32);
            depth = 32;
        }
        bitDepth.control.setSelectedId(depth, juce::dontSendNotification);
        if (!integerOutput())
            dither.control.setSelectedId(1, juce::dontSendNotification);
    }

    void applyAudioSettings(const AudioExportSettings& audio)
    {
        const juce::ScopedValueSetter<bool> guard(updating, true);
        const auto iterator = std::find(exportFormats.begin(), exportFormats.end(), audio.format);
        format.control.setSelectedId(
            iterator == exportFormats.end()
                ? 1 : static_cast<int>(std::distance(exportFormats.begin(), iterator)) + 1,
            juce::dontSendNotification);
        rebuildFormatOptions(audio.sampleRate, audio.bitDepth);
        channels.control.setSelectedId(audio.channels == 1 ? 1 : 2, juce::dontSendNotification);
        dither.control.setSelectedId(
            integerOutput() && audio.dither == AudioExportDither::tpdf ? 2 : 1,
            juce::dontSendNotification);
        mp3Mode.control.setSelectedId(
            audio.mp3BitrateMode == Mp3BitrateMode::variable ? 2 : 1,
            juce::dontSendNotification);
        mp3Bitrate.control.setSelectedId(
            std::find(mp3Bitrates.begin(), mp3Bitrates.end(), audio.mp3BitrateKbps)
                    != mp3Bitrates.end()
                ? audio.mp3BitrateKbps : 320,
            juce::dontSendNotification);
        mp3Quality.control.setSelectedId(
            juce::jlimit(0, 9, audio.mp3VbrQuality) + 1, juce::dontSendNotification);
        oggQuality.control.setSelectedId(
            juce::jlimit(0, 10, audio.oggQuality) + 1, juce::dontSendNotification);
        flacCompression.control.setSelectedId(
            juce::jlimit(1, 8, audio.flacCompressionLevel) + 1, juce::dontSendNotification);
        normalize.control.setToggleState(audio.normalizePeak, juce::dontSendNotification);
        normalizeTarget.control.setText(numberText(audio.normalizePeakDbfs), false);
    }

    void applyPreset()
    {
        if (updating)
            return;
        const auto selected = preset.control.getSelectedId();
        if (selected == 0)
            return;

        AudioExportSettings audio;
        switch (selected)
        {
            case 1: audio = initialAudio; break;
            case 2:
                audio.sampleRate = 44100.0;
                audio.bitDepth = 16;
                audio.dither = AudioExportDither::tpdf;
                break;
            case 3: audio.sampleRate = 96000.0; break;
            case 4:
                audio.format = AudioExportFormat::mp3;
                audio.sampleRate = 44100.0;
                audio.bitDepth = 32;
                break;
            case 5: audio.format = AudioExportFormat::flac; break;
            default: return;
        }
        applyAudioSettings(audio);
        refresh();
    }

    void initialiseMarkers(const MixExportSettings& initial)
    {
        if (project)
            for (const auto& marker : project->sections)
                markers.push_back({ marker.id, marker.name, marker.timeSeconds });
        std::stable_sort(markers.begin(), markers.end(), [](const auto& first, const auto& second)
        {
            if (std::isfinite(first.seconds) != std::isfinite(second.seconds))
                return std::isfinite(first.seconds);
            return std::isfinite(first.seconds) && first.seconds < second.seconds;
        });
        std::vector<juce::String> labels;
        labels.reserve(markers.size());
        for (const auto& marker : markers)
        {
            const auto time = std::isfinite(marker.seconds)
                ? juce::String(marker.seconds, 3) + " s" : "invalid time";
            labels.push_back(
                (marker.name.isNotEmpty() ? marker.name : "Unnamed marker") + " - " + time);
        }
        for (size_t index = 0; index < markers.size(); ++index)
        {
            auto label = labels[index];
            if (std::count(labels.begin(), labels.end(), label) > 1)
                label += " [" + juce::String(static_cast<int>(index) + 1) + "]";
            const auto itemId = static_cast<int>(index) + 1;
            startMarker.control.addItem(label, itemId);
            endMarker.control.addItem(label, itemId);
        }
        auto defaultEnd = 0;
        if (!markers.empty())
            for (size_t index = 1; index < markers.size(); ++index)
                if (markers[index].id.isNotEmpty() && markers[index].id != markers.front().id)
                    defaultEnd = static_cast<int>(index) + 1;
        hasMarkerPair = defaultEnd != 0 && markers.front().id.isNotEmpty();
        range.control.setItemEnabled(3, hasMarkerPair);

        const auto select = [this](
            juce::ComboBox& combo, const juce::String& id, int defaultId, const char* missingText)
        {
            auto selection = defaultId;
            if (id.isNotEmpty())
            {
                const auto found = std::find_if(markers.begin(), markers.end(), [&id](const auto& marker)
                {
                    return marker.id == id;
                });
                selection = found == markers.end()
                    ? 0 : static_cast<int>(std::distance(markers.begin(), found)) + 1;
            }
            combo.setTextWhenNothingSelected(
                id.isNotEmpty() && selection == 0 ? missingText : "Choose a marker");
            combo.setSelectedId(selection, juce::dontSendNotification);
        };
        select(startMarker.control, initial.startMarkerId, hasMarkerPair ? 1 : 0,
               "Start marker unavailable - choose another");
        select(endMarker.control, initial.endMarkerId, defaultEnd,
               "End marker unavailable - choose another");
    }

    void connectCallbacks()
    {
        const auto safe = juce::Component::SafePointer<AudioExportOptionsComponent>(&owner);
        preset.control.onChange = [safe]
        {
            if (safe != nullptr)
                safe->impl->applyPreset();
        };
        format.control.onChange = [safe]
        {
            if (safe == nullptr || safe->impl->updating)
                return;
            auto& state = *safe->impl;
            state.rebuildFormatOptions(
                state.selectedSampleRate().value_or(48000.0),
                state.bitDepth.control.getSelectedId());
            state.audioChanged();
        };
        const auto audioChanged = [safe]
        {
            if (safe != nullptr)
                safe->impl->audioChanged();
        };
        for (auto* field : { &sampleRate, &bitDepth, &channels, &mp3Mode, &mp3Bitrate,
                             &mp3Quality, &oggQuality, &flacCompression, &dither })
            field->control.onChange = audioChanged;
        normalize.control.onClick = audioChanged;
        normalizeTarget.control.onTextChange = audioChanged;
        range.control.onChange = [safe]
        {
            if (safe != nullptr)
                safe->impl->rangeChanged();
        };
        const auto changed = [safe]
        {
            if (safe != nullptr && !safe->impl->updating)
                safe->impl->refresh();
        };
        for (auto* field : { &startMarker, &endMarker, &tailMode })
            field->control.onChange = changed;
        for (auto* field : { &start, &end, &tail, &fadeIn, &fadeOut })
            field->control.onTextChange = changed;
        for (auto* field : { &normalizeTarget, &start, &end, &tail, &fadeIn, &fadeOut })
        {
            field->control.onReturnKey = [safe]
            {
                if (safe != nullptr)
                    safe->impl->submit();
            };
            field->control.onEscapeKey = [safe]
            {
                if (safe != nullptr)
                    safe->impl->dismiss();
            };
        }
        exportButton.onClick = [safe]
        {
            if (safe != nullptr)
                safe->impl->submit();
        };
        cancel.onClick = [safe]
        {
            if (safe != nullptr)
                safe->impl->dismiss();
        };
    }

    void audioChanged()
    {
        if (updating)
            return;
        if (!integerOutput())
            dither.control.setSelectedId(1, juce::dontSendNotification);
        preset.control.setSelectedId(0, juce::dontSendNotification);
        refresh();
    }

    void rangeChanged()
    {
        if (updating)
            return;
        const auto selected = range.control.getSelectedId();
        if (juce::isPositiveAndBelow(selected - 1, 4) && selected != activeRange)
        {
            tailChoices[static_cast<size_t>(activeRange - 1)] = {
                tailMode.control.getSelectedId(), tail.control.getText()
            };
            activeRange = selected;
            const auto& choice = tailChoices[static_cast<size_t>(activeRange - 1)];
            tailMode.control.setSelectedId(choice.mode, juce::dontSendNotification);
            tail.control.setText(choice.seconds, false);
        }
        refresh();
    }

    std::optional<MixExportSettings> readSettings(juce::String& error) const
    {
        error.clear();
        MixExportSettings result;
        const auto selected = selectedFormat();
        const auto rate = selectedSampleRate();
        if (!selected || !rate || bitDepth.control.getSelectedId() == 0
            || !juce::isPositiveAndBelow(channels.control.getSelectedId() - 1, 2)
            || !juce::isPositiveAndBelow(dither.control.getSelectedId() - 1, 2))
        {
            error = "Choose a format, sample rate, bit depth, channel layout and dither option.";
            return std::nullopt;
        }
        result.audio.format = *selected;
        result.audio.sampleRate = *rate;
        result.audio.bitDepth = isLossy(*selected) ? 32 : bitDepth.control.getSelectedId();
        result.audio.channels = channels.control.getSelectedId();
        result.audio.dither = dither.control.getSelectedId() == 2
            ? AudioExportDither::tpdf : AudioExportDither::none;
        result.audio.mp3BitrateMode = mp3Mode.control.getSelectedId() == 2
            ? Mp3BitrateMode::variable : Mp3BitrateMode::constant;
        result.audio.mp3BitrateKbps = mp3Bitrate.control.getSelectedId();
        result.audio.mp3VbrQuality = mp3Quality.control.getSelectedId() - 1;
        result.audio.oggQuality = oggQuality.control.getSelectedId() - 1;
        result.audio.flacCompressionLevel = flacCompression.control.getSelectedId() - 1;
        result.audio.normalizePeak = normalize.control.getToggleState();
        if (result.audio.normalizePeak)
        {
            if (!readNumber(normalizeTarget.control, -24.0, 0.0,
                            "Normalization target (dBFS)", result.audio.normalizePeakDbfs, error))
                return std::nullopt;
        }
        else if (const auto target = parseNumber(normalizeTarget.control.getText());
                 target && *target >= -24.0 && *target <= 0.0)
        {
            result.audio.normalizePeakDbfs = *target;
        }
        if (*selected == AudioExportFormat::mp3
            && !juce::isPositiveAndBelow(mp3Mode.control.getSelectedId() - 1, 2))
        {
            error = "Choose constant or variable MP3 bitrate.";
            return std::nullopt;
        }
        const auto audioResult = AudioExport::validate(result.audio);
        if (audioResult.failed())
        {
            error = audioResult.getErrorMessage();
            return std::nullopt;
        }
        if (!project)
            return result;

        const auto selectedRange = range.control.getSelectedId();
        if (!juce::isPositiveAndBelow(selectedRange - 1, 4))
        {
            error = "Choose an export range.";
            return std::nullopt;
        }
        result.range = static_cast<MixExportRange>(selectedRange - 1);
        if (result.range == MixExportRange::markers)
        {
            const auto first = startMarker.control.getSelectedId() - 1;
            const auto last = endMarker.control.getSelectedId() - 1;
            if (!hasMarkerPair)
            {
                error = "Create at least two named markers in Tracking Setup or the timeline marker lane.";
                return std::nullopt;
            }
            if (!juce::isPositiveAndBelow(first, static_cast<int>(markers.size()))
                || !juce::isPositiveAndBelow(last, static_cast<int>(markers.size())))
            {
                error = "Choose an available start and end marker. A previously selected marker may have been deleted.";
                return std::nullopt;
            }
            result.startMarkerId = markers[static_cast<size_t>(first)].id;
            result.endMarkerId = markers[static_cast<size_t>(last)].id;
        }
        if (result.range == MixExportRange::custom)
        {
            if (!readNumber(start.control, 0.0, std::nullopt,
                            "Start time (seconds)", result.startSeconds, error)
                || !readNumber(end.control, 0.0, std::nullopt,
                               "End time (seconds)", result.endSeconds, error))
                return std::nullopt;
        }
        switch (tailMode.control.getSelectedId())
        {
            case 1: result.tailSeconds.reset(); break;
            case 2: result.tailSeconds = 0.0; break;
            case 3:
            {
                double seconds = 0.0;
                if (!readNumber(tail.control, 0.0, 30.0, "Tail (seconds)", seconds, error))
                    return std::nullopt;
                result.tailSeconds = seconds;
                break;
            }
            default:
                error = "Choose automatic, no tail or a custom tail.";
                return std::nullopt;
        }
        if (!readNumber(fadeIn.control, 0.0, 30.0, "Fade in (seconds)", result.fadeInSeconds, error)
            || !readNumber(fadeOut.control, 0.0, 30.0, "Fade out (seconds)", result.fadeOutSeconds, error))
            return std::nullopt;

        const auto resolved = RenderEngine::resolveRange(*project, result, error);
        if (!resolved)
            return std::nullopt;
        result.startSeconds = resolved->startSeconds;
        result.endSeconds = resolved->endSeconds;
        const auto knownDuration = result.endSeconds - result.startSeconds
            + result.tailSeconds.value_or(0.0);
        if (result.fadeInSeconds > knownDuration || result.fadeOutSeconds > knownDuration)
        {
            error = "Fades cannot exceed the selected duration plus a fixed tail. "
                    "For an automatic tail, keep fades within the base duration.";
            return std::nullopt;
        }
        return result;
    }

    void updateVisibility()
    {
        const auto selected = selectedFormat().value_or(AudioExportFormat::wav);
        const auto mp3 = selected == AudioExportFormat::mp3;
        const auto vbr = mp3Mode.control.getSelectedId() == 2;
        bitDepth.setVisible(!isLossy(selected));
        bitDepth.setEnabled(!isLossy(selected));
        mp3Mode.setVisible(mp3);
        mp3Bitrate.setVisible(mp3);
        mp3Quality.setVisible(mp3);
        mp3Bitrate.setEnabled(mp3 && !vbr);
        mp3Quality.setEnabled(mp3 && vbr);
        oggQuality.setVisible(selected == AudioExportFormat::oggVorbis);
        flacCompression.setVisible(selected == AudioExportFormat::flac);
        dither.setEnabled(integerOutput());
        normalizeTarget.setEnabled(normalize.control.getToggleState());

        const auto mix = project.has_value();
        const auto markerRange = mix && range.control.getSelectedId() == 3;
        const auto customRange = mix && range.control.getSelectedId() == 4;
        rangeHeading.setVisible(mix);
        range.setVisible(mix);
        startMarker.setVisible(markerRange);
        endMarker.setVisible(markerRange);
        startMarker.setEnabled(hasMarkerPair);
        endMarker.setEnabled(hasMarkerPair);
        markerHint.setVisible(mix);
        start.setVisible(customRange);
        end.setVisible(customRange);
        tailMode.setVisible(mix);
        tail.setVisible(mix && tailMode.control.getSelectedId() == 3);
        fadeIn.setVisible(mix);
        fadeOut.setVisible(mix);
        rangeSummary.setVisible(mix);

        markerHint.setText(
            hasMarkerPair
                ? "Create or edit markers in Tracking Setup or timeline marker lane."
                : "Marker ranges need two named markers. Create or edit markers in "
                  "Tracking Setup or timeline marker lane.",
            juce::dontSendNotification);
        juce::String hint;
        switch (selected)
        {
            case AudioExportFormat::wav:
                hint = bitDepth.control.getSelectedId() == 32
                    ? "Uncompressed floating-point audio. Dither is not applied."
                    : "Uncompressed PCM for editing, delivery and archival.";
                break;
            case AudioExportFormat::aiff:
                hint = "Uncompressed integer PCM in an AIFF container.";
                break;
            case AudioExportFormat::flac:
                hint = "Lossless audio. Compression changes file size, not sound quality.";
                break;
            case AudioExportFormat::oggVorbis:
                hint = "Lossy audio. Higher Vorbis quality produces larger files.";
                break;
            case AudioExportFormat::mp3:
                hint = "Lossy audio. CBR fixes bitrate; VBR targets quality (0 is best).";
                break;
        }
        formatHint.setText(hint, juce::dontSendNotification);
    }

    void refresh()
    {
        updateVisibility();
        layout();
        juce::String error;
        const auto value = readSettings(error);
        if (value && project)
        {
            const auto duration = value->endSeconds - value->startSeconds;
            juce::String text = "Start " + juce::String(value->startSeconds, 3)
                + " s  /  End " + juce::String(value->endSeconds, 3) + " s\n"
                + "Base duration " + juce::String(duration, 3) + " s";
            if (!value->tailSeconds)
                text += "\n+ Automatic plug-in tail (up to 30 s)";
            else if (*value->tailSeconds == 0.0)
                text += "\nNo tail - exact range";
            else
                text += "\n+ Tail " + juce::String(*value->tailSeconds, 3)
                    + " s = " + juce::String(duration + *value->tailSeconds, 3) + " s total";
            rangeSummary.setText(text, juce::dontSendNotification);
        }
        else if (project)
        {
            rangeSummary.setText(error, juce::dontSendNotification);
        }
        const auto showError = attempted && !value;
        status.setText(
            showError ? error : "Choose a destination in the next step.",
            juce::dontSendNotification);
        status.setTooltip(showError ? error : juce::String());
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(showError ? StudioColours::amber : StudioColours::secondaryText));
        status.setDescription(showError ? error : juce::String());
    }

    void layout()
    {
        auto bounds = owner.getLocalBounds();
        const auto margin = juce::jmin(20, bounds.getWidth() / 12);
        auto header = bounds.removeFromTop(70).reduced(margin, 8);
        title.setBounds(header.removeFromTop(28));
        subtitle.setBounds(header);
        auto footer = bounds.removeFromBottom(100).reduced(margin, 8);
        auto buttons = footer.removeFromBottom(32);
        const auto exportWidth = juce::jmin(126, buttons.getWidth() / 2);
        exportButton.setBounds(buttons.removeFromRight(exportWidth));
        buttons.removeFromRight(10);
        cancel.setBounds(buttons.removeFromRight(juce::jmin(92, buttons.getWidth())));
        footer.removeFromBottom(8);
        status.setBounds(footer);
        viewport.setBounds(bounds.reduced(8, 0));

        const auto width = juce::jmax(1, viewport.getWidth() - viewport.getScrollBarThickness());
        const auto inset = juce::jmin(12, width / 12);
        const auto fieldWidth = juce::jmax(1, width - 2 * inset);
        const auto stacked = width < 450;
        auto y = 12;
        const auto heading = [&](juce::Label& label)
        {
            label.setBounds(inset, y, fieldWidth, 20);
            y += 28;
        };
        const auto field = [&](auto& item) { item.layout(inset, y, fieldWidth, stacked); };
        heading(fileHeading);
        field(preset);
        field(format);
        field(sampleRate);
        field(bitDepth);
        field(channels);
        field(mp3Mode);
        field(mp3Bitrate);
        field(mp3Quality);
        field(oggQuality);
        field(flacCompression);
        formatHint.setBounds(inset, y, fieldWidth, stacked ? 42 : 32);
        y += stacked ? 56 : 46;
        heading(processingHeading);
        field(dither);
        field(normalize);
        field(normalizeTarget);
        if (project)
        {
            y += 14;
            heading(rangeHeading);
            field(range);
            field(startMarker);
            field(endMarker);
            field(start);
            field(end);
            markerHint.setBounds(inset, y, fieldWidth, stacked ? 55 : 36);
            y += stacked ? 65 : 46;
            field(tailMode);
            field(tail);
            field(fadeIn);
            field(fadeOut);
            y += 5;
            rangeSummary.setBounds(inset, y, fieldWidth, stacked ? 112 : 86);
            y += stacked ? 112 : 86;
        }
        content.setSize(width, y + 16);
    }

    void submit()
    {
        if (completing)
            return;
        attempted = true;
        juce::String error;
        const auto value = readSettings(error);
        if (!value)
        {
            refresh();
            if (owner.isShowing())
                juce::AccessibilityHandler::postAnnouncement(
                    error, juce::AccessibilityHandler::AnnouncementPriority::high);
            return;
        }
        completing = true;
        exportButton.setEnabled(false);
        auto exportCallback = owner.onExport;
        if (auto* window = owner.findParentComponentOfClass<juce::DialogWindow>())
        {
            // The modal callback runs before JUCE deletes the owned content.
            // Dispatching again lets a destination chooser open after that teardown.
            juce::ModalComponentManager::getInstance()->attachCallback(
                window,
                juce::ModalCallbackFunction::create(
                    [modalExport = std::move(exportCallback), selection = *value](int result) mutable
                    {
                        if (result == 1 && modalExport)
                            juce::MessageManager::callAsync(
                                [dispatchExport = std::move(modalExport), selection]() mutable
                                {
                                    dispatchExport(selection);
                                });
                    }));
            window->exitModalState(1);
            return;
        }
        const auto safe = juce::Component::SafePointer<AudioExportOptionsComponent>(&owner);
        juce::MessageManager::callAsync(
            [safe, standaloneExport = std::move(exportCallback), selection = *value]() mutable
            {
                if (safe != nullptr && standaloneExport)
                    standaloneExport(selection);
            });
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

    AudioExportOptionsComponent& owner;
    StudioTheme theme;
    const std::optional<Project> project;
    const AudioExportSettings initialAudio;
    juce::Viewport viewport;
    juce::Component content;
    juce::Label title, subtitle, fileHeading, processingHeading, rangeHeading;
    juce::Label formatHint, markerHint, rangeSummary, status;
    ComboField preset, format, sampleRate, bitDepth, channels;
    ComboField mp3Mode, mp3Bitrate, mp3Quality, oggQuality, flacCompression, dither;
    Field<juce::ToggleButton> normalize;
    NumberField normalizeTarget;
    ComboField range, startMarker, endMarker, tailMode;
    NumberField start, end, tail, fadeIn, fadeOut;
    juce::TextButton cancel;
    ExportActionButton exportButton;
    juce::TooltipWindow tooltipWindow;
    std::vector<double> sampleRates;
    std::vector<MarkerChoice> markers;
    std::array<TailChoice, 4> tailChoices;
    int activeRange = 1;
    bool hasMarkerPair = false;
    bool updating = false;
    bool attempted = false;
    bool completing = false;
};

AudioExportOptionsComponent::AudioExportOptionsComponent(
    std::optional<Project> projectForRanges,
    MixExportSettings initialSettings)
{
    impl = std::make_unique<Impl>(
        *this, std::move(projectForRanges), initialSettings);
    setSize(640, impl->project ? 760 : 610);
    impl->refresh();
}

AudioExportOptionsComponent::~AudioExportOptionsComponent()
{
    impl->viewport.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

std::optional<MixExportSettings> AudioExportOptionsComponent::settings(
    juce::String& error) const
{
    return impl->readSettings(error);
}

void AudioExportOptionsComponent::show(
    const Project* projectForRanges,
    const MixExportSettings& initialSettings,
    juce::Component& centreAround,
    std::function<void(MixExportSettings)> onExport)
{
    auto content = std::make_unique<AudioExportOptionsComponent>(
        projectForRanges ? std::optional<Project>(*projectForRanges) : std::nullopt,
        initialSettings);
    const auto safeOwner = juce::Component::SafePointer<juce::Component>(&centreAround);
    content->onExport = [safeOwner, callback = std::move(onExport)](MixExportSettings value) mutable
    {
        if (safeOwner != nullptr && callback)
            callback(std::move(value));
    };
    auto workArea = juce::Rectangle<int>(0, 0, 1280, 800);
    if (const auto* display = juce::Desktop::getInstance().getDisplays()
                                  .getDisplayForRect(centreAround.getScreenBounds()))
        workArea = display->userBounds.getLargestIntegerWithin();
    auto clientArea = workArea.reduced(16).withTrimmedTop(36);
    if (clientArea.isEmpty())
        clientArea = { workArea.getX(), workArea.getY(), 1, 1 };
    content->setSize(
        juce::jmin(content->getWidth(), juce::jmax(1, clientArea.getWidth())),
        juce::jmin(content->getHeight(), juce::jmax(1, clientArea.getHeight())));
    const auto safeContent = juce::Component::SafePointer<AudioExportOptionsComponent>(content.get());

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = projectForRanges ? "Export mix" : "Export master";
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
            juce::jmin(420, clientArea.getWidth()),
            juce::jmin(350, clientArea.getHeight()),
            juce::jmax(1, clientArea.getWidth()),
            juce::jmax(1, clientArea.getHeight()));
        window->setBounds(window->getBounds().constrainedWithin(clientArea));
        juce::MessageManager::callAsync([safeContent]
        {
            if (safeContent != nullptr && safeContent->isShowing())
                safeContent->impl->preset.control.grabKeyboardFocus();
        });
    }
}

void AudioExportOptionsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawHorizontalLine(69, 0.0f, static_cast<float>(getWidth()));
    graphics.drawHorizontalLine(
        getHeight() - 100, 0.0f, static_cast<float>(getWidth()));
}

void AudioExportOptionsComponent::resized()
{
    if (impl)
        impl->layout();
}

bool AudioExportOptionsComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        impl->dismiss();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        impl->submit();
        return true;
    }
    return false;
}

void AudioExportOptionsComponent::focusOfChildComponentChanged(FocusChangeType)
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (!impl || focused == nullptr || !impl->content.isParentOf(focused))
        return;
    const auto bounds = juce::BorderSize<int>(
        impl->content.getWidth() < 450 ? 24 : 6, 0, 6, 0)
                            .addedTo(impl->content.getLocalArea(
                                focused, focused->getLocalBounds()));
    const auto top = impl->viewport.getViewPositionY();
    const auto bottom = top + impl->viewport.getViewHeight();
    if (bounds.getY() < top)
        impl->viewport.setViewPosition(0, juce::jmax(0, bounds.getY()));
    else if (bounds.getBottom() > bottom)
        impl->viewport.setViewPosition(
            0, bounds.getBottom() - impl->viewport.getViewHeight());
}
}

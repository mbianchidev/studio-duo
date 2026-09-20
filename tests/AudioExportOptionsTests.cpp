#include "TestHarness.h"

#include "ui/AudioExportOptionsComponent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
using studio::AudioExportDither;
using studio::AudioExportFormat;
using studio::AudioExportOptionsComponent;
using studio::MixExportRange;
using studio::MixExportSettings;

juce::Component* findControl(juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id)
        return &component;
    for (auto* child : component.getChildren())
        if (auto* found = findControl(*child, id))
            return found;
    return nullptr;
}

template <typename Control>
Control& control(AudioExportOptionsComponent& component, const char* id)
{
    if (auto* found = dynamic_cast<Control*>(findControl(component, id)))
        return *found;
    throw std::runtime_error(std::string("Missing export control: ") + id);
}

void select(AudioExportOptionsComponent& component, const char* id, int item)
{
    control<juce::ComboBox>(component, id).setSelectedId(item, juce::sendNotificationSync);
}

void selectFormat(AudioExportOptionsComponent& component, AudioExportFormat format)
{
    select(component, "export.format", static_cast<int>(format) + 1);
}

void edit(AudioExportOptionsComponent& component, const char* id, const char* text)
{
    control<juce::TextEditor>(component, id).setText(text, false);
}

studio::Project markerProject()
{
    auto project = studio::Project::createDefault();
    project.markers = {
        { "marker-end", "Boundary", 6.0 },
        { "marker-start", "Boundary", 1.0 },
        { "marker-middle", "Middle", 3.0 }
    };
    project.loopStartSeconds = 2.0;
    project.loopEndSeconds = 5.0;
    return project;
}

void defaultsAndFormatChanges()
{
    AudioExportOptionsComponent component(std::nullopt, {});
    juce::String error;
    auto selected = component.settings(error);
    expect(selected && error.isEmpty()
               && selected->audio.format == AudioExportFormat::wav
               && selected->audio.sampleRate == 48000.0
               && selected->audio.bitDepth == 24
               && selected->audio.channels == 2
               && selected->audio.dither == AudioExportDither::none,
           "The export dialog starts with professional WAV 48 kHz/24-bit stereo defaults.");

    select(component, "export.bitDepth", 16);
    select(component, "export.dither", 2);
    selected = component.settings(error);
    expect(selected && selected->audio.dither == AudioExportDither::tpdf,
           "Integer WAV can explicitly enable TPDF dither.");
    select(component, "export.bitDepth", 32);
    selected = component.settings(error);
    expect(selected && selected->audio.bitDepth == 32
               && selected->audio.dither == AudioExportDither::none
               && !control<juce::ComboBox>(component, "export.dither").isEnabled()
               && control<juce::ComboBox>(component, "export.bitDepth").getText().contains("float"),
           "WAV float is labelled explicitly and clears/disables integer dither.");

    selectFormat(component, AudioExportFormat::aiff);
    selected = component.settings(error);
    expect(selected && selected->audio.bitDepth == 24
               && control<juce::ComboBox>(component, "export.bitDepth").getNumItems() == 2,
           "Changing from float WAV to AIFF explicitly selects a supported integer depth.");

    select(component, "export.preset", 3);
    selected = component.settings(error);
    expect(selected && selected->audio.sampleRate == 96000.0,
           "The high-resolution preset explicitly selects 96 kHz.");
    selectFormat(component, AudioExportFormat::mp3);
    selected = component.settings(error);
    expect(selected && selected->audio.format == AudioExportFormat::mp3
               && selected->audio.sampleRate <= 48000.0
               && selected->audio.bitDepth == 32
               && selected->audio.dither == AudioExportDither::none
               && !control<juce::ComboBox>(component, "export.bitDepth").isVisible()
               && !control<juce::ComboBox>(component, "export.bitDepth").isEnabled(),
           "Changing to MP3 replaces incompatible rates and hides PCM depth without stale dither.");

    bool exported = false;
    component.onExport = [&](auto) { exported = true; };
    select(component, "export.channels", 1);
    selectFormat(component, AudioExportFormat::flac);
    expect(!exported, "Changing combo selections never submits an export.");

    select(component, "export.sampleRate", 0);
    expect(!component.settings(error) && error.isNotEmpty(),
           "An unselected output sample rate is rejected rather than silently replaced.");
}

void codecQualityControls()
{
    AudioExportOptionsComponent component(std::nullopt, {});
    selectFormat(component, AudioExportFormat::mp3);
    expect(control<juce::ComboBox>(component, "export.mp3Mode").isVisible()
               && control<juce::ComboBox>(component, "export.mp3Bitrate").isEnabled()
               && !control<juce::ComboBox>(component, "export.mp3Quality").isEnabled(),
           "CBR enables bitrate and disables the inactive MP3 VBR quality control.");
    select(component, "export.mp3Bitrate", 192);
    select(component, "export.mp3Mode", 2);
    select(component, "export.mp3Quality", 10);
    juce::String error;
    auto selected = component.settings(error);
    expect(selected && selected->audio.mp3BitrateMode == studio::Mp3BitrateMode::variable
               && selected->audio.mp3VbrQuality == 9
               && selected->audio.mp3BitrateKbps == 192
               && !control<juce::ComboBox>(component, "export.mp3Bitrate").isEnabled()
               && control<juce::ComboBox>(component, "export.mp3Quality").isEnabled(),
           "VBR exposes the full 0..9 quality scale while preserving the visible inactive bitrate.");

    selectFormat(component, AudioExportFormat::oggVorbis);
    select(component, "export.oggQuality", 11);
    selected = component.settings(error);
    expect(selected && selected->audio.oggQuality == 10
               && control<juce::ComboBox>(component, "export.oggQuality").getNumItems() == 11
               && !control<juce::ComboBox>(component, "export.mp3Mode").isVisible(),
           "Ogg Vorbis exposes all quality levels from 0 through 10 and hides MP3 options.");

    selectFormat(component, AudioExportFormat::flac);
    auto& compression = control<juce::ComboBox>(component, "export.flacCompression");
    expect(compression.getNumItems() == 8
               && compression.getItemId(0) == 2
               && compression.getItemId(7) == 9
               && compression.getTooltip().contains("1 encodes fastest"),
           "FLAC offers only the supported levels 1..8 and explains the correct minimum.");
    select(component, "export.flacCompression", 2);
    selected = component.settings(error);
    expect(selected && selected->audio.flacCompressionLevel == 1,
           "The fastest FLAC option explicitly selects supported compression level 1.");
    select(component, "export.flacCompression", 9);
    selected = component.settings(error);
    expect(selected && selected->audio.flacCompressionLevel == 8
               && control<juce::ComboBox>(component, "export.flacCompression").isVisible()
               && !control<juce::ComboBox>(component, "export.oggQuality").isVisible(),
           "FLAC exposes its lossless compression levels without lossy quality controls.");

    MixExportSettings obsolete;
    obsolete.audio.format = AudioExportFormat::flac;
    obsolete.audio.flacCompressionLevel = 0;
    AudioExportOptionsComponent restored(std::nullopt, obsolete);
    selected = restored.settings(error);
    expect(selected && selected->audio.flacCompressionLevel == 1
               && control<juce::ComboBox>(restored, "export.flacCompression").getSelectedId() == 2,
           "An obsolete level-zero setting is visibly replaced with supported FLAC level 1.");
}

void advertisedRatesMatchTheEncoder()
{
    AudioExportOptionsComponent component(std::nullopt, {});
    for (const auto format : { AudioExportFormat::wav, AudioExportFormat::aiff,
                               AudioExportFormat::flac, AudioExportFormat::oggVorbis,
                               AudioExportFormat::mp3 })
    {
        selectFormat(component, format);
        const auto rates = studio::AudioExport::sampleRates(format);
        auto& combo = control<juce::ComboBox>(component, "export.sampleRate");
        expect(combo.getNumItems() == static_cast<int>(rates.size()),
               "The dialog advertises exactly the selected encoder's supported sample rates.");
        for (size_t index = 0; index < rates.size(); ++index)
        {
            combo.setSelectedItemIndex(static_cast<int>(index), juce::sendNotificationSync);
            juce::String error;
            const auto selected = component.settings(error);
            expect(selected && std::abs(selected->audio.sampleRate - rates[index]) < 1.0e-9,
                   "Every advertised rate maps to the exact supported numeric value.");
        }
    }
}

void presetsReplaceAllAudioFields()
{
    MixExportSettings initial;
    initial.audio.normalizePeak = true;
    initial.audio.normalizePeakDbfs = -6.0;
    initial.audio.mp3BitrateMode = studio::Mp3BitrateMode::variable;
    initial.audio.mp3VbrQuality = 9;
    initial.audio.oggQuality = 10;
    initial.audio.flacCompressionLevel = 8;
    initial.range = MixExportRange::custom;
    initial.startSeconds = 1.0;
    initial.endSeconds = 6.0;
    initial.tailSeconds = 2.0;
    initial.fadeOutSeconds = 1.0;
    AudioExportOptionsComponent component(markerProject(), initial);

    select(component, "export.preset", 2);
    juce::String error;
    auto selected = component.settings(error);
    expect(selected && selected->audio.format == AudioExportFormat::wav
               && selected->audio.sampleRate == 44100.0
               && selected->audio.bitDepth == 16
               && selected->audio.channels == 2
               && selected->audio.dither == AudioExportDither::tpdf
               && !selected->audio.normalizePeak
               && std::abs(selected->audio.normalizePeakDbfs + 1.0) < 1.0e-9
               && selected->audio.mp3BitrateMode == studio::Mp3BitrateMode::constant
               && selected->audio.mp3VbrQuality == 2
               && selected->audio.oggQuality == 6
               && selected->audio.flacCompressionLevel == 5,
           "The Audio CD preset replaces every audio setting, including inactive codec/level values.");
    expect(selected && selected->range == MixExportRange::custom
               && selected->startSeconds == 1.0 && selected->endSeconds == 6.0
               && selected->tailSeconds == 2.0 && selected->fadeOutSeconds == 1.0,
           "Audio presets leave the user's range, tail and fades unchanged.");

    select(component, "export.preset", 4);
    selected = component.settings(error);
    expect(selected && selected->audio.format == AudioExportFormat::mp3
               && selected->audio.sampleRate == 44100.0
               && selected->audio.mp3BitrateMode == studio::Mp3BitrateMode::constant
               && selected->audio.mp3BitrateKbps == 320
               && selected->audio.bitDepth == 32
               && selected->audio.dither == AudioExportDither::none,
           "The MP3 reference preset is truthful about CBR, bitrate, rate and encoder input.");
    select(component, "export.preset", 5);
    selected = component.settings(error);
    expect(selected && selected->audio.format == AudioExportFormat::flac
               && selected->audio.sampleRate == 48000.0
               && selected->audio.bitDepth == 24,
           "The lossless preset selects FLAC 48 kHz/24-bit.");
    select(component, "export.preset", 1);
    selected = component.settings(error);
    expect(selected && selected->audio.normalizePeak
               && std::abs(selected->audio.normalizePeakDbfs + 6.0) < 1.0e-9
               && selected->audio.mp3VbrQuality == 9
               && selected->audio.oggQuality == 10
               && selected->audio.flacCompressionLevel == 8,
           "Current settings restores the complete original audio snapshot.");
}

void markerRangesUseStableSnapshots()
{
    auto project = markerProject();
    AudioExportOptionsComponent component(project, {});
    project.markers.clear();
    select(component, "export.range", 3);
    juce::String error;
    auto selected = component.settings(error);
    expect(selected && selected->range == MixExportRange::markers
               && selected->startMarkerId == "marker-start"
               && selected->endMarkerId == "marker-end"
               && selected->startSeconds == 1.0
               && selected->endSeconds == 6.0
               && selected->tailSeconds == 0.0,
           "Marker ranges default to first/last distinct IDs in the copied snapshot, with no tail.");
    expect(control<juce::ComboBox>(component, "export.startMarker").getText().contains("1.000")
               && control<juce::ComboBox>(component, "export.endMarker").getText().contains("6.000"),
           "Repeated marker names remain distinguishable by displayed time.");

    select(component, "export.startMarker", 3);
    select(component, "export.endMarker", 1);
    expect(!component.settings(error) && error.isNotEmpty(),
           "Reversed named marker ranges fail validation.");
    select(component, "export.endMarker", 3);
    expect(!component.settings(error),
           "Selecting the same marker for both boundaries fails validation.");

    MixExportSettings missing;
    missing.range = MixExportRange::markers;
    missing.startMarkerId = "deleted-marker";
    missing.endMarkerId = "marker-end";
    AudioExportOptionsComponent missingComponent(markerProject(), missing);
    expect(control<juce::ComboBox>(missingComponent, "export.startMarker").getSelectedId() == 0
               && !missingComponent.settings(error),
           "A deleted marker ID stays explicitly unselected instead of rebinding to another marker.");
    select(missingComponent, "export.startMarker", 1);
    expect(missingComponent.settings(error).has_value(),
           "Choosing an available replacement marker resolves the stale selection.");

    auto singleMarker = markerProject();
    singleMarker.markers.resize(1);
    AudioExportOptionsComponent insufficient(singleMarker, {});
    expect(!control<juce::ComboBox>(insufficient, "export.range").isItemEnabled(3)
               && control<juce::Label>(insufficient, "export.markerHint").getText().contains("two"),
           "Fewer than two markers disables marker export and explains how to add them.");
    select(insufficient, "export.range", 3);
    expect(!insufficient.settings(error),
           "Programmatically selecting an unavailable marker range is also rejected.");

    auto identicalLabels = markerProject();
    identicalLabels.markers = {
        { "first-boundary", "Boundary", 1.0 },
        { "second-boundary", "Boundary", 1.0 },
        { "end-boundary", "Boundary", 6.0 }
    };
    MixExportSettings duplicateSelection;
    duplicateSelection.range = MixExportRange::markers;
    duplicateSelection.startMarkerId = "second-boundary";
    duplicateSelection.endMarkerId = "end-boundary";
    AudioExportOptionsComponent duplicates(identicalLabels, duplicateSelection);
    const auto& selector = control<juce::ComboBox>(duplicates, "export.startMarker");
    selected = duplicates.settings(error);
    expect(selected && selected->startMarkerId == "second-boundary"
               && selector.getSelectedId() == 2
               && selector.getItemText(0) != selector.getItemText(1),
           "Even equal-name, equal-time markers have distinct labels and preserve the selected stable ID.");
    identicalLabels.markers[0].timeSeconds = 1.0001;
    identicalLabels.markers[1].timeSeconds = 1.0002;
    AudioExportOptionsComponent roundedLabels(identicalLabels, duplicateSelection);
    const auto& roundedSelector = control<juce::ComboBox>(roundedLabels, "export.startMarker");
    selected = roundedLabels.settings(error);
    expect(selected && selected->startMarkerId == "second-boundary"
               && roundedSelector.getItemText(0) != roundedSelector.getItemText(1),
           "Different marker times that round to the same displayed timestamp remain distinguishable.");
}

void exactRangesAndTails()
{
    AudioExportOptionsComponent component(markerProject(), {});
    juce::String error;
    auto selected = component.settings(error);
    expect(selected && !selected->tailSeconds,
           "The default whole-project export retains automatic effect tails.");

    select(component, "export.range", 2);
    selected = component.settings(error);
    expect(selected && selected->startSeconds == 2.0 && selected->endSeconds == 5.0
               && selected->tailSeconds == 0.0,
           "Selecting the loop for the first time uses its exact boundaries without a tail.");
    select(component, "export.tailMode", 1);
    select(component, "export.range", 4);
    selected = component.settings(error);
    expect(selected && selected->tailSeconds == 0.0,
           "Selecting custom seconds for the first time also defaults to no tail.");
    select(component, "export.range", 2);
    selected = component.settings(error);
    expect(selected && !selected->tailSeconds,
           "Revisiting a range preserves the tail mode explicitly chosen for that range.");

    select(component, "export.range", 4);
    edit(component, "export.startSeconds", "1");
    edit(component, "export.endSeconds", "2.5");
    select(component, "export.tailMode", 3);
    edit(component, "export.tailSeconds", "2");
    edit(component, "export.fadeInSeconds", "3.5");
    edit(component, "export.fadeOutSeconds", "3.5");
    selected = component.settings(error);
    expect(selected && selected->tailSeconds == 2.0
               && selected->fadeInSeconds == 3.5 && selected->fadeOutSeconds == 3.5,
           "Fades may span the base duration plus a known fixed tail.");
    edit(component, "export.fadeOutSeconds", "3.501");
    expect(!component.settings(error),
           "A fade longer than the known export duration is rejected.");
    edit(component, "export.fadeInSeconds", "0");
    edit(component, "export.fadeOutSeconds", "0");
    edit(component, "export.tailSeconds", "30.1");
    expect(!component.settings(error), "Custom tails above 30 seconds are rejected.");
    edit(component, "export.tailSeconds", "-0.1");
    expect(!component.settings(error), "Negative custom tails are rejected.");
    select(component, "export.tailMode", 1);
    edit(component, "export.fadeInSeconds", "2");
    expect(!component.settings(error),
           "Automatic, not-yet-known tail length cannot justify a fade beyond the base duration.");

    edit(component, "export.fadeInSeconds", "0");
    edit(component, "export.startSeconds", "2.5");
    expect(!component.settings(error), "Equal custom range boundaries are rejected.");
    edit(component, "export.startSeconds", "-1");
    expect(!component.settings(error), "Negative custom range starts are rejected.");
    edit(component, "export.startSeconds", "2.499999999");
    expect(!component.settings(error),
           "A custom range that rounds to no output samples is rejected by the shared resolver.");
}

void numericValidationAndMasteringScope()
{
    MixExportSettings initial;
    initial.range = MixExportRange::markers;
    initial.startMarkerId = "missing";
    initial.tailSeconds = -10.0;
    initial.fadeInSeconds = -5.0;
    AudioExportOptionsComponent component(std::nullopt, initial);
    juce::String error;
    const auto selected = component.settings(error);
    expect(selected && selected->range == MixExportRange::entireProject
               && !selected->tailSeconds && selected->fadeInSeconds == 0.0,
           "Mastering returns only format/processing choices without hidden mix-only settings.");
    for (const auto* id : { "export.range", "export.startMarker", "export.endMarker",
                            "export.startSeconds", "export.endSeconds", "export.tailMode",
                            "export.tailSeconds", "export.fadeInSeconds", "export.fadeOutSeconds",
                            "export.rangeSummary", "export.markerHint" })
    {
        auto* found = findControl(component, id);
        expect(found != nullptr && !found->isVisible(),
               "Mastering hides every range, marker, tail and fade control.");
    }
    control<juce::ToggleButton>(component, "export.normalize")
        .setToggleState(true, juce::sendNotificationSync);
    for (const auto* invalid : { "", "-1junk", "NaN", "Inf", "-1,5", "1e999", "1 2", "0x1" })
    {
        edit(component, "export.normalizeTarget", invalid);
        expect(!component.settings(error) && error.isNotEmpty(),
               "Numeric fields reject empty, non-finite, overflow and partial-string values.");
    }
    for (const auto* valid : { "-24", "0", " -1.25 ", "-1e-1" })
    {
        edit(component, "export.normalizeTarget", valid);
        expect(component.settings(error).has_value(),
               "Normalization accepts complete finite decimal/scientific input at valid bounds.");
    }
    for (const auto* invalid : { "-24.001", "0.001" })
    {
        edit(component, "export.normalizeTarget", invalid);
        expect(!component.settings(error),
               "Normalization is limited to -24 through 0 dBFS.");
    }
    edit(component, "export.normalizeTarget", "invalid");
    control<juce::ToggleButton>(component, "export.normalize")
        .setToggleState(false, juce::sendNotificationSync);
    expect(component.settings(error).has_value()
               && !control<juce::TextEditor>(component, "export.normalizeTarget").isEnabled(),
           "Disabling normalization makes an inactive, invalid target irrelevant to export.");
    control<juce::ToggleButton>(component, "export.normalize")
        .setToggleState(true, juce::sendNotificationSync);
    edit(component, "export.normalizeTarget", "0.001");

    bool exported = false;
    component.onExport = [&](auto) { exported = true; };
    component.setVisible(true);
    component.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(!exported && component.isVisible()
               && control<juce::Label>(component, "export.validation").getText().contains("between"),
           "Return on invalid settings shows an inline error and leaves the dialog open.");
    component.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    expect(!exported && !component.isVisible(),
           "Escape cancels without invoking the export callback.");
}

void scrollLayoutKeepsControlsReachable()
{
    AudioExportOptionsComponent component(markerProject(), {});
    component.setSize(360, 380);
    selectFormat(component, AudioExportFormat::mp3);
    select(component, "export.range", 4);
    select(component, "export.tailMode", 3);
    const auto& viewport = control<juce::Viewport>(component, "export.viewport");
    const auto* content = viewport.getViewedComponent();
    expect(content != nullptr && content->getHeight() > viewport.getHeight(),
           "A compact desktop uses a scrollable form instead of hiding controls.");
    if (content != nullptr)
    {
        for (auto* child : content->getChildren())
            if (child->isVisible())
                expect(!child->getBounds().isEmpty()
                           && content->getLocalBounds().contains(child->getBounds()),
                       "Every visible form child has usable bounds inside the scroll content.");
    }
    for (const auto* id : { "export.submit", "export.cancel" })
    {
        const auto& button = control<juce::TextButton>(component, id);
        expect(component.getLocalBounds().contains(button.getBounds())
                   && button.getY() >= viewport.getBottom()
                   && button.getWantsKeyboardFocus(),
               "Export and Cancel remain below the scrolling content and keyboard reachable.");
    }
    expect(control<juce::ComboBox>(component, "export.format").getTitle().isNotEmpty()
               && control<juce::ComboBox>(component, "export.format").getTooltip().isNotEmpty()
               && control<juce::TextEditor>(component, "export.startSeconds").getTitle().isNotEmpty(),
           "Editable controls expose accessible titles and explanatory tooltips.");
}

void primaryActionContrast()
{
    AudioExportOptionsComponent component(std::nullopt, {});
    const auto& button = control<juce::TextButton>(component, "export.submit");
    const auto foreground = button.findColour(juce::TextButton::textColourOffId);
    const auto background = button.findColour(juce::TextButton::buttonColourId);
    const auto luminance = [](juce::Colour colour)
    {
        const auto linear = [](double channel)
        {
            return channel <= 0.04045 ? channel / 12.92
                                     : std::pow((channel + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(colour.getFloatRed())
            + 0.7152 * linear(colour.getFloatGreen())
            + 0.0722 * linear(colour.getFloatBlue());
    };
    for (const auto state : std::array {
             background,
             background.brighter(0.08f),
             background.darker(0.12f),
             background.brighter(0.08f).darker(0.12f) })
    {
        const auto text = luminance(foreground);
        const auto fill = luminance(state);
        const auto contrast = (std::max(text, fill) + 0.05) / (std::min(text, fill) + 0.05);
        expect(contrast >= 4.5,
               "Export text meets 4.5:1 contrast in the theme's idle, hover and pressed states.");
    }
}
}

void audioExportOptionsTests()
{
    try
    {
        defaultsAndFormatChanges();
        codecQualityControls();
        advertisedRatesMatchTheEncoder();
        presetsReplaceAllAudioFields();
        markerRangesUseStableSnapshots();
        exactRangesAndTails();
        numericValidationAndMasteringScope();
        scrollLayoutKeepsControlsReachable();
        primaryActionContrast();
    }
    catch (const std::exception& error)
    {
        expect(false, error.what());
    }
}

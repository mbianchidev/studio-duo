#include "MasteringWorkspaceComponent.h"

#include "StudioTheme.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>

namespace studio
{
namespace
{
void setupLabel(juce::Component& parent,
                juce::Label& label,
                const juce::String& text,
                bool heading = false)
{
    parent.addAndMakeVisible(label);
    label.setText(text, juce::dontSendNotification);
    label.setColour(
        juce::Label::textColourId,
        juce::Colour(
            heading ? StudioColours::text
                    : StudioColours::secondaryText));
    if (heading)
        label.setFont(
            juce::Font(
                juce::FontOptions(15.0f, juce::Font::bold)));
}

juce::String measurementText(
    const MasteringMeasurement& measurement)
{
    const auto value = [](const std::optional<double>& number,
                          const juce::String& suffix)
    {
        return number.has_value()
            ? juce::String(*number, 2) + suffix
            : juce::String("n/a");
    };
    return "Integrated "
        + value(measurement.integratedLoudnessLufs, " LUFS")
        + "    LRA "
        + value(measurement.loudnessRangeLu, " LU")
        + "    True peak "
        + juce::String(measurement.truePeakDbtp, 2)
        + " dBTP    Sample peak "
        + juce::String(measurement.samplePeakDbfs, 2)
        + " dBFS    Correlation "
        + juce::String(measurement.correlation, 2);
}

juce::String albumFingerprint(const MasteringAlbum& album)
{
    return juce::SHA256(
        juce::JSON::toString(album.toVar(), false).toUTF8())
        .toHexString();
}

juce::String projectFingerprint(const Project& project)
{
    return juce::SHA256(
        juce::JSON::toString(project.toVar(), false).toUTF8())
        .toHexString();
}
}

MasteringWorkspaceComponent::MasteringWorkspaceComponent()
{
    setOpaque(true);
    setupLabel(*this, titleLabel, "MASTERING & RELEASE", true);
    titleLabel.setFont(
        juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    setupLabel(*this, albumSectionLabel, "ALBUM METADATA", true);
    setupLabel(*this, albumTitleLabel, "Title");
    setupLabel(*this, albumArtistLabel, "Artist");
    setupLabel(*this, albumSongwriterLabel, "Songwriter");
    setupLabel(*this, labelNameLabel, "Label");
    setupLabel(*this, catalogLabel, "Catalog #");
    setupLabel(*this, mcnLabel, "MCN / EAN");
    setupLabel(*this, releaseDateLabel, "Release date");
    setupLabel(*this, genreLabel, "Genre");
    setupLabel(*this, outputGainLabel, "Output gain dB");
    setupLabel(*this, trackSectionLabel, "SELECTED SONG", true);
    setupLabel(*this, trackTitleLabel, "Title");
    setupLabel(*this, trackArtistLabel, "Artist");
    setupLabel(*this, trackSongwriterLabel, "Songwriter");
    setupLabel(*this, isrcLabel, "ISRC");
    setupLabel(*this, sourceLabel, "Source mix");
    setupLabel(*this, gapLabel, "Gap before s");
    setupLabel(*this, overlapLabel, "Overlap s");
    setupLabel(*this, fadeInLabel, "Fade in s");
    setupLabel(*this, fadeOutLabel, "Fade out s");
    setupLabel(*this, trackGainLabel, "Track gain dB");

    for (auto* editor : {
             &albumTitleEditor,
             &albumArtistEditor,
             &albumSongwriterEditor,
             &labelNameEditor,
             &catalogEditor,
             &mcnEditor,
             &releaseDateEditor,
             &genreEditor,
             &outputGainEditor })
        configureEditor(*editor, "Edit album release metadata");
    for (auto* editor : {
             &trackTitleEditor,
             &trackArtistEditor,
             &trackSongwriterEditor,
             &isrcEditor,
             &gapEditor,
             &overlapEditor,
             &fadeInEditor,
             &fadeOutEditor,
             &trackGainEditor })
        configureEditor(*editor, "Edit selected mastering song");

    const auto albumCommit = [this] { commitAlbumFields(); };
    for (auto* editor : {
             &albumTitleEditor,
             &albumArtistEditor,
             &albumSongwriterEditor,
             &labelNameEditor,
             &catalogEditor,
             &mcnEditor,
             &releaseDateEditor,
             &genreEditor,
             &outputGainEditor })
    {
        editor->onFocusLost = albumCommit;
        editor->onReturnKey = albumCommit;
    }
    const auto trackCommit = [this] { commitTrackFields(); };
    for (auto* editor : {
             &trackTitleEditor,
             &trackArtistEditor,
             &trackSongwriterEditor,
             &isrcEditor,
             &gapEditor,
             &overlapEditor,
             &fadeInEditor,
             &fadeOutEditor,
             &trackGainEditor })
    {
        editor->onFocusLost = trackCommit;
        editor->onReturnKey = trackCommit;
    }

    addAndMakeVisible(trackList);
    trackList.setRowHeight(42);
    trackList.setColour(
        juce::ListBox::backgroundColourId,
        juce::Colour(StudioColours::panel));
    addAndMakeVisible(sourceSelector);
    sourceSelector.onChange = [this]
    {
        if (refreshing || sourceSelector.getSelectedItemIndex() < 0)
            return;
        const auto index = selectedTrackIndex;
        const auto sourceIndex = sourceSelector.getSelectedItemIndex();
        editAlbum([index, sourceIndex](auto& album)
        {
            if (index < 0
                || index >= static_cast<int>(album.tracks.size()))
                return;
            auto& track =
                album.tracks[static_cast<std::size_t>(index)];
            if (sourceIndex >= 0
                && sourceIndex < static_cast<int>(track.sources.size()))
                track.selectedSourceId =
                    track.sources[static_cast<std::size_t>(sourceIndex)].id;
        });
    };

    for (auto* button : {
             &addSongButton,
             &addAlternateButton,
             &addReferenceButton,
             &moveUpButton,
             &moveDownButton,
             &removeButton,
             &analyseButton,
             &exportButton,
             &ddpButton,
             &portableButton,
             &repairButton })
    {
        addAndMakeVisible(*button);
        button->setWantsKeyboardFocus(false);
    }
    addSongButton.onClick = [this] { chooseAudio(AddMode::song); };
    addAlternateButton.onClick =
        [this] { chooseAudio(AddMode::alternate); };
    addReferenceButton.onClick =
        [this] { chooseAudio(AddMode::reference); };
    moveUpButton.onClick = [this] { moveSelectedTrack(-1); };
    moveDownButton.onClick = [this] { moveSelectedTrack(1); };
    removeButton.onClick = [this] { removeSelectedTrack(); };
    analyseButton.onClick = [this] { analyseAlbum(); };
    exportButton.onClick = [this] { showExportMenu(); };
    ddpButton.onClick = [this] { beginDdpExport(); };
    portableButton.onClick = [this] { beginPortableCopy(); };
    repairButton.onClick = [this] { beginRepair(); };

    addAndMakeVisible(analysisLabel);
    analysisLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::secondaryText));
    analysisLabel.setJustificationType(
        juce::Justification::centredLeft);
    analysisLabel.setMinimumHorizontalScale(0.65f);
    refresh();
}

void MasteringWorkspaceComponent::setProject(
    Project* projectToUse)
{
    project = projectToUse;
    refresh();
}

void MasteringWorkspaceComponent::setProjectPackage(
    const juce::File& package)
{
    projectPackage = package;
}

void MasteringWorkspaceComponent::configureEditor(
    juce::TextEditor& editor,
    const juce::String& tooltip)
{
    addAndMakeVisible(editor);
    editor.setTooltip(tooltip);
    editor.setSelectAllWhenFocused(true);
}

int MasteringWorkspaceComponent::getNumRows()
{
    return project != nullptr
        ? static_cast<int>(project->mastering.tracks.size())
        : 0;
}

void MasteringWorkspaceComponent::paintListBoxItem(
    int row,
    juce::Graphics& graphics,
    int width,
    int height,
    bool selected)
{
    if (project == nullptr
        || row < 0
        || row >= static_cast<int>(project->mastering.tracks.size()))
        return;
    if (selected)
    {
        graphics.setColour(
            juce::Colour(StudioColours::orange).withAlpha(0.22f));
        graphics.fillRect(0, 0, width, height);
    }
    const auto& track =
        project->mastering.tracks[static_cast<std::size_t>(row)];
    const auto placements = project->mastering.placements();
    graphics.setColour(juce::Colour(StudioColours::text));
    graphics.setFont(
        juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    graphics.drawText(
        juce::String(row + 1).paddedLeft('0', 2)
            + "  "
            + track.title,
        10,
        3,
        width - 20,
        20,
        juce::Justification::centredLeft);
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(11.0f);
    const auto* source = track.selectedSource();
    graphics.drawText(
        (source != nullptr ? source->name : juce::String("No source"))
            + "  -  "
            + (row < static_cast<int>(placements.size())
                   ? juce::String(
                         placements[static_cast<std::size_t>(row)]
                             .startSeconds,
                         2)
                         + " s"
                   : juce::String("n/a")),
        10,
        22,
        width - 20,
        17,
        juce::Justification::centredLeft);
}

void MasteringWorkspaceComponent::selectedRowsChanged(int row)
{
    selectedTrackIndex = row;
    updateTrackEditors();
}

MasteringTrack* MasteringWorkspaceComponent::selectedTrack() noexcept
{
    return project != nullptr
            && selectedTrackIndex >= 0
            && selectedTrackIndex
                < static_cast<int>(project->mastering.tracks.size())
        ? &project->mastering.tracks[
              static_cast<std::size_t>(selectedTrackIndex)]
        : nullptr;
}

const MasteringTrack*
MasteringWorkspaceComponent::selectedTrack() const noexcept
{
    return const_cast<MasteringWorkspaceComponent*>(this)
        ->selectedTrack();
}

void MasteringWorkspaceComponent::refresh()
{
    refreshing = true;
    if (project != nullptr)
    {
        const auto& album = project->mastering;
        albumTitleEditor.setText(album.title, false);
        albumArtistEditor.setText(album.artist, false);
        albumSongwriterEditor.setText(album.songwriter, false);
        labelNameEditor.setText(album.label, false);
        catalogEditor.setText(album.catalogNumber, false);
        mcnEditor.setText(album.mcn, false);
        releaseDateEditor.setText(album.releaseDate, false);
        genreEditor.setText(album.genre, false);
        outputGainEditor.setText(
            juce::String(album.outputGainDecibels, 2),
            false);
        if (album.tracks.empty())
            selectedTrackIndex = -1;
        else
            selectedTrackIndex = juce::jlimit(
                0,
                static_cast<int>(album.tracks.size()) - 1,
                selectedTrackIndex < 0 ? 0 : selectedTrackIndex);
    }
    else
    {
        selectedTrackIndex = -1;
    }
    trackList.updateContent();
    trackList.selectRow(selectedTrackIndex);
    updateTrackEditors();
    refreshing = false;
    repaint();
}

void MasteringWorkspaceComponent::updateTrackEditors()
{
    const auto* track = selectedTrack();
    const auto enabled = track != nullptr;
    for (juce::Component* component : {
             static_cast<juce::Component*>(&trackTitleEditor),
             static_cast<juce::Component*>(&trackArtistEditor),
             static_cast<juce::Component*>(&trackSongwriterEditor),
             static_cast<juce::Component*>(&isrcEditor),
             static_cast<juce::Component*>(&sourceSelector),
             static_cast<juce::Component*>(&gapEditor),
             static_cast<juce::Component*>(&overlapEditor),
             static_cast<juce::Component*>(&fadeInEditor),
             static_cast<juce::Component*>(&fadeOutEditor),
             static_cast<juce::Component*>(&trackGainEditor),
             static_cast<juce::Component*>(&addAlternateButton),
             static_cast<juce::Component*>(&moveUpButton),
             static_cast<juce::Component*>(&moveDownButton),
             static_cast<juce::Component*>(&removeButton) })
        component->setEnabled(enabled);
    sourceSelector.clear(juce::dontSendNotification);
    if (track == nullptr)
    {
        for (auto* editor : {
                 &trackTitleEditor,
                 &trackArtistEditor,
                 &trackSongwriterEditor,
                 &isrcEditor,
                 &gapEditor,
                 &overlapEditor,
                 &fadeInEditor,
                 &fadeOutEditor,
                 &trackGainEditor })
            editor->clear();
        return;
    }
    trackTitleEditor.setText(track->title, false);
    trackArtistEditor.setText(track->artist, false);
    trackSongwriterEditor.setText(track->songwriter, false);
    isrcEditor.setText(track->isrc, false);
    gapEditor.setText(juce::String(track->gapBeforeSeconds, 3), false);
    overlapEditor.setText(
        juce::String(track->overlapPreviousSeconds, 3),
        false);
    fadeInEditor.setText(
        juce::String(track->fadeInSeconds, 3),
        false);
    fadeOutEditor.setText(
        juce::String(track->fadeOutSeconds, 3),
        false);
    trackGainEditor.setText(
        juce::String(track->gainDecibels, 2),
        false);
    auto selectedSource = -1;
    for (std::size_t index = 0; index < track->sources.size(); ++index)
    {
        sourceSelector.addItem(
            track->sources[index].name,
            static_cast<int>(index + 1));
        if (track->sources[index].id == track->selectedSourceId)
            selectedSource = static_cast<int>(index);
    }
    sourceSelector.setSelectedItemIndex(
        selectedSource,
        juce::dontSendNotification);
}

void MasteringWorkspaceComponent::editAlbum(
    const std::function<void(MasteringAlbum&)>& edit)
{
    if (project == nullptr)
        return;
    const auto before = project->mastering;
    auto after = before;
    edit(after);
    if (onAlbumEdited)
        onAlbumEdited(before, after);
    else
        project->mastering = std::move(after);
    refresh();
}

void MasteringWorkspaceComponent::commitAlbumFields()
{
    if (refreshing)
        return;
    editAlbum([this](auto& album)
    {
        album.title = albumTitleEditor.getText().trim();
        album.artist = albumArtistEditor.getText().trim();
        album.songwriter = albumSongwriterEditor.getText().trim();
        album.label = labelNameEditor.getText().trim();
        album.catalogNumber = catalogEditor.getText().trim();
        album.mcn = mcnEditor.getText().retainCharacters("0123456789");
        album.releaseDate = releaseDateEditor.getText().trim();
        album.genre = genreEditor.getText().trim();
        album.outputGainDecibels =
            outputGainEditor.getText().getDoubleValue();
    });
}

void MasteringWorkspaceComponent::commitTrackFields()
{
    if (refreshing || selectedTrack() == nullptr)
        return;
    const auto index = selectedTrackIndex;
    editAlbum([this, index](auto& album)
    {
        if (index < 0
            || index >= static_cast<int>(album.tracks.size()))
            return;
        auto& track =
            album.tracks[static_cast<std::size_t>(index)];
        track.title = trackTitleEditor.getText().trim();
        track.artist = trackArtistEditor.getText().trim();
        track.songwriter = trackSongwriterEditor.getText().trim();
        track.isrc = isrcEditor.getText()
                         .toUpperCase()
                         .removeCharacters(" -");
        track.gapBeforeSeconds =
            gapEditor.getText().getDoubleValue();
        track.overlapPreviousSeconds =
            overlapEditor.getText().getDoubleValue();
        track.fadeInSeconds =
            fadeInEditor.getText().getDoubleValue();
        track.fadeOutSeconds =
            fadeOutEditor.getText().getDoubleValue();
        track.gainDecibels =
            trackGainEditor.getText().getDoubleValue();
    });
}

void MasteringWorkspaceComponent::chooseAudio(AddMode mode)
{
    chooser = std::make_unique<juce::FileChooser>(
        mode == AddMode::song
            ? "Add song master"
            : mode == AddMode::alternate
                ? "Add alternate source mix"
                : "Add mastering reference",
        juce::File::getSpecialLocation(
            juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg",
        true,
        false,
        this);
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this),
         mode](const juce::FileChooser& completed)
        {
            if (safe == nullptr)
                return;
            const auto file = completed.getResult();
            if (file != juce::File())
                safe->addAudio(file, mode);
            safe->chooser.reset();
        });
}

void MasteringWorkspaceComponent::addAudio(
    const juce::File& file,
    AddMode mode)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(
        formats.createReaderFor(file));
    if (reader == nullptr
        || reader->sampleRate <= 0.0
        || reader->lengthInSamples <= 0)
    {
        setStatus("Could not read " + file.getFullPathName(), true);
        return;
    }
    const auto hash = juce::SHA256(file).toHexString();
    if (mode == AddMode::reference)
    {
        editAlbum([&](auto& album)
        {
            MasteringReference reference;
            reference.name = file.getFileNameWithoutExtension();
            reference.file = file;
            reference.sourceHash = hash;
            album.references.push_back(std::move(reference));
        });
        setStatus("Added reference; references bypass album gain and release rendering.");
        return;
    }

    MasteringSourceMix source;
    source.name = mode == AddMode::alternate
        ? file.getFileNameWithoutExtension()
        : "Main";
    source.file = file;
    source.sourceHash = hash;
    source.durationSeconds =
        static_cast<double>(reader->lengthInSamples)
        / reader->sampleRate;
    if (mode == AddMode::song)
    {
        editAlbum([&, sourceToAdd = std::move(source)](
                      auto& album) mutable
        {
            MasteringTrack track;
            track.title = file.getFileNameWithoutExtension();
            track.artist = album.artist;
            track.songwriter = album.songwriter;
            track.gapBeforeSeconds = album.tracks.empty() ? 2.0 : 0.0;
            track.selectedSourceId = sourceToAdd.id;
            track.sources.push_back(std::move(sourceToAdd));
            album.tracks.push_back(std::move(track));
            selectedTrackIndex =
                static_cast<int>(album.tracks.size()) - 1;
        });
        setStatus("Added song to the mastering sequence.");
        return;
    }

    const auto trackIndex = selectedTrackIndex;
    editAlbum([trackIndex, sourceToAdd = std::move(source)](
                  auto& album) mutable
    {
        if (trackIndex < 0
            || trackIndex >= static_cast<int>(album.tracks.size()))
            return;
        album.tracks[static_cast<std::size_t>(trackIndex)]
            .sources.push_back(std::move(sourceToAdd));
    });
    setStatus("Added alternate source mix.");
}

void MasteringWorkspaceComponent::removeSelectedTrack()
{
    const auto index = selectedTrackIndex;
    editAlbum([index](auto& album)
    {
        if (index >= 0
            && index < static_cast<int>(album.tracks.size()))
            album.tracks.erase(album.tracks.begin() + index);
    });
    selectedTrackIndex = juce::jmin(
        selectedTrackIndex,
        getNumRows() - 1);
    refresh();
}

void MasteringWorkspaceComponent::moveSelectedTrack(int offset)
{
    const auto source = selectedTrackIndex;
    const auto destination = source + offset;
    if (project == nullptr
        || source < 0
        || destination < 0
        || destination
            >= static_cast<int>(project->mastering.tracks.size()))
        return;
    editAlbum([source, destination](auto& album)
    {
        std::swap(
            album.tracks[static_cast<std::size_t>(source)],
            album.tracks[static_cast<std::size_t>(destination)]);
    });
    selectedTrackIndex = destination;
    refresh();
}

void MasteringWorkspaceComponent::analyseAlbum()
{
    if (project == nullptr)
        return;
    if (!beginOperation("Analyzing mastering album..."))
        return;
    const auto album = project->mastering;
    const auto projectId = project->id;
    const auto fingerprint = albumFingerprint(album);
    const auto safe =
        juce::Component::SafePointer<MasteringWorkspaceComponent>(this);
    backgroundJobs.addJob([safe, album, projectId, fingerprint]
    {
        juce::String error;
        auto render = MasteringEngine::renderAlbum(
            album,
            48000.0,
            error);
        std::optional<MasteringMeasurement> measurement;
        if (render.has_value())
            measurement = MasteringEngine::analyse(
                render->audio,
                render->sampleRate);
        juce::MessageManager::callAsync(
            [safe, measurement, error, projectId, fingerprint]
            {
                if (safe == nullptr)
                    return;
                safe->finishOperation();
                if (!measurement.has_value())
                {
                    safe->setStatus(error, true);
                    return;
                }
                if (safe->project == nullptr
                    || safe->project->id != projectId
                    || albumFingerprint(safe->project->mastering)
                        != fingerprint)
                {
                    safe->setStatus(
                        "Analysis completed, but the project changed before results could be displayed.",
                        true);
                    return;
                }
                safe->analysisLabel.setText(
                    measurementText(*measurement),
                    juce::dontSendNotification);
                safe->setStatus("Mastering analysis completed.");
            });
    });
}

void MasteringWorkspaceComponent::showExportMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "WAV master - 48 kHz / 24-bit");
    menu.addItem(2, "FLAC master - 48 kHz / 24-bit");
    menu.addItem(3, "Ogg reference - 48 kHz");
    menu.addItem(4, "Audio CD WAV - 44.1 kHz / 16-bit TPDF");
    menu.showMenuAsync(
        juce::PopupMenu::Options()
            .withTargetComponent(&exportButton),
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this)](int choice)
        {
            if (safe == nullptr || choice == 0)
                return;
            if (choice == 1 || choice == 4)
                safe->beginExport(MasteringExportFormat::wav);
            else if (choice == 2)
                safe->beginExport(MasteringExportFormat::flac);
            else
                safe->beginExport(
                    MasteringExportFormat::oggReference);
            safe->exportButton.getProperties().set(
                "cdPreset",
                choice == 4);
        });
}

void MasteringWorkspaceComponent::beginExport(
    MasteringExportFormat format)
{
    const juce::String extension =
        format == MasteringExportFormat::wav
        ? ".wav"
        : format == MasteringExportFormat::flac ? ".flac" : ".ogg";
    chooser = std::make_unique<juce::FileChooser>(
        "Export mastering release",
        juce::File::getSpecialLocation(
            juce::File::userMusicDirectory)
            .getChildFile(
                (project != nullptr
                     && project->mastering.title.isNotEmpty()
                     ? project->mastering.title
                     : juce::String("Studio-Duo-Master"))
                + extension),
        "*" + extension,
        true,
        false,
        this);
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this),
         format](const juce::FileChooser& completed)
        {
            if (safe == nullptr)
                return;
            const auto file = completed.getResult();
            if (file != juce::File())
            {
                MasteringExportSettings settings;
                settings.format = format;
                settings.sampleRate = 48000.0;
                settings.bitDepth = 24;
                settings.distributionPresetId =
                    "streaming-balanced";
                if (format
                    == MasteringExportFormat::oggReference)
                    settings.bitDepth = 32;
                if (static_cast<bool>(
                        safe->exportButton.getProperties()
                            .getWithDefault("cdPreset", false)))
                {
                    settings.sampleRate = 44100.0;
                    settings.bitDepth = 16;
                    settings.dither = MasteringDither::tpdf;
                    settings.distributionPresetId = "cd";
                }
                safe->exportTo(file, settings);
            }
            safe->chooser.reset();
        });
}

void MasteringWorkspaceComponent::exportTo(
    const juce::File& destination,
    MasteringExportSettings settings)
{
    if (project == nullptr)
        return;
    if (!beginOperation(
            "Rendering " + destination.getFileName() + "..."))
        return;
    const auto album = project->mastering;
    const auto projectId = project->id;
    const auto fingerprint = albumFingerprint(album);
    const auto safe =
        juce::Component::SafePointer<MasteringWorkspaceComponent>(this);
    backgroundJobs.addJob(
        [safe,
         album,
         projectId,
         fingerprint,
         destination,
         settings]
        {
            juce::String error;
            auto report = MasteringReleaseService::exportMaster(
                album,
                destination,
                settings,
                signingDirectory(),
                error);
            juce::MessageManager::callAsync(
                [safe,
                 report,
                 error,
                 projectId,
                 fingerprint,
                 destination]
                {
                    if (safe == nullptr)
                        return;
                    safe->finishOperation();
                    if (!report.has_value())
                    {
                        safe->setStatus(error, true);
                        return;
                    }
                    const auto projectMatches =
                        safe->project != nullptr
                        && safe->project->id == projectId
                        && albumFingerprint(
                               safe->project->mastering)
                            == fingerprint;
                    safe->analysisLabel.setText(
                        "Exported "
                            + report->format.toUpperCase()
                            + " - "
                            + juce::String(
                                  report->sampleRate / 1000.0,
                                  1)
                            + " kHz / "
                            + juce::String(report->bitDepth)
                            + "-bit - "
                            + (report->integratedLoudnessLufs.has_value()
                                   ? juce::String(
                                         *report->integratedLoudnessLufs,
                                         2)
                                         + " LUFS"
                                   : juce::String("loudness n/a")),
                        juce::dontSendNotification);
                    if (projectMatches && safe->onReportsAdded)
                        safe->onReportsAdded({ *report });
                    safe->setStatus(
                        "Exported master and signed report to "
                            + destination.getFullPathName()
                            + (projectMatches
                                   ? juce::String()
                                   : juce::String(
                                         " The project changed, so the report was not attached.")),
                        !projectMatches);
                });
        });
}

void MasteringWorkspaceComponent::beginDdpExport()
{
    chooser = std::make_unique<juce::FileChooser>(
        "Select licensed DDP encoder",
        juce::File(),
        juce::String(),
        true,
        false,
        this);
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this)](
            const juce::FileChooser& completed)
        {
            if (safe == nullptr)
                return;
            const auto encoder = completed.getResult();
            safe->chooser.reset();
            if (encoder != juce::File())
                safe->chooseDdpDestination(encoder);
        });
}

void MasteringWorkspaceComponent::chooseDdpDestination(
    const juce::File& encoder)
{
    chooser = std::make_unique<juce::FileChooser>(
        "Choose empty DDP export directory",
        juce::File::getSpecialLocation(
            juce::File::userMusicDirectory),
        juce::String(),
        true,
        true,
        this);
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this),
         encoder](const juce::FileChooser& completed)
        {
            if (safe == nullptr)
                return;
            const auto destination = completed.getResult();
            safe->chooser.reset();
            if (destination == juce::File() || safe->project == nullptr)
                return;
            if (!safe->beginOperation("Exporting DDP fileset..."))
                return;
            const auto album = safe->project->mastering;
            const auto projectId = safe->project->id;
            const auto fingerprint = albumFingerprint(album);
            safe->backgroundJobs.addJob(
                [safe,
                 album,
                 projectId,
                 fingerprint,
                 destination,
                 encoder]
                {
                    juce::String error;
                    auto report =
                        MasteringReleaseService::exportDdp(
                            album,
                            destination,
                            encoder,
                            signingDirectory(),
                            error);
                    juce::MessageManager::callAsync(
                        [safe,
                         report,
                         error,
                         projectId,
                         fingerprint,
                         destination]
                        {
                            if (safe == nullptr)
                                return;
                            safe->finishOperation();
                            if (!report.has_value())
                            {
                                safe->setStatus(error, true);
                                return;
                            }
                            const auto projectMatches =
                                safe->project != nullptr
                                && safe->project->id == projectId
                                && albumFingerprint(
                                       safe->project->mastering)
                                    == fingerprint;
                            if (projectMatches
                                && safe->onReportsAdded)
                                safe->onReportsAdded({ *report });
                            safe->setStatus(
                                "Exported validated DDP fileset to "
                                    + destination.getFullPathName()
                                    + (projectMatches
                                           ? juce::String()
                                           : juce::String(
                                                 " The project changed, so the report was not attached.")),
                                !projectMatches);
                        });
                });
        });
}

void MasteringWorkspaceComponent::beginPortableCopy()
{
    if (project == nullptr)
        return;
    chooser = std::make_unique<juce::FileChooser>(
        "Save portable Studio Duo copy",
        juce::File::getSpecialLocation(
            juce::File::userDocumentsDirectory)
            .getChildFile(project->name + "-Portable.studioduo"),
        "*.studioduo",
        true,
        false,
        this);
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this)](
            const juce::FileChooser& completed)
        {
            if (safe == nullptr)
                return;
            const auto destination = completed.getResult();
            safe->chooser.reset();
            if (destination == juce::File() || safe->project == nullptr)
                return;
            if (!safe->beginOperation("Collecting portable copy..."))
                return;
            const auto projectCopy = *safe->project;
            const auto sourcePackage = safe->projectPackage;
            safe->backgroundJobs.addJob(
                [safe, projectCopy, sourcePackage, destination]
                {
                    juce::String error;
                    const auto report =
                        ProjectCollectionService::savePortableCopy(
                            projectCopy,
                            sourcePackage,
                            destination,
                            error);
                    juce::MessageManager::callAsync(
                        [safe, report, error]
                        {
                            if (safe == nullptr)
                                return;
                            safe->finishOperation();
                            if (!report.has_value())
                            {
                                safe->setStatus(error, true);
                                return;
                            }
                            safe->setStatus(
                                "Portable copy saved with "
                                + juce::String(
                                      report->copiedResources)
                                + " collected resource(s).");
                        });
                });
        });
}

void MasteringWorkspaceComponent::beginRepair()
{
    if (project == nullptr)
        return;
    chooser = std::make_unique<juce::FileChooser>(
        "Choose a folder containing missing project media",
        juce::File::getSpecialLocation(
            juce::File::userMusicDirectory),
        juce::String(),
        true,
        true,
        this);
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<
             MasteringWorkspaceComponent>(this)](
            const juce::FileChooser& completed)
        {
            if (safe == nullptr)
                return;
            const auto root = completed.getResult();
            safe->chooser.reset();
            if (root == juce::File() || safe->project == nullptr)
                return;
            if (!safe->beginOperation("Scanning for missing files..."))
                return;
            auto projectToRepair = *safe->project;
            const auto package = safe->projectPackage;
            const auto originalProjectId = projectToRepair.id;
            const auto originalFingerprint =
                projectFingerprint(projectToRepair);
            safe->backgroundJobs.addJob(
                [safe,
                 backgroundProject = std::move(projectToRepair),
                 originalProjectId,
                 originalFingerprint,
                 package,
                 root]() mutable
                {
                    juce::String error;
                    const auto report =
                        ProjectCollectionService::
                            repairMissingResources(
                                backgroundProject,
                                { root },
                                error,
                                package);
                    juce::MessageManager::callAsync(
                        [safe,
                         completedProject =
                             std::move(backgroundProject),
                         report,
                         error,
                         originalProjectId,
                         originalFingerprint]() mutable
                        {
                            if (safe == nullptr)
                                return;
                            safe->finishOperation();
                            if (safe->project == nullptr
                                || safe->project->id
                                    != originalProjectId
                                || projectFingerprint(*safe->project)
                                    != originalFingerprint)
                            {
                                safe->setStatus(
                                    "Repair scan completed, but the project changed before repairs could be applied.",
                                    true);
                                return;
                            }
                            if (report.repairedResources > 0
                                && safe->onProjectRepaired)
                                safe->onProjectRepaired(
                                    std::move(completedProject));
                            safe->setStatus(
                                "Repaired "
                                    + juce::String(
                                          report.repairedResources)
                                    + " resource(s). "
                                    + (error.isNotEmpty()
                                           ? error
                                           : juce::String(
                                                 "No resources remain missing.")),
                                report.missingResources > 0);
                        });
                });
        });
}

void MasteringWorkspaceComponent::setStatus(
    const juce::String& message,
    bool error)
{
    if (onStatus)
        onStatus(message, error);
}

bool MasteringWorkspaceComponent::beginOperation(
    const juce::String& message)
{
    auto expected = false;
    if (!operationInProgress.compare_exchange_strong(expected, true))
    {
        setStatus("A mastering operation is already in progress.", true);
        return false;
    }
    for (auto* button : {
             &analyseButton,
             &exportButton,
             &ddpButton,
             &portableButton,
             &repairButton })
        button->setEnabled(false);
    setStatus(message);
    return true;
}

void MasteringWorkspaceComponent::finishOperation()
{
    operationInProgress.store(false);
    for (auto* button : {
             &analyseButton,
             &exportButton,
             &ddpButton,
             &portableButton,
             &repairButton })
        button->setEnabled(true);
}

juce::File MasteringWorkspaceComponent::signingDirectory()
{
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Studio Duo")
        .getChildFile("Signing");
}

void MasteringWorkspaceComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::window));
    graphics.setColour(juce::Colour(StudioColours::border));
    graphics.drawVerticalLine(
        340,
        54.0f,
        static_cast<float>(getHeight() - 12));
}

void MasteringWorkspaceComponent::resized()
{
    auto bounds = getLocalBounds().reduced(16, 12);
    titleLabel.setBounds(bounds.removeFromTop(34));
    bounds.removeFromTop(8);
    auto actions = bounds.removeFromBottom(42);
    analyseButton.setBounds(actions.removeFromLeft(130).reduced(2));
    exportButton.setBounds(actions.removeFromLeft(132).reduced(2));
    ddpButton.setBounds(actions.removeFromLeft(112).reduced(2));
    portableButton.setBounds(actions.removeFromLeft(126).reduced(2));
    repairButton.setBounds(actions.removeFromLeft(116).reduced(2));
    analysisLabel.setBounds(actions.reduced(8, 0));
    bounds.removeFromBottom(8);

    auto left = bounds.removeFromLeft(316);
    bounds.removeFromLeft(18);
    trackList.setBounds(left.removeFromTop(
        juce::jmax(140, left.getHeight() - 96)));
    auto addRow = left.removeFromTop(34);
    addSongButton.setBounds(addRow.removeFromLeft(96).reduced(2));
    addAlternateButton.setBounds(addRow.removeFromLeft(104).reduced(2));
    addReferenceButton.setBounds(addRow.reduced(2));
    auto orderRow = left.removeFromTop(34);
    moveUpButton.setBounds(orderRow.removeFromLeft(62).reduced(2));
    moveDownButton.setBounds(orderRow.removeFromLeft(70).reduced(2));
    removeButton.setBounds(orderRow.removeFromLeft(92).reduced(2));

    auto album = bounds.removeFromLeft(
        juce::jmax(280, bounds.getWidth() / 2));
    bounds.removeFromLeft(18);
    auto track = bounds;
    const auto placeField = [](juce::Rectangle<int>& area,
                               juce::Label& label,
                               juce::Component& editor)
    {
        auto row = area.removeFromTop(48);
        label.setBounds(row.removeFromTop(18));
        editor.setBounds(row.removeFromTop(28));
    };
    albumSectionLabel.setBounds(album.removeFromTop(28));
    placeField(album, albumTitleLabel, albumTitleEditor);
    placeField(album, albumArtistLabel, albumArtistEditor);
    placeField(album, albumSongwriterLabel, albumSongwriterEditor);
    placeField(album, labelNameLabel, labelNameEditor);
    placeField(album, catalogLabel, catalogEditor);
    placeField(album, mcnLabel, mcnEditor);
    placeField(album, releaseDateLabel, releaseDateEditor);
    placeField(album, genreLabel, genreEditor);
    placeField(album, outputGainLabel, outputGainEditor);

    trackSectionLabel.setBounds(track.removeFromTop(28));
    placeField(track, trackTitleLabel, trackTitleEditor);
    placeField(track, trackArtistLabel, trackArtistEditor);
    placeField(track, trackSongwriterLabel, trackSongwriterEditor);
    placeField(track, isrcLabel, isrcEditor);
    placeField(track, sourceLabel, sourceSelector);
    placeField(track, gapLabel, gapEditor);
    placeField(track, overlapLabel, overlapEditor);
    placeField(track, fadeInLabel, fadeInEditor);
    placeField(track, fadeOutLabel, fadeOutEditor);
    placeField(track, trackGainLabel, trackGainEditor);
}
}

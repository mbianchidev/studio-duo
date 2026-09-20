#pragma once

#include "StudioIconButton.h"
#include "mastering/MasteringEngine.h"
#include "mastering/MasteringReleaseService.h"
#include "model/ProjectModel.h"
#include "project_io/ProjectCollectionService.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <atomic>

namespace studio
{
class MasteringWorkspaceComponent final : public juce::Component,
                                          private juce::ListBoxModel
{
public:
    MasteringWorkspaceComponent();

    void setProject(Project* projectToUse);
    void setProjectPackage(const juce::File& package);
    void refresh();

    std::function<bool(const MasteringAlbum&, const MasteringAlbum&)>
        onAlbumEdited;
    std::function<void(std::vector<RenderReport>)> onReportsAdded;
    std::function<void(Project)> onProjectRepaired;
    std::function<void(const juce::String&, bool)> onStatus;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    enum class AddMode
    {
        song,
        alternate,
        reference
    };

    int getNumRows() override;
    void paintListBoxItem(int row,
                          juce::Graphics& graphics,
                          int width,
                          int height,
                          bool selected) override;
    void selectedRowsChanged(int row) override;

    void configureEditor(juce::TextEditor& editor,
                         const juce::String& tooltip);
    void commitAlbumFields();
    void commitTrackFields();
    void editAlbum(
        const std::function<void(MasteringAlbum&)>& edit);
    void chooseAudio(AddMode mode);
    void addAudio(const juce::File& file, AddMode mode);
    void removeSelectedTrack();
    void moveSelectedTrack(int offset);
    void analyseAlbum();
    void showExportOptions();
    void beginExport(MasteringExportSettings settings);
    void exportTo(const juce::File& destination,
                  MasteringExportSettings settings);
    void beginDdpExport();
    void chooseDdpDestination(const juce::File& encoder);
    void beginPortableCopy();
    void beginRepair();
    void updateTrackEditors();
    void setStatus(const juce::String& message, bool error = false);
    bool beginOperation(const juce::String& message);
    void finishOperation();
    [[nodiscard]] MasteringTrack* selectedTrack() noexcept;
    [[nodiscard]] const MasteringTrack* selectedTrack() const noexcept;
    static juce::File signingDirectory();

    Project* project = nullptr;
    juce::File projectPackage;
    int selectedTrackIndex = -1;
    bool refreshing = false;
    std::atomic<bool> operationInProgress { false };
    juce::ThreadPool backgroundJobs { 1 };
    std::unique_ptr<juce::FileChooser> chooser;
    MasteringExportSettings lastExportSettings;

    juce::Label titleLabel;
    juce::ListBox trackList { "Mastering songs", this };
    juce::TextButton addSongButton { "+ SONG" };
    juce::TextButton addAlternateButton { "+ ALT MIX" };
    juce::TextButton addReferenceButton { "+ REFERENCE" };
    StudioIconButton moveUpButton {
        StudioIcon::chevronUp,
        "Move song up",
        "Move the selected song earlier in the album"
    };
    StudioIconButton moveDownButton {
        StudioIcon::chevronDown,
        "Move song down",
        "Move the selected song later in the album"
    };
    StudioIconButton removeButton {
        StudioIcon::deleteItem,
        "Remove mastering item",
        "Remove the selected song, alternate mix, or reference"
    };

    juce::Label albumSectionLabel;
    juce::Label albumTitleLabel;
    juce::Label albumArtistLabel;
    juce::Label albumSongwriterLabel;
    juce::Label labelNameLabel;
    juce::Label catalogLabel;
    juce::Label mcnLabel;
    juce::Label releaseDateLabel;
    juce::Label genreLabel;
    juce::Label outputGainLabel;
    juce::TextEditor albumTitleEditor;
    juce::TextEditor albumArtistEditor;
    juce::TextEditor albumSongwriterEditor;
    juce::TextEditor labelNameEditor;
    juce::TextEditor catalogEditor;
    juce::TextEditor mcnEditor;
    juce::TextEditor releaseDateEditor;
    juce::TextEditor genreEditor;
    juce::TextEditor outputGainEditor;

    juce::Label trackSectionLabel;
    juce::Label trackTitleLabel;
    juce::Label trackArtistLabel;
    juce::Label trackSongwriterLabel;
    juce::Label isrcLabel;
    juce::Label sourceLabel;
    juce::Label gapLabel;
    juce::Label overlapLabel;
    juce::Label fadeInLabel;
    juce::Label fadeOutLabel;
    juce::Label trackGainLabel;
    juce::TextEditor trackTitleEditor;
    juce::TextEditor trackArtistEditor;
    juce::TextEditor trackSongwriterEditor;
    juce::TextEditor isrcEditor;
    juce::ComboBox sourceSelector;
    juce::TextEditor gapEditor;
    juce::TextEditor overlapEditor;
    juce::TextEditor fadeInEditor;
    juce::TextEditor fadeOutEditor;
    juce::TextEditor trackGainEditor;

    juce::Label analysisLabel;
    juce::TextButton analyseButton { "ANALYZE ALBUM" };
    juce::TextButton exportButton { "EXPORT MASTER" };
    juce::TextButton ddpButton { "EXPORT DDP" };
    juce::TextButton portableButton { "PORTABLE COPY" };
    juce::TextButton repairButton { "REPAIR FILES" };
};
}

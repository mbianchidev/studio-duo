#include "UpdateSettingsComponent.h"

#include "ui/StudioTheme.h"

namespace studio
{
namespace
{
class GeneralSettingsComponent final : public juce::Component
{
public:
    explicit GeneralSettingsComponent(
        StudioPreferences& preferencesToUse)
        : preferences(preferencesToUse)
    {
        addAndMakeVisible(title);
        title.setText(
            "Project preferences",
            juce::dontSendNotification);
        title.setFont(
            juce::Font(
                juce::FontOptions(22.0f,
                                  juce::Font::bold)));

        addAndMakeVisible(autosave);
        autosave.setButtonText(
            "Autosave project recovery after edits");
        autosave.setTooltip(
            "Write the latest project state to the package recovery copy after each edit.");
        autosave.setToggleState(
            preferences.autosaveEnabled(),
            juce::dontSendNotification);
        autosave.onClick = [this]
        {
            const auto enabled =
                autosave.getToggleState();
            const auto result =
                preferences.setAutosaveEnabled(enabled);
            if (result.failed())
            {
                autosave.setToggleState(
                    !enabled,
                    juce::dontSendNotification);
                status.setText(
                    result.getErrorMessage(),
                    juce::dontSendNotification);
                status.setColour(
                    juce::Label::textColourId,
                    juce::Colour(StudioColours::orange));
                return;
            }
            status.setText(
                enabled
                    ? "Autosave recovery is enabled."
                    : "Autosave recovery is disabled.",
                juce::dontSendNotification);
            status.setColour(
                juce::Label::textColourId,
                juce::Colour(
                    StudioColours::secondaryText));
        };

        addAndMakeVisible(help);
        help.setText(
            "Autosave writes a recovery copy inside saved .studioduo projects. "
            "Manual Save still creates the durable project generation.",
            juce::dontSendNotification);
        help.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
        help.setJustificationType(
            juce::Justification::topLeft);

        addAndMakeVisible(status);
        status.setText(
            preferences.status(),
            juce::dontSendNotification);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(
            juce::Colour(StudioColours::panel));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(24);
        title.setBounds(bounds.removeFromTop(36));
        bounds.removeFromTop(18);
        autosave.setBounds(bounds.removeFromTop(32));
        help.setBounds(bounds.removeFromTop(58));
        status.setBounds(bounds.removeFromTop(28));
    }

private:
    StudioPreferences& preferences;
    juce::Label title;
    juce::ToggleButton autosave;
    juce::Label help;
    juce::Label status;
};

class ThemeColourSelector final
    : public juce::ColourSelector,
      private juce::ChangeListener
{
public:
    ThemeColourSelector(
        juce::Colour initial,
        std::function<void(juce::Colour)> changed)
        : juce::ColourSelector(
              showColourAtTop
                  | editableColour
                  | showSliders
                  | showColourspace),
          colourChanged(std::move(changed))
    {
        setName("Theme colour");
        setSize(320, 400);
        setCurrentColour(
            initial.withAlpha(1.0f),
            juce::dontSendNotification);
        addChangeListener(this);
    }

    ~ThemeColourSelector() override
    {
        removeChangeListener(this);
    }

private:
    void changeListenerCallback(
        juce::ChangeBroadcaster*) override
    {
        if (colourChanged)
        {
            colourChanged(
                getCurrentColour()
                    .withAlpha(1.0f));
        }
    }

    std::function<void(juce::Colour)>
        colourChanged;
};

class AppearanceSettingsComponent final
    : public juce::Component
{
public:
    AppearanceSettingsComponent(
        StudioPreferences& preferencesToUse,
        std::function<void(
            const StudioThemePalette&)>
            themeChangedToUse)
        : preferences(preferencesToUse),
          themeChanged(std::move(
              themeChangedToUse))
    {
        addAndMakeVisible(title);
        title.setText(
            "Appearance",
            juce::dontSendNotification);
        title.setFont(
            juce::Font(
                juce::FontOptions(
                    22.0f,
                    juce::Font::bold)));

        addAndMakeVisible(presetLabel);
        presetLabel.setText(
            "Theme",
            juce::dontSendNotification);
        presetLabel.setColour(
            juce::Label::textColourId,
            juce::Colour(
                StudioColours::secondaryText));

        addAndMakeVisible(presetSelector);
        int itemId = 1;
        for (const auto& preset :
             studioThemePresets())
        {
            presetSelector.addItem(
                preset.name,
                itemId++);
        }
        presetSelector.addItem("Custom", itemId);
        presetSelector.setTooltip(
            "Choose a curated theme or edit a custom palette");
        presetSelector.onChange = [this]
        {
            const auto index =
                presetSelector
                    .getSelectedItemIndex();
            if (index < 0)
                return;
            const auto& presets =
                studioThemePresets();
            if (index
                >= static_cast<int>(
                    presets.size()))
            {
                const auto result =
                    preferences
                        .setThemePreset(
                            "custom");
                if (result.failed())
                {
                    setStatus(
                        result.getErrorMessage(),
                        true);
                    refresh();
                    return;
                }
                applyTheme(
                    preferences
                        .themePalette());
                setStatus(
                    "Custom theme applied and saved.",
                    false);
                return;
            }
            const auto result =
                preferences.setThemePreset(
                    presets[
                        static_cast<std::size_t>(
                            index)]
                        .id);
            if (result.failed())
            {
                setStatus(
                    result.getErrorMessage(),
                    true);
                refresh();
                return;
            }
            applyTheme(
                preferences.themePalette());
            setStatus(
                "Theme applied and saved.",
                false);
            refresh();
        };

        addAndMakeVisible(help);
        help.setText(
            "Choose a bundled palette or customize the base surfaces, "
            "text, and focus accent. Custom colors must preserve WCAG AA contrast.",
            juce::dontSendNotification);
        help.setColour(
            juce::Label::textColourId,
            juce::Colour(
                StudioColours::secondaryText));
        help.setJustificationType(
            juce::Justification::topLeft);

        configureColourButton(
            baseButton,
            "BASE",
            [](auto& palette,
               std::uint32_t value)
            {
                palette.window = value;
            });
        configureColourButton(
            panelButton,
            "PANEL",
            [](auto& palette,
               std::uint32_t value)
            {
                palette.panel = value;
            });
        configureColourButton(
            raisedButton,
            "RAISED",
            [](auto& palette,
               std::uint32_t value)
            {
                palette.raised = value;
            });
        configureColourButton(
            textButton,
            "TEXT",
            [](auto& palette,
               std::uint32_t value)
            {
                palette.text = value;
            });
        configureColourButton(
            accentButton,
            "ACCENT",
            [](auto& palette,
               std::uint32_t value)
            {
                palette.orange = value;
            });

        addAndMakeVisible(resetButton);
        resetButton.setButtonText(
            "RESET DEFAULT THEME");
        resetButton.setTooltip(
            "Restore the Studio Gray theme");
        resetButton.onClick = [this]
        {
            const auto result =
                preferences.resetTheme();
            if (result.failed())
            {
                setStatus(
                    result.getErrorMessage(),
                    true);
                return;
            }
            applyTheme(
                preferences.themePalette());
            setStatus(
                "Default theme restored.",
                false);
            refresh();
        };

        addAndMakeVisible(status);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(
                StudioColours::secondaryText));
        refresh();
    }

    void paint(
        juce::Graphics& graphics) override
    {
        graphics.fillAll(
            juce::Colour(
                StudioColours::panel));
    }

    void resized() override
    {
        auto bounds =
            getLocalBounds().reduced(24);
        title.setBounds(
            bounds.removeFromTop(36));
        bounds.removeFromTop(12);
        presetLabel.setBounds(
            bounds.removeFromTop(22));
        presetSelector.setBounds(
            bounds.removeFromTop(34));
        bounds.removeFromTop(10);
        help.setBounds(
            bounds.removeFromTop(52));
        bounds.removeFromTop(12);
        auto firstRow =
            bounds.removeFromTop(42);
        baseButton.setBounds(
            firstRow.removeFromLeft(150)
                .reduced(2));
        panelButton.setBounds(
            firstRow.removeFromLeft(150)
                .reduced(2));
        raisedButton.setBounds(
            firstRow.removeFromLeft(150)
                .reduced(2));
        bounds.removeFromTop(8);
        auto secondRow =
            bounds.removeFromTop(42);
        textButton.setBounds(
            secondRow.removeFromLeft(150)
                .reduced(2));
        accentButton.setBounds(
            secondRow.removeFromLeft(150)
                .reduced(2));
        bounds.removeFromTop(20);
        resetButton.setBounds(
            bounds.removeFromTop(38)
                .removeFromLeft(220));
        bounds.removeFromTop(10);
        status.setBounds(
            bounds.removeFromTop(30));
    }

private:
    using ColourSetter =
        std::function<void(
            StudioThemePalette&,
            std::uint32_t)>;

    void configureColourButton(
        juce::TextButton& button,
        juce::String role,
        ColourSetter setter)
    {
        addAndMakeVisible(button);
        button.setTooltip(
            "Customize the " + role.toLowerCase()
            + " theme color");
        auto* const buttonToUpdate = &button;
        button.onClick =
            [this,
             buttonToUpdate,
             roleName = std::move(role),
             applyColour = std::move(setter)]
            {
                const auto initial =
                    buttonToUpdate->findColour(
                        juce::TextButton::
                            buttonColourId);
                auto selector =
                    std::make_unique<
                        ThemeColourSelector>(
                        initial,
                        [this,
                         roleName,
                         applyColour](
                            juce::Colour selected)
                        {
                            auto palette =
                                preferences
                                    .themePalette();
                            applyColour(
                                palette,
                                selected.getARGB());
                            const auto result =
                                preferences
                                    .setCustomThemePalette(
                                        palette);
                            if (result.failed())
                            {
                                setStatus(
                                    result
                                        .getErrorMessage(),
                                    true);
                                return;
                            }
                            applyTheme(palette);
                            setStatus(
                                roleName
                                    + " color applied.",
                                false);
                            refresh();
                        });
                juce::CallOutBox::
                    launchAsynchronously(
                        std::move(selector),
                        buttonToUpdate
                            ->getScreenBounds(),
                        nullptr);
            };
    }

    void applyTheme(
        const StudioThemePalette& palette)
    {
        if (themeChanged)
            themeChanged(palette);
    }

    void refresh()
    {
        const auto& presets =
            studioThemePresets();
        if (preferences.themePresetId()
            == "custom")
        {
            presetSelector.setSelectedId(
                static_cast<int>(
                    presets.size())
                    + 1,
                juce::dontSendNotification);
        }
        else
        {
            for (std::size_t index = 0;
                 index < presets.size();
                 ++index)
            {
                if (preferences
                        .themePresetId()
                    == presets[index].id)
                {
                    presetSelector
                        .setSelectedId(
                            static_cast<int>(
                                index)
                                + 1,
                            juce::dontSendNotification);
                    break;
                }
            }
        }
        const auto palette =
            preferences.themePalette();
        updateColourButton(
            baseButton,
            "BASE",
            palette.window);
        updateColourButton(
            panelButton,
            "PANEL",
            palette.panel);
        updateColourButton(
            raisedButton,
            "RAISED",
            palette.raised);
        updateColourButton(
            textButton,
            "TEXT",
            palette.text);
        updateColourButton(
            accentButton,
            "ACCENT",
            palette.orange);
    }

    static void updateColourButton(
        juce::TextButton& button,
        const juce::String& role,
        std::uint32_t colourValue)
    {
        const auto colour =
            juce::Colour(colourValue);
        button.setButtonText(
            role + "  #"
            + colour.toDisplayString(false));
        button.setColour(
            juce::TextButton::buttonColourId,
            colour);
        button.setColour(
            juce::TextButton::textColourOffId,
            colour.contrasting());
    }

    void setStatus(
        const juce::String& message,
        bool error)
    {
        status.setText(
            message,
            juce::dontSendNotification);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(
                error
                    ? StudioColours::orange
                    : StudioColours::secondaryText));
    }

    StudioPreferences& preferences;
    std::function<void(
        const StudioThemePalette&)>
        themeChanged;
    juce::Label title;
    juce::Label presetLabel;
    juce::ComboBox presetSelector;
    juce::Label help;
    juce::TextButton baseButton;
    juce::TextButton panelButton;
    juce::TextButton raisedButton;
    juce::TextButton textButton;
    juce::TextButton accentButton;
    juce::TextButton resetButton;
    juce::Label status;
};

class VstPluginSettingsComponent final
    : public juce::Component,
      private juce::ListBoxModel,
      private juce::Timer
{
public:
    VstPluginSettingsComponent(
        PluginCatalog& catalogToUse,
        StudioPreferences& preferencesToUse,
        std::function<void(const PluginCatalogEntry&)>
            validationCallback)
        : catalog(catalogToUse),
          preferences(preferencesToUse),
          validatePlugin(std::move(validationCallback)),
          list("VST3 search folders", this)
    {
        addAndMakeVisible(title);
        title.setText(
            "VST plug-ins",
            juce::dontSendNotification);
        title.setFont(
            juce::Font(
                juce::FontOptions(22.0f,
                                  juce::Font::bold)));

        addAndMakeVisible(scanAtStartup);
        scanAtStartup.setButtonText(
            "Scan plug-in folders at startup");
        scanAtStartup.setToggleState(
            preferences.scanPluginsAtStartup(),
            juce::dontSendNotification);
        scanAtStartup.onClick = [this]
        {
            const auto enabled =
                scanAtStartup.getToggleState();
            const auto result =
                preferences.setScanPluginsAtStartup(
                    enabled);
            if (result.failed())
            {
                scanAtStartup.setToggleState(
                    !enabled,
                    juce::dontSendNotification);
                setStatus(
                    result.getErrorMessage(),
                    true);
            }
            else
            {
                setStatus(
                    enabled
                        ? "Startup plug-in scan enabled."
                        : "Startup plug-in scan disabled.",
                    false);
            }
        };

        addAndMakeVisible(pluginSelector);
        pluginSelector.setTooltip(
            "Choose one scanned plug-in for advanced isolated validation");
        pluginSelector.onChange = [this]
        {
            validateSelected.setEnabled(
                pluginSelector.getSelectedItemIndex() >= 0);
        };
        addAndMakeVisible(validateSelected);
        validateSelected.setButtonText(
            "ADVANCED VALIDATE");
        validateSelected.setTooltip(
            "Launch an isolated deep compatibility check for the selected plug-in");
        validateSelected.onClick = [this]
        {
            const auto index =
                pluginSelector.getSelectedItemIndex();
            if (index >= 0
                && index
                    < static_cast<int>(
                        validationEntries.size())
                && validatePlugin)
            {
                validatePlugin(
                    validationEntries[
                        static_cast<std::size_t>(index)]);
            }
        };

        addAndMakeVisible(list);
        list.setRowHeight(34);
        list.setColour(
            juce::ListBox::backgroundColourId,
            juce::Colour(StudioColours::window));
        list.setColour(
            juce::ListBox::outlineColourId,
            juce::Colour(StudioColours::border));
        list.setOutlineThickness(1);

        addAndMakeVisible(addFolder);
        addFolder.setButtonText("ADD FOLDER");
        addFolder.onClick = [this] { chooseFolder(); };
        addAndMakeVisible(removeFolder);
        removeFolder.setButtonText("REMOVE");
        removeFolder.onClick = [this] { removeSelected(); };
        addAndMakeVisible(restoreDefaults);
        restoreDefaults.setButtonText("RESTORE DEFAULTS");
        restoreDefaults.onClick = [this]
        {
            const auto result =
                catalog.restoreDefaultVst3SearchFolders();
            setStatus(
                result.wasOk()
                    ? "Default VST3 folders restored."
                    : result.getErrorMessage(),
                result.failed());
            refresh();
        };
        addAndMakeVisible(rescan);
        rescan.setButtonText("RESCAN NOW");
        rescan.onClick = [this]
        {
            catalog.startScan(true);
            setStatus(
                "VST3 rescan started.",
                false);
        };

        addAndMakeVisible(status);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(StudioColours::secondaryText));
        refresh();
        refreshPlugins();
        startTimerHz(2);
    }

    ~VstPluginSettingsComponent() override
    {
        stopTimer();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(
            juce::Colour(StudioColours::panel));
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(24);
        title.setBounds(bounds.removeFromTop(36));
        scanAtStartup.setBounds(
            bounds.removeFromTop(32));
        auto validationRow = bounds.removeFromTop(34);
        pluginSelector.setBounds(
            validationRow.removeFromLeft(
                juce::jmax(
                    180,
                    validationRow.getWidth() - 170))
                .reduced(2));
        validateSelected.setBounds(
            validationRow.reduced(2));
        bounds.removeFromTop(8);
        auto actions = bounds.removeFromBottom(36);
        addFolder.setBounds(
            actions.removeFromLeft(110).reduced(2));
        removeFolder.setBounds(
            actions.removeFromLeft(92).reduced(2));
        restoreDefaults.setBounds(
            actions.removeFromLeft(150).reduced(2));
        rescan.setBounds(
            actions.removeFromLeft(112).reduced(2));
        status.setBounds(
            bounds.removeFromBottom(28));
        bounds.removeFromBottom(8);
        list.setBounds(bounds);
    }

private:
    void timerCallback() override
    {
        if (lastCatalogRevision != catalog.revision())
            refreshPlugins();
    }

    int getNumRows() override
    {
        return paths.size();
    }

    void paintListBoxItem(int row,
                          juce::Graphics& graphics,
                          int width,
                          int height,
                          bool selected) override
    {
        if (row < 0 || row >= paths.size())
            return;
        if (selected)
        {
            graphics.setColour(
                juce::Colour(StudioColours::raised));
            graphics.fillRect(
                0,
                0,
                width,
                height);
        }
        const auto file = juce::File(paths[row]);
        graphics.setColour(
            file.isDirectory()
                ? juce::Colour(StudioColours::text)
                : juce::Colour(StudioColours::orange));
        graphics.setFont(10.5f);
        graphics.drawFittedText(
            paths[row],
            8,
            2,
            width - 90,
            height - 4,
            juce::Justification::centredLeft,
            1);
        graphics.setColour(
            juce::Colour(StudioColours::secondaryText));
        graphics.setFont(
            juce::Font(
                juce::FontOptions(8.5f,
                                  juce::Font::bold)));
        graphics.drawText(
            defaultFlags[row] ? "DEFAULT" : "CUSTOM",
            width - 78,
            0,
            70,
            height,
            juce::Justification::centredRight);
    }

    void selectedRowsChanged(int row) override
    {
        removeFolder.setEnabled(
            row >= 0 && row < paths.size());
    }

    void refresh()
    {
        paths.clear();
        defaultFlags.clear();
        const auto defaults =
            catalog.defaultVst3SearchFolders();
        const auto disabled =
            catalog.disabledDefaultVst3SearchFolders();
        for (const auto& path : defaults)
        {
            if (!disabled.contains(path))
            {
                paths.add(path);
                defaultFlags.add(true);
            }
        }
        for (const auto& path :
             catalog.customVst3SearchFolders())
        {
            if (!paths.contains(path))
            {
                paths.add(path);
                defaultFlags.add(false);
            }
        }
        list.updateContent();
        list.deselectAllRows();
        removeFolder.setEnabled(false);
        repaint();
    }

    void refreshPlugins()
    {
        const auto selectedIdentifier =
            pluginSelector.getSelectedItemIndex() >= 0
                && pluginSelector.getSelectedItemIndex()
                    < static_cast<int>(
                        validationEntries.size())
            ? validationEntries[
                  static_cast<std::size_t>(
                      pluginSelector
                          .getSelectedItemIndex())]
                  .identifier
            : juce::String();
        validationEntries.clear();
        pluginSelector.clear(
            juce::dontSendNotification);
        for (const auto& entry : catalog.entries())
        {
            if (entry.bundledDevice)
                continue;
            validationEntries.push_back(entry);
            pluginSelector.addItem(
                entry.name + " [" + entry.format + "]",
                static_cast<int>(
                    validationEntries.size()));
        }
        const auto selected = std::find_if(
            validationEntries.cbegin(),
            validationEntries.cend(),
            [&selectedIdentifier](const auto& entry)
            {
                return entry.identifier
                    == selectedIdentifier;
            });
        if (selected != validationEntries.cend())
        {
            pluginSelector.setSelectedItemIndex(
                static_cast<int>(
                    std::distance(
                        validationEntries.cbegin(),
                        selected)),
                juce::dontSendNotification);
        }
        validateSelected.setEnabled(
            pluginSelector.getSelectedItemIndex() >= 0);
        lastCatalogRevision = catalog.revision();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Add VST3 search folder",
            juce::File::getSpecialLocation(
                juce::File::userHomeDirectory));
        const auto safe =
            juce::Component::SafePointer<
                VstPluginSettingsComponent>(this);
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectDirectories,
            [safe](const juce::FileChooser& completed)
            {
                if (safe == nullptr)
                    return;
                const auto folder = completed.getResult();
                if (folder == juce::File())
                    return;
                const auto result =
                    safe->catalog.addCustomVst3SearchFolder(
                        folder);
                safe->setStatus(
                    result.wasOk()
                        ? "VST3 folder added."
                        : result.getErrorMessage(),
                    result.failed());
                safe->refresh();
            });
    }

    void removeSelected()
    {
        const auto row = list.getSelectedRow();
        if (row < 0 || row >= paths.size())
            return;
        const auto result =
            catalog.removeVst3SearchFolder(
                juce::File(paths[row]));
        setStatus(
            result.wasOk()
                ? "VST3 folder removed."
                : result.getErrorMessage(),
            result.failed());
        refresh();
    }

    void setStatus(const juce::String& message,
                   bool error)
    {
        status.setText(
            message,
            juce::dontSendNotification);
        status.setColour(
            juce::Label::textColourId,
            juce::Colour(
                error
                    ? StudioColours::orange
                    : StudioColours::secondaryText));
    }

    PluginCatalog& catalog;
    StudioPreferences& preferences;
    std::function<void(const PluginCatalogEntry&)>
        validatePlugin;
    juce::Label title;
    juce::ToggleButton scanAtStartup;
    juce::ComboBox pluginSelector;
    juce::TextButton validateSelected;
    juce::ListBox list;
    juce::TextButton addFolder;
    juce::TextButton removeFolder;
    juce::TextButton restoreDefaults;
    juce::TextButton rescan;
    juce::Label status;
    juce::StringArray paths;
    juce::Array<bool> defaultFlags;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<PluginCatalogEntry> validationEntries;
    std::uint64_t lastCatalogRevision = 0;
};
}

UpdateSettingsComponent::UpdateSettingsComponent(
    UpdateService& service,
    std::function<void()> restartCallback)
    : updateService(service),
      restartRequested(std::move(restartCallback))
{
    addAndMakeVisible(titleLabel);
    titleLabel.setText(
        "Studio Duo updates",
        juce::dontSendNotification);
    titleLabel.setFont(
        juce::Font(
            juce::FontOptions(22.0f, juce::Font::bold)));
    titleLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::text));

    addAndMakeVisible(versionLabel);
    versionLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::secondaryText));

    addAndMakeVisible(statusLabel);
    statusLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(StudioColours::text));
    statusLabel.setJustificationType(
        juce::Justification::topLeft);
    statusLabel.setMinimumHorizontalScale(0.8f);

    addAndMakeVisible(automaticDownloadToggle);
    automaticDownloadToggle.setTooltip(
        "Download verified updates in the background after launch");
    automaticDownloadToggle.onClick = [this]
    {
        const auto requested =
            automaticDownloadToggle.getToggleState();
        const auto result =
            updateService.setAutomaticDownloads(requested);
        if (result.failed())
        {
            automaticDownloadToggle.setToggleState(
                !requested,
                juce::dontSendNotification);
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Update settings could not be saved",
                result.getErrorMessage());
        }
    };

    addAndMakeVisible(progressBar);
    progressBar.setPercentageDisplay(true);

    addAndMakeVisible(checkButton);
    checkButton.setTooltip(
        "Check the official Studio Duo release feed now");
    checkButton.onClick = [this]
    {
        updateService.checkForUpdates();
    };

    addAndMakeVisible(downloadButton);
    downloadButton.setTooltip(
        "Download and verify the available update");
    downloadButton.onClick = [this]
    {
        updateService.downloadUpdate();
    };

    addAndMakeVisible(restartButton);
    restartButton.setTooltip(
        "Quit Studio Duo, install the downloaded update, and reopen it");
    restartButton.onClick = [this]
    {
        if (restartRequested)
            restartRequested();
    };

    updateService.addListener(this);
    refresh(updateService.snapshot());
}

UpdateSettingsComponent::~UpdateSettingsComponent()
{
    updateService.removeListener(this);
}

void UpdateSettingsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void UpdateSettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced(24);
    titleLabel.setBounds(bounds.removeFromTop(34));
    versionLabel.setBounds(bounds.removeFromTop(26));
    bounds.removeFromTop(14);
    statusLabel.setBounds(bounds.removeFromTop(72));
    bounds.removeFromTop(10);
    automaticDownloadToggle.setBounds(
        bounds.removeFromTop(30));
    bounds.removeFromTop(16);
    progressBar.setBounds(bounds.removeFromTop(24));

    auto buttons = bounds.removeFromBottom(38);
    checkButton.setBounds(
        buttons.removeFromLeft(168).reduced(3));
    downloadButton.setBounds(
        buttons.removeFromLeft(132).reduced(3));
    restartButton.setBounds(
        buttons.removeFromLeft(188).reduced(3));
}

void UpdateSettingsComponent::updateStateChanged(
    const UpdateSnapshot& snapshot)
{
    refresh(snapshot);
}

void UpdateSettingsComponent::refresh(
    const UpdateSnapshot& snapshot)
{
    auto versionText =
        "Current version: " + snapshot.currentVersion;
    if (snapshot.availableVersion.isNotEmpty())
    {
        versionText +=
            "    Available: " + snapshot.availableVersion;
    }
    versionLabel.setText(
        versionText,
        juce::dontSendNotification);
    statusLabel.setText(
        snapshot.message,
        juce::dontSendNotification);
    statusLabel.setColour(
        juce::Label::textColourId,
        juce::Colour(
            snapshot.phase == UpdatePhase::failed
                ? StudioColours::orange
                : StudioColours::text));
    automaticDownloadToggle.setToggleState(
        snapshot.automaticDownloads,
        juce::dontSendNotification);
    automaticDownloadToggle.setEnabled(
        snapshot.phase != UpdatePhase::unsupported);

    downloadProgress = snapshot.progress;
    progressBar.setVisible(
        snapshot.phase == UpdatePhase::downloading);

    const auto busy =
        snapshot.phase == UpdatePhase::checking
        || snapshot.phase == UpdatePhase::downloading;
    checkButton.setEnabled(
        !busy
        && snapshot.phase != UpdatePhase::unsupported);
    downloadButton.setEnabled(
        !busy
        && snapshot.availableVersion.isNotEmpty()
        && snapshot.phase != UpdatePhase::ready);
    downloadButton.setButtonText(
        snapshot.phase == UpdatePhase::failed
            && snapshot.availableVersion.isNotEmpty()
            ? "RETRY DOWNLOAD"
            : "DOWNLOAD");
    restartButton.setEnabled(
        snapshot.phase == UpdatePhase::ready);
}

SettingsComponent::SettingsComponent(
    StudioAudioDeviceManager* deviceManager,
    UpdateService& updateService,
    StudioPreferences& preferences,
    PluginCatalog& pluginCatalog,
    std::function<void(const PluginCatalogEntry&)>
        validatePlugin,
    std::function<void(const StudioThemePalette&)>
        themeChanged,
    std::function<void()> restartRequested,
    bool showUpdatesInitially,
    const juce::String& audioUnavailableReason)
{
    juce::Component* audioContent = &audioUnavailableLabel;
    if (deviceManager != nullptr)
    {
        deviceManager->prepareDeviceTypesForSettings();
        audioPage = std::make_unique<juce::AudioDeviceSelectorComponent>(
            *deviceManager,
            0,
            maximumHardwareAudioChannels,
            0,
            maximumHardwareAudioChannels,
            true,
            true,
            false,
            false);
        audioContent = audioPage.get();
    }
    else
    {
        audioUnavailableLabel.setText(
            audioUnavailableReason.isNotEmpty()
                ? audioUnavailableReason
                : "Audio is disabled. Close this window and open Settings again "
                  "to configure audio. Updates remain available.",
            juce::dontSendNotification);
        audioUnavailableLabel.setJustificationType(juce::Justification::centred);
        audioUnavailableLabel.setColour(
            juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    }
    updatePage = std::make_unique<UpdateSettingsComponent>(
        updateService,
        std::move(restartRequested));
    generalPage =
        std::make_unique<GeneralSettingsComponent>(
            preferences);
    appearancePage =
        std::make_unique<
            AppearanceSettingsComponent>(
            preferences,
            std::move(themeChanged));
    vstPage =
        std::make_unique<VstPluginSettingsComponent>(
            pluginCatalog,
            preferences,
            std::move(validatePlugin));

    addAndMakeVisible(tabs);
    tabs.setTabBarDepth(34);
    tabs.setOutline(1);
    tabs.addTab(
        "General",
        juce::Colour(StudioColours::panel),
        generalPage.get(),
        false);
    tabs.addTab(
        "Appearance",
        juce::Colour(StudioColours::panel),
        appearancePage.get(),
        false);
    tabs.addTab(
        "VST Plug-ins",
        juce::Colour(StudioColours::panel),
        vstPage.get(),
        false);
    tabs.addTab(
        "Audio / MIDI",
        juce::Colour(StudioColours::panel),
        audioContent,
        false);
    tabs.addTab(
        "Updates",
        juce::Colour(StudioColours::panel),
        updatePage.get(),
        false);
    if (showUpdatesInitially)
        showUpdates();
    setSize(680, 520);
}

void SettingsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void SettingsComponent::resized()
{
    tabs.setBounds(getLocalBounds());
}

void SettingsComponent::showUpdates()
{
    tabs.setCurrentTabIndex(4);
}
}

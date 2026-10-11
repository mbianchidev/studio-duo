#pragma once

#include "audio/StudioAudioEngine.h"
#include "model/ProjectModel.h"
#include "StudioIconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace studio
{
class PluginInsertPanel final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    PluginInsertPanel();
    ~PluginInsertPanel() override;
    void setProject(const Project* value);
    void setTrack(const juce::String& value);
    void setRuntimeStatuses(std::vector<StudioAudioEngine::PluginRuntimeStatus> value,
                            std::uint64_t lateBlocks, std::uint64_t droppedMidi = 0);

    std::function<void(const juce::String&, const juce::String&, bool)> onBypass;
    std::function<void(const juce::String&, const juce::String&)> onRemove;
    std::function<void(const juce::String&,
                       const juce::String&,
                       PluginBridgeMode)> onModeChange;
    std::function<void(const juce::String&, const juce::String&)> onReplace;
    std::function<void(const juce::String&, const juce::String&)> onEdit;
    std::function<void(const juce::String&, const juce::String&)>
        onOpenEditor;
    std::function<void(const juce::String&, const juce::String&)> onReload;
    std::function<void(const juce::String&, const juce::String&)> onAddPlugin;
    std::function<void(const juce::String&, std::vector<PluginInsert>, const juce::String&)>
        onSetChain;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class Item;
    class InsertTree final : public juce::TreeView
    {
    public:
        explicit InsertTree(PluginInsertPanel& panel) : owner(panel) {}
        void paint(juce::Graphics& graphics) override;
        bool keyPressed(const juce::KeyPress& key) override;
        void focusGained(juce::Component::FocusChangeType) override { owner.repaint(); }
        void focusLost(juce::Component::FocusChangeType) override { owner.repaint(); }

    private:
        PluginInsertPanel& owner;
    };

    [[nodiscard]] const Track* insertOwnerTrack() const;
    [[nodiscard]] const PluginInsert* selectedInsert() const;
    [[nodiscard]] juce::String addContainerId() const;
    void refresh();
    void updateButtons();
    void showAddMenu();
    void showInsertMenu();
    void createRack(bool parallel, const juce::String& containerId);
    void wrapSelected();
    void moveSelected(const juce::String& containerId);
    void reorderSelected(int direction);
    void editSelected(bool name);
    void bypassSelected();
    void removeSelected();
    void openSelected();
    void applyChain(std::vector<PluginInsert> inserts, const juce::String& description);

    const Project* project = nullptr;
    juce::String trackId;
    std::vector<StudioAudioEngine::PluginRuntimeStatus> runtimeStatuses;
    std::uint64_t lateBlockCount = 0;
    std::uint64_t droppedMidiCount = 0;
    InsertTree tree { *this };
    std::unique_ptr<Item> root;
    juce::Label emptyLabel;
    StudioIconButton addButton { StudioIcon::add, "Add plugin or rack", "Add a plugin, serial rack, or parallel splitter" };
    StudioIconButton bypassButton { StudioIcon::power, "Bypass selected insert", "Bypass or enable the selected plugin or rack" };
    StudioIconButton removeButton { StudioIcon::close, "Remove selected insert", "Remove the selected plugin or rack and its children" };
    StudioIconButton menuButton { StudioIcon::settings, "Insert actions", "Edit, move, rename, or adjust the selected insert" };
};
}

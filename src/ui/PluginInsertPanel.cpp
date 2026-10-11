#include "PluginInsertPanel.h"

#include "NumericInput.h"
#include "StudioTheme.h"
#include "model/PluginRack.h"

#include <algorithm>

namespace studio
{
class PluginInsertPanel::Item final : public juce::TreeViewItem
{
public:
    Item(PluginInsertPanel& panel, juce::String insertId)
        : owner(panel), id(std::move(insertId)) {}

    const PluginInsert* insert() const
    {
        const auto* track = owner.insertOwnerTrack();
        return track != nullptr ? PluginRack::find(track->inserts, id) : nullptr;
    }

    bool mightContainSubItems() override
    {
        const auto* value = insert();
        return id.isEmpty() || (value != nullptr && value->isContainer());
    }

    bool canBeSelected() const override { return id.isNotEmpty(); }
    juce::String getUniqueName() const override { return id.isEmpty() ? juce::String("inserts") : id; }
    int getItemHeight() const override { return 50; }

    juce::String getTooltip() override
    {
        const auto* value = insert();
        return value != nullptr
            ? value->name + ". Enter opens; Space bypasses; Shift+F10 shows actions."
            : juce::String();
    }

    juce::String getAccessibilityName() override
    {
        const auto* value = insert();
        return value != nullptr ? value->name : juce::String("Inserts");
    }

    void paintItem(juce::Graphics& graphics, int width, int height) override
    {
        const auto* value = insert();
        const auto* track = owner.insertOwnerTrack();
        if (value == nullptr || track == nullptr)
            return;
        const auto bypassed = PluginRack::isBypassed(track->inserts, *value);
        const auto row = juce::Rectangle<float>(0.0f, 1.0f,
            static_cast<float>(width - 4), static_cast<float>(height - 5));
        graphics.setColour(juce::Colour(bypassed ? StudioColours::panel : StudioColours::raised));
        graphics.fillRoundedRectangle(row, 4.0f);
        graphics.setColour(juce::Colour(isSelected() ? StudioColours::orange : StudioColours::border));
        graphics.drawRoundedRectangle(row, 4.0f, isSelected() ? 2.0f : 1.0f);
        graphics.setColour(juce::Colour(bypassed ? StudioColours::secondaryText : StudioColours::text));
        graphics.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
        graphics.drawFittedText(value->name, 8, 4, width - 20, 18,
                                juce::Justification::centredLeft, 1);
        auto detail = value->kind == PluginInsertKind::parallelSplitter ? juce::String("PARALLEL")
            : value->isContainer() ? juce::String("SERIAL RACK")
            : value->bridgeMode == PluginBridgeMode::sandboxed ? juce::String("SANDBOX")
            : value->bridgeMode == PluginBridgeMode::araCompatibility ? juce::String("ARA 2")
            : juce::String("TRUSTED");
        auto colour = juce::Colour(StudioColours::secondaryText);
        if (value->isContainer())
            detail << " | " << formatDecibels(value->gainDecibels);
        else if (value->format.isNotEmpty())
            detail << " | " << value->format;
        const auto status = std::find_if(owner.runtimeStatuses.cbegin(), owner.runtimeStatuses.cend(),
            [this](const auto& candidate) { return candidate.insertId == id; });
        if (bypassed)
            detail << " | BYPASSED";
        else if (value->recoveryDisabled)
        {
            detail << " | RECOVERY DISABLED";
            colour = juce::Colour(StudioColours::orange);
        }
        else if (status != owner.runtimeStatuses.cend())
        {
            using State = StudioAudioEngine::PluginRuntimeStatus::State;
            switch (status->state)
            {
                case State::ready: detail << " | READY"; colour = juce::Colour(StudioColours::green); break;
                case State::failed: detail << " | FAILED"; colour = juce::Colour(StudioColours::orange); break;
                case State::missing: detail << " | MISSING"; colour = juce::Colour(StudioColours::orange); break;
                case State::loading: detail << " | LOADING"; break;
                case State::bypassed: detail << " | BYPASSED"; break;
            }
        }
        if (owner.trackId != track->id)
            detail = "INHERITED | " + detail;
        graphics.setColour(colour);
        graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
        graphics.drawFittedText(detail, 8, 25, width - 20, 14,
                                juce::Justification::centredLeft, 1);
    }

    void itemSelectionChanged(bool) override { owner.updateButtons(); }

    void itemClicked(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            setSelected(true, true);
            owner.showInsertMenu();
        }
    }

    void itemDoubleClicked(const juce::MouseEvent&) override
    {
        setSelected(true, true);
        owner.openSelected();
    }

    PluginInsertPanel& owner;
    juce::String id;
};

PluginInsertPanel::PluginInsertPanel()
{
    tree.setComponentID("pluginRackTree");
    tree.setTitle("Plugin insert rack tree");
    tree.setRootItemVisible(false);
    tree.setMultiSelectEnabled(false);
    tree.setDefaultOpenness(true);
    tree.setIndentSize(14);
    tree.setColour(juce::TreeView::backgroundColourId, juce::Colour(StudioColours::panel));
    tree.setWantsKeyboardFocus(true);
    addAndMakeVisible(tree);
    emptyLabel.setText("Use + to add a plugin, rack, or parallel splitter.", juce::dontSendNotification);
    emptyLabel.setColour(juce::Label::textColourId, juce::Colour(StudioColours::secondaryText));
    emptyLabel.setJustificationType(juce::Justification::topLeft);
    emptyLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(emptyLabel);
    for (auto* button : { &addButton, &bypassButton, &removeButton, &menuButton })
        addAndMakeVisible(*button);
    addButton.onClick = [this] { showAddMenu(); };
    bypassButton.onClick = [this] { bypassSelected(); };
    removeButton.onClick = [this] { removeSelected(); };
    menuButton.onClick = [this] { showInsertMenu(); };
    updateButtons();
}

PluginInsertPanel::~PluginInsertPanel()
{
    tree.setRootItem(nullptr);
}

const Track* PluginInsertPanel::insertOwnerTrack() const
{
    const auto* track = project != nullptr ? project->findTrack(trackId) : nullptr;
    return track != nullptr && track->parentTrackId.isNotEmpty()
        ? project->findTrack(track->parentTrackId) : track;
}

const PluginInsert* PluginInsertPanel::selectedInsert() const
{
    const auto* selected = dynamic_cast<Item*>(tree.getSelectedItem(0));
    return selected != nullptr ? selected->insert() : nullptr;
}

juce::String PluginInsertPanel::addContainerId() const
{
    const auto* selected = selectedInsert();
    return selected != nullptr
        ? selected->isContainer() ? selected->id : selected->containerId
        : juce::String();
}

void PluginInsertPanel::setProject(const Project* value)
{
    project = value;
    refresh();
}

void PluginInsertPanel::setTrack(const juce::String& value)
{
    if (trackId == value)
        return;
    trackId = value;
    tree.setRootItem(nullptr);
    root.reset();
    refresh();
}

void PluginInsertPanel::refresh()
{
    const auto openness = tree.getOpennessState(true);
    tree.setRootItem(nullptr);
    root = std::make_unique<Item>(*this, juce::String());
    if (const auto* track = insertOwnerTrack())
    {
        const auto add = [&](const auto& self, Item& parent, const juce::String& parentId) -> void
        {
            for (const auto& insert : track->inserts)
            {
                if (insert.containerId != parentId)
                    continue;
                auto item = std::make_unique<Item>(*this, insert.id);
                if (insert.isContainer())
                    self(self, *item, insert.id);
                parent.addSubItem(item.release());
            }
        };
        add(add, *root, {});
    }
    tree.setRootItem(root.get());
    root->setOpen(true);
    if (openness != nullptr)
        tree.restoreOpennessState(*openness, true);
    const auto* track = insertOwnerTrack();
    emptyLabel.setVisible(track == nullptr || track->inserts.empty());
    updateButtons();
    repaint();
}

void PluginInsertPanel::updateButtons()
{
    addButton.setEnabled(insertOwnerTrack() != nullptr);
    const auto* selected = selectedInsert();
    bypassButton.setEnabled(selected != nullptr);
    removeButton.setEnabled(selected != nullptr);
    menuButton.setEnabled(selected != nullptr);
    if (selected != nullptr)
    {
        bypassButton.setAccessibleLabel((selected->bypassed ? "Enable " : "Bypass ") + selected->name);
        bypassButton.setToggleState(!selected->bypassed, juce::dontSendNotification);
    }
}

void PluginInsertPanel::setRuntimeStatuses(
    std::vector<StudioAudioEngine::PluginRuntimeStatus> value,
    std::uint64_t lateBlocks, std::uint64_t droppedMidi)
{
    runtimeStatuses = std::move(value);
    lateBlockCount = lateBlocks;
    droppedMidiCount = droppedMidi;
    tree.repaint();
    repaint();
}

void PluginInsertPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
    graphics.setColour(juce::Colour(StudioColours::secondaryText));
    graphics.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
    graphics.drawText("INSERTS", 0, 0, getWidth() - 128, 26, juce::Justification::centredLeft);
    if (lateBlockCount != 0 || droppedMidiCount != 0)
    {
        graphics.setColour(juce::Colour(StudioColours::amber));
        graphics.setFont(juce::Font(juce::FontOptions(9.0f)));
        graphics.drawFittedText(juce::String(lateBlockCount) + " late blocks | "
            + juce::String(droppedMidiCount) + " dropped rack MIDI", 0, getHeight() - 18,
            getWidth(), 18, juce::Justification::centredLeft, 1);
    }
}

void PluginInsertPanel::resized()
{
    auto buttons = getLocalBounds().removeFromTop(26);
    menuButton.setBounds(buttons.removeFromRight(30).reduced(1));
    removeButton.setBounds(buttons.removeFromRight(30).reduced(1));
    bypassButton.setBounds(buttons.removeFromRight(30).reduced(1));
    addButton.setBounds(buttons.removeFromRight(30).reduced(1));
    tree.setBounds(0, 30, getWidth(), std::max(0, getHeight() - 50));
    emptyLabel.setBounds(tree.getBounds().reduced(6));
}

void PluginInsertPanel::applyChain(std::vector<PluginInsert> inserts,
                                   const juce::String& description)
{
    if (const auto* track = insertOwnerTrack(); track != nullptr && onSetChain)
        onSetChain(track->id, std::move(inserts), description);
}

void PluginInsertPanel::createRack(bool parallel, const juce::String& containerId)
{
    const auto* track = insertOwnerTrack();
    if (track == nullptr)
        return;
    auto inserts = track->inserts;
    PluginInsert rack;
    rack.kind = parallel ? PluginInsertKind::parallelSplitter : PluginInsertKind::serialRack;
    rack.name = parallel ? "Parallel splitter" : "Plugin rack";
    rack.containerId = containerId;
    inserts.push_back(rack);
    if (parallel)
        for (int branch = 1; branch <= 2; ++branch)
        {
            PluginInsert child;
            child.kind = PluginInsertKind::serialRack;
            child.name = "Branch " + juce::String(branch);
            child.containerId = rack.id;
            child.gainDecibels = -6.0206f;
            inserts.push_back(std::move(child));
        }
    applyChain(std::move(inserts), parallel ? "Create parallel splitter" : "Create plugin rack");
}

void PluginInsertPanel::showAddMenu()
{
    const auto* track = insertOwnerTrack();
    if (track == nullptr)
        return;
    const auto parent = addContainerId();
    const auto* container = PluginRack::find(track->inserts, parent);
    const auto branchOnly = container != nullptr && container->kind == PluginInsertKind::parallelSplitter;
    const juce::Component::SafePointer<PluginInsertPanel> safe(this);
    juce::PopupMenu menu;
    menu.addItem("Add plugin here", !branchOnly, false, [safe, trackIdToUse = track->id, parent]
    {
        if (safe != nullptr && safe->onAddPlugin)
            safe->onAddPlugin(trackIdToUse, parent);
    });
    menu.addItem(branchOnly ? "Add serial branch" : "Create serial rack", [safe, parent]
    {
        if (safe != nullptr)
            safe->createRack(false, parent);
    });
    menu.addItem("Create parallel splitter", !branchOnly, false, [safe, parent]
    {
        if (safe != nullptr)
            safe->createRack(true, parent);
    });
    if (parent.isNotEmpty())
        menu.addItem("Create rack at track level", [safe]
        {
            if (safe != nullptr)
                safe->createRack(false, {});
        });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton));
}

void PluginInsertPanel::bypassSelected()
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected != nullptr && track != nullptr && onBypass)
        onBypass(track->id, selected->id, !selected->bypassed);
}

void PluginInsertPanel::removeSelected()
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected != nullptr && track != nullptr && onRemove)
        onRemove(track->id, selected->id);
}

void PluginInsertPanel::openSelected()
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected == nullptr || track == nullptr)
        return;
    if (selected->isContainer())
    {
        if (auto* item = tree.getSelectedItem(0))
            item->setOpen(!item->isOpen());
    }
    else if (onOpenEditor)
        onOpenEditor(track->id, selected->id);
}

void PluginInsertPanel::wrapSelected()
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected == nullptr || track == nullptr)
        return;
    auto inserts = track->inserts;
    const auto index = static_cast<std::size_t>(selected - track->inserts.data());
    PluginInsert rack;
    rack.kind = PluginInsertKind::serialRack;
    rack.name = selected->name + " rack";
    rack.containerId = selected->containerId;
    inserts[index].containerId = rack.id;
    inserts.insert(inserts.begin() + static_cast<std::ptrdiff_t>(index), rack);
    applyChain(std::move(inserts), "Wrap insert in rack");
}

void PluginInsertPanel::moveSelected(const juce::String& containerId)
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected == nullptr || track == nullptr)
        return;
    auto inserts = track->inserts;
    for (auto& insert : inserts)
        if (insert.id == selected->id)
            insert.containerId = containerId;
    applyChain(std::move(inserts), "Move insert");
}

void PluginInsertPanel::reorderSelected(int direction)
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected == nullptr || track == nullptr)
        return;
    std::vector<std::size_t> siblings;
    for (std::size_t index = 0; index < track->inserts.size(); ++index)
        if (track->inserts[index].containerId == selected->containerId)
            siblings.push_back(index);
    const auto index = static_cast<std::size_t>(selected - track->inserts.data());
    const auto position = std::find(siblings.cbegin(), siblings.cend(), index);
    const auto next = static_cast<int>(std::distance(siblings.cbegin(), position)) + direction;
    if (next < 0 || next >= static_cast<int>(siblings.size()))
        return;
    auto inserts = track->inserts;
    std::swap(inserts[index], inserts[siblings[static_cast<std::size_t>(next)]]);
    applyChain(std::move(inserts), "Reorder insert");
}

void PluginInsertPanel::editSelected(bool editName)
{
    const auto* selected = selectedInsert();
    if (selected == nullptr)
        return;
    const auto id = selected->id;
    auto* dialog = new juce::AlertWindow(editName ? "Rename rack" : "Rack output level",
        editName ? "Enter a rack or branch name." : "Enter a level between -60 and +12 dB.",
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("value", editName ? selected->name : formatDecibels(selected->gainDecibels));
    dialog->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->centreAroundComponent(this, 360, 160);
    const juce::Component::SafePointer<PluginInsertPanel> safe(this);
    const juce::Component::SafePointer<juce::AlertWindow> safeDialog(dialog);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe, safeDialog, id, editName](int result)
        {
            if (result != 1 || safe == nullptr || safeDialog == nullptr)
                return;
            const auto* track = safe->insertOwnerTrack();
            if (track == nullptr)
                return;
            auto inserts = track->inserts;
            const auto found = std::find_if(inserts.begin(), inserts.end(),
                [&id](const auto& insert) { return insert.id == id; });
            if (found == inserts.end())
                return;
            const auto text = safeDialog->getTextEditorContents("value").trim();
            if (editName && text.isNotEmpty())
                found->name = text;
            else if (!editName && parseTrackDecibels(text).has_value())
                found->gainDecibels = *parseTrackDecibels(text);
            else
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Invalid rack setting", editName ? "Rack names cannot be blank."
                        : "Enter a finite number between -60 and +12 dB.");
                return;
            }
            safe->applyChain(std::move(inserts), editName ? "Rename rack" : "Set rack output level");
        }), true);
}

void PluginInsertPanel::showInsertMenu()
{
    const auto* selected = selectedInsert();
    const auto* track = insertOwnerTrack();
    if (selected == nullptr || track == nullptr)
        return;
    const juce::Component::SafePointer<PluginInsertPanel> safe(this);
    juce::PopupMenu menu;
    if (selected->isContainer())
    {
        menu.addItem("Add inside rack", [safe] { if (safe != nullptr) safe->showAddMenu(); });
        menu.addItem("Rename", [safe] { if (safe != nullptr) safe->editSelected(true); });
        menu.addItem("Output level...", [safe] { if (safe != nullptr) safe->editSelected(false); });
    }
    else
    {
        const auto trackIdToUse = track->id;
        const auto insertId = selected->id;
        menu.addItem("Open plugin editor", !selected->missing && !selected->recoveryDisabled, false,
            [safe, trackIdToUse, insertId]
            { if (safe != nullptr && safe->onOpenEditor) safe->onOpenEditor(trackIdToUse, insertId); });
        menu.addItem("Open parameter editor", [safe, trackIdToUse, insertId]
            { if (safe != nullptr && safe->onEdit) safe->onEdit(trackIdToUse, insertId); });
        menu.addItem("Reload plugin", [safe, trackIdToUse, insertId]
            { if (safe != nullptr && safe->onReload) safe->onReload(trackIdToUse, insertId); });
        menu.addItem("Replace plugin...", [safe, trackIdToUse, insertId]
            { if (safe != nullptr && safe->onReplace) safe->onReplace(trackIdToUse, insertId); });
        menu.addSeparator();
        for (const auto mode : { PluginBridgeMode::sandboxed, PluginBridgeMode::trustedInProcess,
                                 PluginBridgeMode::araCompatibility })
        {
            const auto enabled = mode == PluginBridgeMode::trustedInProcess
                || (!selected->bundledDevice && (mode != PluginBridgeMode::araCompatibility || selected->araCapable));
            menu.addItem(mode == PluginBridgeMode::sandboxed ? "Sandboxed DSP"
                : mode == PluginBridgeMode::araCompatibility ? "ARA 2 (reduced isolation)" : "Trusted in-process",
                enabled, selected->bridgeMode == mode, [safe, trackIdToUse, insertId, mode]
                { if (safe != nullptr && safe->onModeChange) safe->onModeChange(trackIdToUse, insertId, mode); });
        }
    }
    menu.addSeparator();
    menu.addItem(selected->bypassed ? "Enable" : "Bypass", [safe]
        { if (safe != nullptr) safe->bypassSelected(); });
    menu.addItem("Wrap in serial rack", [safe] { if (safe != nullptr) safe->wrapSelected(); });
    menu.addItem("Move earlier", [safe] { if (safe != nullptr) safe->reorderSelected(-1); });
    menu.addItem("Move later", [safe] { if (safe != nullptr) safe->reorderSelected(1); });
    juce::PopupMenu destinations;
    destinations.addItem("Track level", selected->containerId.isNotEmpty(), false,
        [safe] { if (safe != nullptr) safe->moveSelected({}); });
    for (const auto& container : track->inserts)
        if (container.kind == PluginInsertKind::serialRack
            && container.id != selected->containerId
            && !PluginRack::contains(track->inserts, selected->id, container))
            destinations.addItem(container.name, [safe, parent = container.id]
                { if (safe != nullptr) safe->moveSelected(parent); });
    menu.addSubMenu("Move into...", destinations);
    menu.addSeparator();
    menu.addItem("Remove", [safe] { if (safe != nullptr) safe->removeSelected(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton));
}

bool PluginInsertPanel::InsertTree::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress(juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier, 0))
        owner.showInsertMenu();
    else if (key == juce::KeyPress(juce::KeyPress::returnKey))
        owner.openSelected();
    else if (key == juce::KeyPress(juce::KeyPress::spaceKey))
        owner.bypassSelected();
    else if (key == juce::KeyPress(juce::KeyPress::deleteKey)
             || key == juce::KeyPress(juce::KeyPress::backspaceKey))
        owner.removeSelected();
    else
        return juce::TreeView::keyPressed(key);
    return true;
}

void PluginInsertPanel::InsertTree::paint(juce::Graphics& graphics)
{
    juce::TreeView::paint(graphics);
    if (hasKeyboardFocus(false))
    {
        graphics.setColour(juce::Colour(StudioColours::orange));
        graphics.drawRect(getLocalBounds(), 2);
    }
}
}

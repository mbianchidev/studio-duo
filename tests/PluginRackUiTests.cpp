#include "TestHarness.h"
#include "TestSuites.h"

#include "ui/PluginInsertPanel.h"

void pluginRackUiTests()
{
    auto project = studio::Project::createDefault();
    studio::PluginInsert rack;
    rack.name = "Instrument rack";
    rack.kind = studio::PluginInsertKind::serialRack;
    studio::PluginInsert plugin;
    plugin.name = "Mock instrument";
    plugin.pluginIdentifier = "mock.instrument";
    plugin.containerId = rack.id;
    project.tracks.front().inserts = { rack, plugin };

    studio::PluginInsertPanel panel;
    panel.setSize(320, 360);
    panel.setProject(&project);
    panel.setTrack(project.tracks.front().id);
    auto* tree = dynamic_cast<juce::TreeView*>(panel.findChildWithID("pluginRackTree"));
    expect(tree != nullptr && tree->getRootItem() != nullptr
               && tree->getRootItem()->getNumSubItems() == 1
               && tree->getRootItem()->getSubItem(0)->getNumSubItems() == 1,
           "The insert inspector exposes actual nested racks in an accessible native tree.");
    if (tree == nullptr || tree->getRootItem() == nullptr
        || tree->getRootItem()->getNumSubItems() != 1)
        return;
    auto* rackItem = tree->getRootItem()->getSubItem(0);
    rackItem->setOpen(true);
    auto* child = rackItem->getSubItem(0);
    child->setSelected(true, true);
    bool bypassRequested = false;
    panel.onBypass = [&](const auto& trackId, const auto& insertId, bool bypassed)
    {
        bypassRequested = trackId == project.tracks.front().id
            && insertId == plugin.id && bypassed;
    };
    tree->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
    expect(bypassRequested, "Keyboard bypass targets the selected nested processor.");
    panel.setProject(&project);
    expect(tree->getSelectedItem(0) != nullptr
               && tree->getSelectedItem(0)->getUniqueName() == plugin.id,
           "Refreshing insert state preserves keyboard selection inside a rack.");
}

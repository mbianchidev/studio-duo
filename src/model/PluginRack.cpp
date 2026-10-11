#include "PluginRack.h"

#include <algorithm>
#include <cmath>

namespace studio
{
const PluginInsert* PluginRack::find(const std::vector<PluginInsert>& inserts,
                                     const juce::String& id)
{
    const auto found = std::find_if(inserts.cbegin(), inserts.cend(),
        [&id](const auto& insert) { return insert.id == id; });
    return found == inserts.cend() ? nullptr : &*found;
}

bool PluginRack::isBypassed(const std::vector<PluginInsert>& inserts,
                            const PluginInsert& insert)
{
    auto* current = &insert;
    for (auto depth = 0; current != nullptr && depth <= maximumDepth; ++depth)
    {
        if (current->bypassed)
            return true;
        if (current->containerId.isEmpty())
            return false;
        current = find(inserts, current->containerId);
    }
    return false;
}

bool PluginRack::contains(const std::vector<PluginInsert>& inserts,
                          const juce::String& containerId,
                          const PluginInsert& insert)
{
    auto* current = &insert;
    for (auto depth = 0; current != nullptr && depth <= maximumDepth; ++depth)
    {
        if (current->id == containerId)
            return true;
        if (current->containerId.isEmpty())
            return false;
        current = find(inserts, current->containerId);
    }
    return false;
}

PluginRack::Timing PluginRack::measure(
    const std::vector<PluginInsert>& inserts, const TimingLookup& lookup,
    std::map<juce::String, std::int64_t>* inputLatencies,
    const juce::String& containerId)
{
    if (inputLatencies != nullptr)
        inputLatencies->clear();
    const auto chain = [&](const auto& self, const juce::String& parentId,
                           bool parallel, std::int64_t inputLatency) -> Timing
    {
        Timing total;
        for (const auto& insert : inserts)
        {
            if (insert.containerId != parentId)
                continue;
            const auto start = inputLatency + (parallel ? 0 : total.latencySamples);
            if (inputLatencies != nullptr)
                (*inputLatencies)[insert.id] = start;
            if (insert.bypassed)
                continue;
            const auto timing = insert.isContainer()
                ? self(self, insert.id,
                       insert.kind == PluginInsertKind::parallelSplitter, start)
                : lookup ? lookup(insert)
                         : insert.missing ? Timing {}
                         : Timing { std::max(0, insert.latencySamples),
                                    std::max(0.0, insert.tailSeconds) };
            if (parallel)
            {
                total.latencySamples = std::max(total.latencySamples, timing.latencySamples);
                total.tailSeconds = std::max(total.tailSeconds, timing.tailSeconds);
            }
            else
            {
                total.latencySamples += timing.latencySamples;
                total.tailSeconds += timing.tailSeconds;
            }
        }
        return total;
    };
    const auto* container = find(inserts, containerId);
    if (container != nullptr && isBypassed(inserts, *container))
        return {};
    return chain(chain, containerId,
                 container != nullptr && container->kind == PluginInsertKind::parallelSplitter, 0);
}

bool PluginRack::validate(const std::vector<PluginInsert>& inserts,
                          juce::String& error)
{
    std::vector<juce::String> ids;
    for (const auto& insert : inserts)
    {
        if (insert.id.isEmpty()
            || std::find(ids.cbegin(), ids.cend(), insert.id) != ids.cend())
        {
            error = "Insert IDs must be non-empty and unique within a chain.";
            return false;
        }
        ids.push_back(insert.id);
        if (!std::isfinite(insert.gainDecibels)
            || insert.gainDecibels < -60.0f || insert.gainDecibels > 12.0f)
        {
            error = "Rack output level must be between -60 and +12 dB.";
            return false;
        }
        auto parentId = insert.containerId;
        std::vector<juce::String> ancestors { insert.id };
        while (parentId.isNotEmpty())
        {
            if (std::find(ancestors.cbegin(), ancestors.cend(), parentId)
                != ancestors.cend())
            {
                error = "Rack containment cannot contain a cycle.";
                return false;
            }
            if (ancestors.size() > static_cast<std::size_t>(maximumDepth))
            {
                error = "Rack nesting exceeds the supported depth of "
                    + juce::String(maximumDepth) + ".";
                return false;
            }
            const auto* parent = find(inserts, parentId);
            if (parent == nullptr || !parent->isContainer())
            {
                error = "An insert's rack must be an existing container on the same track.";
                return false;
            }
            if (ancestors.size() == 1
                && parent->kind == PluginInsertKind::parallelSplitter
                && insert.kind != PluginInsertKind::serialRack)
            {
                error = "Parallel splitters contain serial rack branches.";
                return false;
            }
            ancestors.push_back(parentId);
            parentId = parent->containerId;
        }
        if (insert.kind == PluginInsertKind::parallelSplitter
            && std::count_if(
                   inserts.cbegin(), inserts.cend(),
                   [&insert](const auto& child)
                   { return child.containerId == insert.id; }) < 2)
        {
            error = "A parallel splitter requires at least two serial branches.";
            return false;
        }
    }
    return true;
}
}

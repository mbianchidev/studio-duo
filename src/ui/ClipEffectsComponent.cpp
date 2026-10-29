#include "ClipEffectsComponent.h"

#include "PluginParameterPanel.h"
#include "StudioTheme.h"
#include "logging/StudioLogger.h"
#include "model/PluginRack.h"
#include "util/NumberParsing.h"

namespace studio
{
ClipEffectsComponent::ClipEffectsComponent(const Project& context, const AudioClip& clip,
                                           PluginCatalog& plugins, RenderEngine::RequestBuilder builder)
    : catalog(plugins), requestBuilder(std::move(builder)),
      engine(std::make_unique<StudioAudioEngine>()), clipDuration(clip.durationSeconds)
{
    setSize(580, 540);
    setTitle("AudioSuite clip effects");
    effectsProject.tempo = context.tempo;
    effectsProject.tempoChanges = context.tempoChanges;
    effectsProject.meterChanges = context.meterChanges;
    effectsProject.sections = context.sections;
    effectsProject.timeSignatureNumerator = context.timeSignatureNumerator;
    effectsProject.timeSignatureDenominator = context.timeSignatureDenominator;
    effectsProject.metronomeEnabled = false;
    Track effects;
    effects.name = "Clip FX";
    Track master;
    master.type = TrackType::master;
    master.name = "Clip FX master";
    effectsProject.tracks = { effects, master };
    for (auto* component : std::array<juce::Component*, 10> {
            &title, &rangeLabel, &tailLabel, &start, &end, &tail, &rack, &status, &apply, &cancel })
        addAndMakeVisible(*component);
    title.setText("AudioSuite: " + clip.name, juce::dontSendNotification);
    rangeLabel.setText("Clip-relative range, seconds: start / end", juce::dontSendNotification);
    tailLabel.setText("Effects tail, seconds (0 - 30)", juce::dontSendNotification);
    start.setText("0", false);
    end.setText(formatRoundTripNumber(clip.durationSeconds), false);
    tail.setText("0.5", false);
    for (const auto& [field, name] : {
            std::pair { &start, "Clip FX range start" }, std::pair { &end, "Clip FX range end" },
            std::pair { &tail, "Clip FX effects tail" } })
    {
        field->setTitle(name);
        field->setComponentID(juce::String(name).replaceCharacter(' ', '-'));
        field->setWantsKeyboardFocus(true);
        field->setSelectAllWhenFocused(true);
    }
    status.setTitle("Clip FX status");
    status.setJustificationType(juce::Justification::topLeft);
    apply.setTitle("Render effects onto selected clip range");
    apply.setEnabled(false);
    apply.onClick = [this] { juce::ignoreUnused(applyChanges()); };
    cancel.onClick = [this] { if (onClose) onClose(); };
    rack.setProject(&effectsProject);
    rack.setTrack(effects.id);
    rack.onAddPlugin = [this](const auto&, const auto& container) { choosePlugin(container); };
    rack.onReplace = [this](const auto&, const auto& id)
    {
        const auto* insert = PluginRack::find(effectsProject.tracks.front().inserts, id);
        if (insert != nullptr)
            choosePlugin(insert->containerId, id);
    };
    rack.onSetChain = [this](const auto&, auto chain, const auto&) { setChain(std::move(chain)); };
    rack.onBypass = [this](const auto&, const auto& id, bool bypassed)
    {
        auto chain = effectsProject.tracks.front().inserts;
        for (auto& insert : chain)
            if (insert.id == id)
                insert.bypassed = bypassed;
        setChain(std::move(chain));
    };
    rack.onRemove = [this](const auto&, const auto& id)
    {
        const auto original = effectsProject.tracks.front().inserts;
        auto chain = original;
        std::erase_if(chain, [&](const auto& insert)
        {
            return insert.id == id || PluginRack::contains(original, id, insert);
        });
        setChain(std::move(chain));
    };
    rack.onEdit = [this](const auto&, const auto& id) { editParameters(id); };
    rack.onOpenEditor = [this](const auto&, const auto& id)
    {
        if (const auto result = engine->showPluginEditor(id); result.failed())
        {
            showStatus("Native editor unavailable: " + result.getErrorMessage() + " Opening parameters.");
            editParameters(id);
        }
    };
    rack.onReload = [this](const auto&, const auto& id)
    {
        if (!captureStates())
            return;
        if (const auto result = engine->forcePluginRuntimeReload(effectsProject, requests(), id); result.failed())
            showStatus(result.getErrorMessage(), true);
    };
    rack.onModeChange = [this](const auto&, const auto&, PluginBridgeMode)
    {
        showStatus("Clip FX use sandboxed third-party processing and trusted bundled DSP. "
                   "Reduced-isolation track modes are not available in AudioSuite.", true);
    };
    showStatus("Add an effect, edit its parameters, then render. Original audio and track inserts remain unchanged.");
    startTimerHz(10);
}

ClipEffectsComponent::~ClipEffectsComponent()
{
    stopTimer();
    engine->shutdown();
}

void ClipEffectsComponent::showStatus(const juce::String& text, bool error)
{
    const auto message = (error ? "Error: " : "") + text;
    if (status.getText() == message)
        return;
    status.setText(message, juce::dontSendNotification);
    if (error)
        logError("clip-effects", text);
}

bool ClipEffectsComponent::captureStates()
{
    if (engine->pluginRuntimeTransitionPending())
    {
        showStatus("Wait for the clip effects to finish loading.", true);
        return false;
    }
    for (const auto& state : engine->capturePluginStates(2000))
    {
        if (state.result.failed() && (!state.preservePreviousState || !states.contains(state.insertId)))
        {
            showStatus(state.result.getErrorMessage(), true);
            return false;
        }
        if (state.result.wasOk() && state.insertId.isNotEmpty())
            states[state.insertId] = state.state;
    }
    return true;
}

std::vector<StudioAudioEngine::PluginRuntimeRequest> ClipEffectsComponent::requests() const
{
    auto result = requestBuilder(effectsProject);
    for (auto& request : result)
        if (const auto state = states.find(request.insertId); state != states.end())
            request.state = state->second;
    return result;
}

void ClipEffectsComponent::setChain(std::vector<PluginInsert> chain)
{
    juce::String error;
    if (!PluginRack::validate(chain, error))
    {
        showStatus(error, true);
        return;
    }
    if (!captureStates())
        return;
    for (const auto& insert : chain)
        if (const auto* old = PluginRack::find(effectsProject.tracks.front().inserts, insert.id);
            old != nullptr && old->pluginIdentifier != insert.pluginIdentifier)
            states.erase(insert.id);
    effectsProject.tracks.front().inserts = std::move(chain);
    rebuildRuntime();
}

void ClipEffectsComponent::rebuildRuntime()
{
    apply.setEnabled(false);
    rack.setProject(&effectsProject);
    if (const auto result = engine->updateProject(effectsProject, requests()); result.failed())
        showStatus(result.getErrorMessage(), true);
}

void ClipEffectsComponent::choosePlugin(juce::String container, juce::String replacement)
{
    auto browser = std::make_unique<PluginBrowserComponent>(catalog);
    const auto safe = juce::Component::SafePointer<ClipEffectsComponent>(this);
    const auto browserSafe = juce::Component::SafePointer<PluginBrowserComponent>(browser.get());
    browser->setSize(440, 460);
    browser->onPluginActivated = [safe, browserSafe, container, replacement](const PluginCatalogEntry& entry)
    {
        if (safe == nullptr)
            return;
        if (entry.instrument || entry.inputChannels < 1)
        {
            safe->showStatus("Choose an audio effect, not an instrument or signal generator.", true);
            return;
        }
        PluginInsert insert;
        insert.name = entry.name;
        insert.pluginIdentifier = entry.identifier;
        insert.manufacturer = entry.manufacturer;
        insert.format = entry.format;
        insert.version = entry.version;
        insert.architecture = entry.architecture;
        insert.fileOrIdentifier = entry.fileOrIdentifier;
        insert.bundledDevice = entry.bundledDevice;
        insert.bridgeMode = entry.bundledDevice ? PluginBridgeMode::trustedInProcess : PluginBridgeMode::sandboxed;
        insert.containerId = container;
        auto chain = safe->effectsProject.tracks.front().inserts;
        if (replacement.isNotEmpty())
        {
            insert.id = replacement;
            for (auto& current : chain)
                if (current.id == replacement)
                    current = insert;
        }
        else
            chain.push_back(insert);
        safe->setChain(std::move(chain));
        if (browserSafe != nullptr)
            if (auto* callout = browserSafe->findParentComponentOfClass<juce::CallOutBox>())
                callout->dismiss();
    };
    juce::CallOutBox::launchAsynchronously(std::move(browser), rack.getScreenBounds(), nullptr);
}

void ClipEffectsComponent::editParameters(const juce::String& id)
{
    const auto statuses = engine->pluginRuntimeStatuses();
    const auto found = std::find_if(statuses.begin(), statuses.end(),
                                    [&](const auto& item) { return item.insertId == id; });
    if (found == statuses.end() || found->state != StudioAudioEngine::PluginRuntimeStatus::State::ready)
    {
        showStatus("Wait for the effect to load before editing its parameters.", true);
        return;
    }
    auto parameters = std::make_unique<PluginParameterPanel>(found->name, id, found->parameters);
    const auto safe = juce::Component::SafePointer<ClipEffectsComponent>(this);
    parameters->onValueChanged = [safe](const auto& insertId, const auto&, const auto&, int index, float value)
    {
        if (safe == nullptr)
            return;
        juce::String error;
        if (!safe->engine->setPluginParameter(insertId, index, value, error))
            safe->showStatus(error, true);
    };
    juce::CallOutBox::launchAsynchronously(std::move(parameters), rack.getScreenBounds(), nullptr);
}

juce::Result ClipEffectsComponent::applyChanges()
{
    const auto first = parseFiniteNumber(start.getText());
    const auto last = parseFiniteNumber(end.getText());
    const auto seconds = parseFiniteNumber(tail.getText());
    if (!first || !last || !seconds || *first < 0.0 || *last <= *first
        || *last > clipDuration || *seconds < 0.0 || *seconds > 30.0)
    {
        const auto error = "Enter a finite start and end inside this clip and a tail between 0 and 30 seconds.";
        showStatus(error, true);
        return juce::Result::fail(error);
    }
    const auto& inserts = effectsProject.tracks.front().inserts;
    if (std::none_of(inserts.begin(), inserts.end(),
        [&](const auto& insert) { return !insert.isContainer() && !PluginRack::isBypassed(inserts, insert); }))
    {
        showStatus("Add and enable at least one audio effect.", true);
        return juce::Result::fail("No active clip effect.");
    }
    if (!effectsReady())
    {
        showStatus("Wait for every enabled clip effect to be ready; reload or remove failed effects.", true);
        return juce::Result::fail(status.getText());
    }
    if (!captureStates())
        return juce::Result::fail(status.getText());
    if (!onApply)
    {
        showStatus("The clip effects renderer is unavailable.", true);
        return juce::Result::fail("The clip effects renderer is unavailable.");
    }
    ClipEffectsSettings settings;
    settings.startSeconds = *first;
    settings.endSeconds = *last;
    settings.tailSeconds = *seconds;
    settings.effects = effectsProject.tracks.front();
    settings.requests = requests();
    const auto result = onApply(std::move(settings));
    if (result.failed())
        showStatus(result.getErrorMessage(), true);
    else if (onClose)
        onClose();
    return result;
}

void ClipEffectsComponent::timerCallback()
{
    const auto statuses = engine->pluginRuntimeStatuses();
    rack.setRuntimeStatuses(statuses, engine->pluginLateBlockCount());
    apply.setEnabled(effectsReady());
    for (const auto& state : statuses)
        if (state.state == StudioAudioEngine::PluginRuntimeStatus::State::failed
            || state.state == StudioAudioEngine::PluginRuntimeStatus::State::missing)
        {
            showStatus(state.name + ": " + state.message, true);
            break;
        }
}

bool ClipEffectsComponent::effectsReady() const
{
    if (engine->pluginRuntimeTransitionPending())
        return false;
    const auto statuses = engine->pluginRuntimeStatuses();
    const auto& inserts = effectsProject.tracks.front().inserts;
    auto active = false;
    for (const auto& insert : inserts)
    {
        if (insert.isContainer() || PluginRack::isBypassed(inserts, insert))
            continue;
        active = true;
        const auto found = std::find_if(statuses.begin(), statuses.end(),
            [&](const auto& state) { return state.insertId == insert.id; });
        if (found == statuses.end() || found->state != StudioAudioEngine::PluginRuntimeStatus::State::ready)
            return false;
    }
    return active;
}

void ClipEffectsComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(StudioColours::panel));
}

void ClipEffectsComponent::resized()
{
    auto bounds = getLocalBounds().reduced(16);
    title.setBounds(bounds.removeFromTop(30));
    rangeLabel.setBounds(bounds.removeFromTop(22));
    auto range = bounds.removeFromTop(28);
    start.setBounds(range.removeFromLeft(range.getWidth() / 2).reduced(2));
    end.setBounds(range.reduced(2));
    tailLabel.setBounds(bounds.removeFromTop(24));
    tail.setBounds(bounds.removeFromTop(28));
    bounds.removeFromTop(8);
    auto buttons = bounds.removeFromBottom(30);
    apply.setBounds(buttons.removeFromRight(190).reduced(2));
    cancel.setBounds(buttons.removeFromRight(100).reduced(2));
    status.setBounds(bounds.removeFromBottom(56));
    rack.setBounds(bounds);
}
}

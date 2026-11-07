#include "TestHarness.h"
#include "TestSuites.h"
#include "ReleaseFixtures.h"

#include "model/ProjectCommands.h"
#include "render/ArrangementRender.h"
#include "ui/SpectrumExportOptionsComponent.h"

#include <array>
#include <cmath>
#include <stdexcept>

namespace
{
juce::Component* findControl(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id)
        return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findControl(*child, id))
            return found;
    return nullptr;
}

template <typename Control>
Control& control(studio::SpectrumExportOptionsComponent& parent, const char* id)
{
    if (auto* result = dynamic_cast<Control*>(findControl(parent, id)))
        return *result;
    throw std::runtime_error(juce::String("Missing spectrum export control: " + juce::String(id)).toStdString());
}

void select(studio::SpectrumExportOptionsComponent& parent, const char* id, int value)
{
    control<juce::ComboBox>(parent, id).setSelectedId(value, juce::sendNotificationSync);
}

void optionsAndKeyboard()
{
    auto project = studio::Project::createDefault();
    studio::Track inactive;
    inactive.active = false;
    project.tracks.push_back(inactive);
    studio::Track take;
    take.parentTrackId = project.tracks.front().id;
    project.tracks.push_back(take);
    studio::SpectrumExportOptionsComponent component(project, {});
    juce::String error;
    auto selected = component.settings(error);
    expect(selected && selected->trackId.isEmpty() && selected->gif.width == 1280
               && selected->gif.height == 720 && selected->gif.framesPerSecond == 25
               && selected->gif.transparentBackground && selected->render.tailSeconds == 0.0,
           "The export dialog defaults to a full-mix transparent 720p/25 fps video overlay with no tail.");
    const auto sources = control<juce::ComboBox>(component, "spectrum.source").getNumItems();
    studio::SpectrumExportOptionsComponent roots(studio::Project::createDefault(), {});
    expect(sources == control<juce::ComboBox>(roots, "spectrum.source").getNumItems(),
           "Inactive tracks and take lanes are not offered as standalone spectrum sources.");
    bool exported = false;
    component.onExport = [&](auto) { exported = true; };
    select(component, "spectrum.source", 2);
    select(component, "spectrum.colour", 5);
    select(component, "spectrum.canvas", 3);
    select(component, "spectrum.frameRate", 30);
    select(component, "spectrum.background", 2);
    selected = component.settings(error);
    expect(selected && selected->trackId == project.tracks.front().id
               && selected->gif.barColour == project.tracks.front().colour
               && selected->gif.width == 1920 && selected->gif.height == 1080
               && selected->gif.framesPerSecond == 30 && !selected->gif.transparentBackground
               && !exported,
           "Track, palette, canvas and frame-rate changes preserve explicit settings without submitting.");
    select(component, "spectrum.source", 1);
    expect(!component.settings(error) && error.contains("track"),
           "Track color cannot silently become another color when switching to the whole mix.");
    select(component, "spectrum.colour", 2);
    select(component, "spectrum.range", 4);
    auto& start = control<juce::TextEditor>(component, "spectrum.start");
    auto& end = control<juce::TextEditor>(component, "spectrum.end");
    for (const auto* invalid : { "", "NaN", "Inf", "1junk", "1,2", "1e999" })
    {
        start.setText(invalid, false);
        expect(!component.settings(error) && error.isNotEmpty(),
               "Custom range positions reject partial, locale-dependent and nonfinite input.");
    }
    start.setText("2", false);
    end.setText("1", false);
    component.setVisible(true);
    component.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    expect(!exported && component.isVisible()
               && control<juce::Label>(component, "spectrum.status").getText().isNotEmpty(),
           "Return with an invalid range reports the error and keeps the dialog open.");
    start.setText("0.125", false);
    end.setText("2.5", false);
    selected = component.settings(error);
    expect(selected && selected->render.startSeconds == 0.125 && selected->render.endSeconds == 2.5,
           "Custom export ranges retain exact entered seconds.");
    component.setSize(360, 350);
    const auto& viewport = control<juce::Viewport>(component, "spectrum.viewport");
    expect(viewport.getViewedComponent()->getHeight() > viewport.getHeight(),
           "A compact spectrum dialog scrolls instead of dropping controls.");
    for (const auto* id : { "spectrum.source", "spectrum.range", "spectrum.start", "spectrum.end",
                            "spectrum.canvas", "spectrum.frameRate", "spectrum.colour", "spectrum.background" })
    {
        auto* field = findControl(component, id);
        expect(field && field->getWantsKeyboardFocus() && field->getTitle().isNotEmpty()
                   && !field->getBounds().isEmpty(),
               "Every visible export control has a keyboard tab stop, native title and usable bounds.");
    }
    component.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    expect(!component.isVisible() && !exported, "Escape cancels without creating an export.");

    studio::SpectrumExportOptionsComponent submit(studio::Project::createDefault(), {});
    int submissions = 0;
    submit.onExport = [&](auto) { ++submissions; };
    auto& button = control<juce::TextButton>(submit, "spectrum.export");
    button.onClick();
    button.onClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    expect(submissions == 1, "Submission dispatches once after dialog teardown, even after repeated activation.");
    studio::SpectrumExportSettings stale;
    stale.trackId = "removed-synthetic-track";
    studio::SpectrumExportOptionsComponent missing(project, stale);
    expect(!missing.settings(error) && error.isNotEmpty(), "A stale track ID cannot silently export the full song.");
}

void trackRoutingAndMixParity()
{
    release_fixture::Directory directory;
    const auto source = directory.file.getChildFile("synthetic-spectrum.wav");
    juce::AudioBuffer<float> tone(2, 48000);
    for (int sample = 0; sample < tone.getNumSamples(); ++sample)
    {
        const auto value = static_cast<float>(0.5 * std::sin(
            juce::MathConstants<double>::twoPi * 440.0 * sample / 48000.0));
        tone.setSample(0, sample, value);
        tone.setSample(1, sample, -value);
    }
    studio::AudioExportSettings outputSettings;
    outputSettings.bitDepth = 32;
    expect(studio::AudioExport::write(tone, source, outputSettings).wasOk(), "A synthetic spectral audio source can be written.");
    auto project = studio::Project::createDefault();
    project.metronomeEnabled = false;
    studio::AudioClip clip;
    clip.sourceFile = source;
    clip.durationSeconds = clip.sourceLengthSeconds = clip.sourceRangeEndSeconds = 1.0;
    project.tracks[0].clips = { clip };
    project.tracks[0].volumeDecibels = -6.0f;
    auto copy = clip;
    copy.id = juce::Uuid().toString();
    project.tracks[1].clips = { copy };
    project.findTrack(project.masterTrackId())->volumeDecibels = -12.0f;
    const auto id = project.tracks[0].id;
    juce::String error;
    const auto isolated = studio::ArrangementRender::prepareTrackExport(project, id, error);
    expect(isolated && project.tracks[0].outputTrackId == juce::String()
               && project.tracks[1].active
               && std::abs(project.findTrack(project.masterTrackId())->volumeDecibels + 12.0f) < 0.0001f,
           "Preparing a track spectrum leaves the live project and its mixer state unchanged.");
    if (!isolated)
        return;
    studio::MixExportSettings settings;
    settings.range = studio::MixExportRange::custom;
    settings.startSeconds = 0.25;
    settings.endSeconds = 0.75;
    settings.tailSeconds = 0.0;
    auto engine = std::make_unique<studio::StudioAudioEngine>();
    juce::AudioBuffer<float> stem, mix;
    expect(studio::RenderEngine::renderMix(*engine, *isolated, stem, settings).wasOk()
               && stem.getNumSamples() == 24000 && stem.getNumChannels() == 2,
           "A track spectrum uses the exact selected range and no additional tail.");
    expect(studio::RenderEngine::renderMix(*engine, project, mix, settings).wasOk(),
           "The whole-mix buffer uses the ordinary offline renderer.");
    const auto sourceRms = tone.getRMSLevel(0, 12000, 24000);
    expect(std::abs(stem.getRMSLevel(0, 0, stem.getNumSamples())
                        - sourceRms * juce::Decibels::decibelsToGain(-6.0f)) < 0.0001f,
           "A single-track spectrum includes its fader but excludes unrelated tracks and master gain.");
    const auto expectedMix = sourceRms * (1.0f + juce::Decibels::decibelsToGain(-6.0f))
        * juce::Decibels::decibelsToGain(-12.0f);
    expect(std::abs(mix.getRMSLevel(0, 0, mix.getNumSamples()) - expectedMix) < 0.0001f,
           "A full-song spectrum includes all audible tracks and master processing.");
    const auto mixFile = directory.file.getChildFile("ordinary-mix.wav");
    settings.audio.bitDepth = 32;
    expect(studio::RenderEngine::exportMix(*engine, project, mixFile, settings).wasOk(),
           "The shared render refactor preserves existing audio export.");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(mixFile));
    juce::AudioBuffer<float> decoded(2, 24000);
    expect(reader && reader->read(&decoded, 0, 24000, 0, true, true)
               && std::abs(decoded.getRMSLevel(0, 0, 24000) - mix.getRMSLevel(0, 0, 24000)) < 0.000001f,
           "Ordinary WAV exports and spectrum analysis share identical rendered PCM.");
    studio::Track bus;
    bus.type = studio::TrackType::bus;
    bus.volumeDecibels = -9.0f;
    const auto busId = bus.id;
    project.tracks.push_back(bus);
    expect(studio::SetTrackOutputCommand(id, busId).perform(project, error),
           "The synthetic bus input updates both explicit and implicit track routing.");
    const auto busProject = studio::ArrangementRender::prepareTrackExport(project, busId, error);
    juce::AudioBuffer<float> busAudio;
    const auto busResult = busProject
        ? studio::RenderEngine::renderMix(*engine, *busProject, busAudio, settings)
        : juce::Result::fail(error);
    expect(busResult.wasOk(), ("The bus spectrum can be prepared and rendered: " + busResult.getErrorMessage()).toRawUTF8());
    if (busResult.wasOk())
        expect(std::abs(busAudio.getRMSLevel(0, 0, busAudio.getNumSamples())
                       - sourceRms * juce::Decibels::decibelsToGain(-15.0f)) < 0.0001f,
               ("A bus spectrum retains upstream faders without unrelated audio (RMS "
                + juce::String(busAudio.getRMSLevel(0, 0, busAudio.getNumSamples()), 6)
                + ", expected " + juce::String(sourceRms * juce::Decibels::decibelsToGain(-15.0f), 6) + ").").toRawUTF8());
    project.findTrack(id)->inputMonitoring = true;
    expect(!studio::ArrangementRender::prepareTrackExport(project, id, error) && error.contains("monitoring"),
           "A live monitored track reports the need to record its input instead of exporting silence.");
    project.findTrack(id)->inputMonitoring = false;
    project.findTrack(id)->active = false;
    expect(!studio::ArrangementRender::prepareTrackExport(project, id, error) && error.isNotEmpty(),
           "An inactive source cannot be exported as an apparently successful track spectrum.");

    auto instrument = studio::Project::createDefault();
    const auto instrumentId = instrument.tracks.front().id;
    instrument.tracks.front().type = studio::TrackType::instrument;
    studio::PluginInsert generator;
    generator.pluginIdentifier = "studio.device.generator";
    generator.name = "Synthetic spectral generator";
    generator.bundledDevice = true;
    generator.bridgeMode = studio::PluginBridgeMode::trustedInProcess;
    instrument.tracks.front().inserts = { generator };
    const auto instrumentProject = studio::ArrangementRender::prepareTrackExport(instrument, instrumentId, error);
    studio::StudioAudioEngine::PluginRuntimeRequest request;
    request.trackId = instrumentId;
    request.insertId = generator.id;
    request.name = generator.name;
    request.deviceIdentifier = generator.pluginIdentifier;
    request.bridgeMode = generator.bridgeMode;
    juce::AudioBuffer<float> generated;
    expect(instrumentProject
               && studio::RenderEngine::renderMix(*engine, *instrumentProject, generated, settings, { request }).wasOk()
               && generated.getRMSLevel(0, 0, generated.getNumSamples()) > 0.001f,
           "Instrument spectra include the real bundled processor rather than a dry or silent substitute.");
    expect(instrumentProject
               && studio::RenderEngine::renderMix(*engine, *instrumentProject, generated, settings).failed(),
           "Missing active processor requests fail explicitly instead of exporting a processor-free spectrum.");
}

void optionalSyntheticPreview()
{
    const auto path = juce::SystemStats::getEnvironmentVariable("STUDIO_DUO_SPECTRUM_FIXTURE_DIRECTORY", {});
    if (path.isEmpty())
        return;
    const juce::File directory(path);
    expect(directory.createDirectory().wasOk(), "The requested synthetic spectrum fixture directory can be created.");
    juce::AudioBuffer<float> audio(2, 8 * 48000);
    constexpr std::array<double, 4> notes { 196.0, 261.63, 329.63, 440.0 };
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
    {
        const auto time = static_cast<double>(sample) / 48000.0;
        const auto frequency = notes[static_cast<std::size_t>(static_cast<int>(time) % 4)];
        const auto envelope = 0.2 + 0.8 * std::pow(std::sin(juce::MathConstants<double>::pi * time), 2.0);
        const auto value = static_cast<float>(envelope * (0.35 * std::sin(
            juce::MathConstants<double>::twoPi * frequency * time) + 0.15 * std::sin(
            juce::MathConstants<double>::twoPi * frequency * 3.0 * time)));
        audio.setSample(0, sample, value);
        audio.setSample(1, sample, -value);
    }
    studio::SpectrumExportSettings settings;
    settings.gif.width = 640;
    settings.gif.height = 360;
    expect(studio::SpectrumGif::write(audio, 48000.0, directory.getChildFile("spectrum-overlay.gif"), settings.gif).wasOk(),
           "The real native encoder writes the requested synthetic video overlay.");
    expect(studio::AudioExport::write(audio, directory.getChildFile("spectrum-audio.wav"), settings.render.audio).wasOk(),
           "The optional overlay fixture has matching synthetic audio.");
    studio::SpectrumExportOptionsComponent component(studio::Project::createDefault(), settings);
    juce::MemoryOutputStream png;
    expect(juce::PNGImageFormat().writeImageToStream(
               component.createComponentSnapshot(component.getLocalBounds()), png)
               && directory.getChildFile("spectrum-export-dialog.png").replaceWithData(png.getData(), png.getDataSize()),
           "The native options component can be inspected without controlling the desktop.");
}
}

void spectrumExportTests()
{
    optionsAndKeyboard();
    trackRoutingAndMixParity();
    optionalSyntheticPreview();
}

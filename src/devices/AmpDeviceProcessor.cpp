#include "AmpDeviceProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#if STUDIO_DUO_TESTING
#include <thread>
#endif

namespace studio
{
namespace
{
constexpr auto stateMagic = static_cast<juce::int64>(
    0x53544455414d5031ULL);
constexpr auto stateVersion = 1;
constexpr auto maximumStateStringBytes = 1024;

struct CabinetAsset
{
    bool embedded = true;
    juce::String name;
    double sampleRate = 48000.0;
    int channels = 1;
    std::vector<float> left;
    std::vector<float> right;
};

bool writeBoundedString(juce::MemoryOutputStream& output,
                        const juce::String& value)
{
    const auto bytes = value.toUTF8();
    const auto size = static_cast<int>(value.getNumBytesAsUTF8());
    if (size < 0 || size > maximumStateStringBytes)
        return false;
    output.writeInt(size);
    return size == 0
        || output.write(bytes.getAddress(),
                        static_cast<std::size_t>(size));
}

std::optional<juce::String> readBoundedString(
    juce::MemoryInputStream& input)
{
    if (input.getNumBytesRemaining()
        < static_cast<juce::int64>(sizeof(int)))
        return std::nullopt;
    const auto size = input.readInt();
    if (size < 0
        || size > maximumStateStringBytes
        || input.getNumBytesRemaining()
            < static_cast<juce::int64>(size))
        return std::nullopt;
    std::array<char, maximumStateStringBytes + 1> bytes {};
    if (size > 0
        && input.read(bytes.data(), size) != size)
        return std::nullopt;
    return juce::String::fromUTF8(bytes.data(), size);
}

CabinetAsset makeEmbeddedCabinet(AmpDeviceType type)
{
    CabinetAsset asset;
    asset.embedded = true;
    asset.sampleRate = 48000.0;
    asset.channels = 1;
    asset.name = type == AmpDeviceType::guitar
        ? "Embedded Modern 4x12"
        : "Embedded Tight 8x10";
    const auto length = type == AmpDeviceType::guitar ? 640 : 768;
    asset.left.resize(static_cast<std::size_t>(length));
    auto random = type == AmpDeviceType::guitar
        ? std::uint32_t { 0x714ac23dU }
        : std::uint32_t { 0x4f91db67U };
    auto peak = 0.0f;
    for (int sample = 0; sample < length; ++sample)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const auto noise =
            static_cast<float>(random)
                / static_cast<float>(
                    std::numeric_limits<std::uint32_t>::max())
                * 2.0f
            - 1.0f;
        const auto time = static_cast<double>(sample) / 48000.0;
        const auto envelope = std::exp(
            -time
            * (type == AmpDeviceType::guitar ? 245.0 : 185.0));
        const auto resonances = type == AmpDeviceType::guitar
            ? 0.52 * std::sin(
                  juce::MathConstants<double>::twoPi * 1080.0 * time)
                + 0.31 * std::sin(
                    juce::MathConstants<double>::twoPi * 2780.0 * time)
                + 0.15 * std::sin(
                    juce::MathConstants<double>::twoPi * 4650.0 * time)
            : 0.58 * std::sin(
                  juce::MathConstants<double>::twoPi * 135.0 * time)
                + 0.28 * std::sin(
                    juce::MathConstants<double>::twoPi * 720.0 * time)
                + 0.12 * std::sin(
                    juce::MathConstants<double>::twoPi * 1850.0 * time);
        auto value = static_cast<float>(
            envelope
            * (resonances + noise * 0.08));
        if (sample == 0)
            value += type == AmpDeviceType::guitar ? 0.82f : 0.94f;
        asset.left[static_cast<std::size_t>(sample)] = value;
        peak = std::max(peak, std::abs(value));
    }
    if (peak > 0.0f)
        for (auto& sample : asset.left)
            sample *= 0.92f / peak;
    return asset;
}

float saturate(float input, AmpDeviceType type) noexcept
{
    if (type == AmpDeviceType::guitar)
    {
        const auto biased = input + 0.055f;
        return std::tanh(biased)
            - std::tanh(0.055f);
    }
    const auto limited = juce::jlimit(-3.0f, 3.0f, input);
    return limited
        - 0.12f * limited * limited * limited;
}
}

class AmpDeviceProcessor::CabinetConvolver
{
public:
    static constexpr int partitionSize = 128;
    static constexpr int fftOrder = 8;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int maximumPartitions = 64;
    static constexpr int maximumImpulseSamples =
        partitionSize * maximumPartitions;
    using Complex = juce::dsp::Complex<float>;
    using Spectrum = std::array<Complex, fftSize>;

    explicit CabinetConvolver(AmpDeviceType ampType)
        : type(ampType),
          fft(fftOrder),
          asset(makeEmbeddedCabinet(type))
    {
        juce::String ignored;
        publish(asset, 48000.0, ignored);
    }

    juce::Result prepare(double sampleRate)
    {
        const auto snapshot = assetSnapshot();
        juce::String error;
        if (!publish(snapshot, sampleRate, error))
            return juce::Result::fail(error);
        reset();
        return juce::Result::ok();
    }

    void reset() noexcept
    {
        for (auto& state : states)
            state.reset();
        appliedGeneration = 0;
    }

    void process(juce::AudioBuffer<float>& audio) noexcept
    {
        auto slotIndex = 0;
        for (;;)
        {
            slotIndex = activeSlot.load(std::memory_order_acquire);
#if STUDIO_DUO_TESTING
            if (auto* barrier = readerBarrier.exchange(
                    nullptr,
                    std::memory_order_acq_rel))
            {
                barrier->slotLoaded.store(
                    true,
                    std::memory_order_release);
                while (!barrier->resume.load(
                    std::memory_order_acquire))
                    std::this_thread::yield();
            }
#endif
            readers[static_cast<std::size_t>(slotIndex)].fetch_add(
                1,
                std::memory_order_acq_rel);
            if (activeSlot.load(std::memory_order_acquire)
                == slotIndex)
                break;
            readers[static_cast<std::size_t>(slotIndex)].fetch_sub(
                1,
                std::memory_order_release);
        }
        const auto& kernel = slots[static_cast<std::size_t>(slotIndex)];
        if (appliedGeneration != kernel.generation)
        {
            for (auto& state : states)
                state.reset();
            appliedGeneration = kernel.generation;
        }
        for (int channel = 0;
             channel < std::min(2, audio.getNumChannels());
             ++channel)
        {
            auto* samples = audio.getWritePointer(channel);
            auto& state = states[static_cast<std::size_t>(channel)];
            const auto kernelChannel = std::min(
                channel,
                kernel.channels - 1);
            for (int sample = 0;
                 sample < audio.getNumSamples();
                 ++sample)
            {
                const auto output = state.popOutput();
                state.pushInput(
                    samples[sample],
                    kernel.spectra[static_cast<std::size_t>(
                        kernelChannel)],
                    kernel.partitionCount,
                    fft);
                samples[sample] = output;
            }
        }
        readers[static_cast<std::size_t>(slotIndex)].fetch_sub(
            1,
            std::memory_order_release);
    }

    juce::Result loadFile(const juce::File& file,
                          double targetSampleRate)
    {
        if (!file.existsAsFile())
            return fail("Cabinet IR file does not exist.");

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            formats.createReaderFor(file));
        if (reader == nullptr)
        {
            return fail(
                "Cabinet IR is not a supported or valid audio file.");
        }
        if (reader->sampleRate < 8000.0
            || reader->sampleRate > 384000.0)
            return fail("Cabinet IR sample rate is invalid.");
        if (reader->numChannels < 1 || reader->numChannels > 2)
            return fail("Cabinet IR must contain one or two channels.");
        if (reader->lengthInSamples <= 0
            || reader->lengthInSamples
                > std::numeric_limits<int>::max())
            return fail("Cabinet IR has an invalid length.");
        const auto validationSampleRate = std::max(
            targetSampleRate,
            384000.0);
        const auto targetLength = static_cast<std::int64_t>(
            std::llround(
                static_cast<double>(reader->lengthInSamples)
                * validationSampleRate
                / reader->sampleRate));
        if (targetLength <= 0
            || targetLength > maximumImpulseSamples)
        {
            return fail(
                "Cabinet IR is too long after resampling; the maximum is "
                + juce::String(maximumImpulseSamples)
                + " samples.");
        }

        const auto sourceSamples =
            static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> decoded(
            static_cast<int>(reader->numChannels),
            sourceSamples);
        if (!reader->read(
                &decoded,
                0,
                sourceSamples,
                0,
                true,
                reader->numChannels > 1))
            return fail("Cabinet IR could not be decoded.");

        CabinetAsset candidate;
        candidate.embedded = false;
        candidate.name = file.getFileName();
        candidate.sampleRate = reader->sampleRate;
        candidate.channels = static_cast<int>(reader->numChannels);
        candidate.left.resize(
            static_cast<std::size_t>(sourceSamples));
        if (candidate.channels > 1)
            candidate.right.resize(
                static_cast<std::size_t>(sourceSamples));
        auto peak = 0.0f;
        for (int sample = 0; sample < sourceSamples; ++sample)
        {
            const auto left = decoded.getSample(0, sample);
            const auto right = candidate.channels > 1
                ? decoded.getSample(1, sample)
                : left;
            if (!std::isfinite(left) || !std::isfinite(right))
                return fail("Cabinet IR contains non-finite samples.");
            candidate.left[static_cast<std::size_t>(sample)] = left;
            if (candidate.channels > 1)
                candidate.right[static_cast<std::size_t>(sample)] = right;
            peak = std::max({ peak, std::abs(left), std::abs(right) });
        }
        if (peak < 0.000001f)
            return fail("Cabinet IR is silent.");
        const auto normalization = 0.95f / peak;
        for (auto& sample : candidate.left)
            sample *= normalization;
        for (auto& sample : candidate.right)
            sample *= normalization;

        juce::String error;
        if (!publish(candidate, targetSampleRate, error))
            return fail(error);
        {
            const juce::ScopedLock lock(assetLock);
            asset = std::move(candidate);
            lastError.clear();
        }
        return juce::Result::ok();
    }

    juce::Result useEmbedded(double targetSampleRate)
    {
        auto candidate = makeEmbeddedCabinet(type);
        juce::String error;
        if (!publish(candidate, targetSampleRate, error))
            return fail(error);
        {
            const juce::ScopedLock lock(assetLock);
            asset = std::move(candidate);
            lastError.clear();
        }
        return juce::Result::ok();
    }

    juce::Result restoreAsset(CabinetAsset candidate,
                              double targetSampleRate)
    {
        juce::String error;
        if (!validateAsset(candidate, targetSampleRate, error)
            || !publish(candidate, targetSampleRate, error))
            return fail(error);
        {
            const juce::ScopedLock lock(assetLock);
            asset = std::move(candidate);
            lastError.clear();
        }
        return juce::Result::ok();
    }

    [[nodiscard]] CabinetAsset assetSnapshot() const
    {
        const juce::ScopedLock lock(assetLock);
        return asset;
    }

    [[nodiscard]] juce::String description() const
    {
        const juce::ScopedLock lock(assetLock);
        auto value = asset.name;
        value << " | "
              << juce::String(asset.channels)
              << " ch | "
              << juce::String(asset.sampleRate, 0)
              << " Hz";
        if (lastError.isNotEmpty())
            value << " | ERROR: " << lastError;
        return value;
    }

    [[nodiscard]] double tailSeconds() const noexcept
    {
        return currentTailSeconds.load(std::memory_order_relaxed);
    }

    void reportError(const juce::String& error)
    {
        const juce::ScopedLock lock(assetLock);
        lastError = error;
    }

#if STUDIO_DUO_TESTING
    void setReaderBarrierForTesting(
        AmpDeviceProcessor::CabinetReaderBarrierForTesting* barrier) noexcept
    {
        readerBarrier.store(barrier, std::memory_order_release);
    }
#endif

private:
    struct KernelSlot
    {
        std::array<std::array<Spectrum, maximumPartitions>, 2>
            spectra {};
        int partitionCount = 1;
        int impulseSamples = 1;
        int channels = 1;
        std::uint64_t generation = 0;
    };

    struct ChannelState
    {
        std::array<Spectrum, maximumPartitions> history {};
        Spectrum inputSpectrum {};
        Spectrum transformed {};
        Spectrum accumulated {};
        std::array<float, partitionSize> input {};
        std::array<float, partitionSize> overlap {};
        std::array<float, partitionSize> output {};
        int inputCount = 0;
        int outputRead = 0;
        int outputAvailable = 0;
        int historyPosition = 0;

        void reset() noexcept
        {
            for (auto& spectrum : history)
                spectrum.fill({});
            inputSpectrum.fill({});
            transformed.fill({});
            accumulated.fill({});
            input.fill(0.0f);
            overlap.fill(0.0f);
            output.fill(0.0f);
            inputCount = 0;
            outputRead = 0;
            outputAvailable = 0;
            historyPosition = 0;
        }

        float popOutput() noexcept
        {
            if (outputAvailable <= 0)
                return 0.0f;
            const auto value = output[static_cast<std::size_t>(
                outputRead++)];
            --outputAvailable;
            return value;
        }

        void pushInput(
            float value,
            const std::array<Spectrum, maximumPartitions>& kernel,
            int partitions,
            juce::dsp::FFT& transform) noexcept
        {
            input[static_cast<std::size_t>(inputCount++)] = value;
            if (inputCount < partitionSize)
                return;

            inputSpectrum.fill({});
            for (int sample = 0; sample < partitionSize; ++sample)
            {
                inputSpectrum[static_cast<std::size_t>(sample)] =
                    Complex(input[static_cast<std::size_t>(sample)], 0.0f);
            }
            transform.perform(
                inputSpectrum.data(),
                transformed.data(),
                false);
            history[static_cast<std::size_t>(historyPosition)] =
                transformed;
            accumulated.fill({});
            for (int partition = 0; partition < partitions; ++partition)
            {
                auto historyIndex = historyPosition - partition;
                if (historyIndex < 0)
                    historyIndex += maximumPartitions;
                const auto& prior = history[static_cast<std::size_t>(
                    historyIndex)];
                const auto& response = kernel[static_cast<std::size_t>(
                    partition)];
                for (int bin = 0; bin < fftSize; ++bin)
                {
                    accumulated[static_cast<std::size_t>(bin)]
                        += prior[static_cast<std::size_t>(bin)]
                            * response[static_cast<std::size_t>(bin)];
                }
            }
            transform.perform(
                accumulated.data(),
                inputSpectrum.data(),
                true);
            for (int sample = 0; sample < partitionSize; ++sample)
            {
                output[static_cast<std::size_t>(sample)] =
                    inputSpectrum[static_cast<std::size_t>(sample)].real()
                    + overlap[static_cast<std::size_t>(sample)];
                overlap[static_cast<std::size_t>(sample)] =
                    inputSpectrum[static_cast<std::size_t>(
                        sample + partitionSize)].real();
            }
            inputCount = 0;
            outputRead = 0;
            outputAvailable = partitionSize;
            historyPosition =
                (historyPosition + 1) % maximumPartitions;
        }
    };

    bool validateAsset(const CabinetAsset& candidate,
                       double targetSampleRate,
                       juce::String& error) const
    {
        if (candidate.sampleRate < 8000.0
            || candidate.sampleRate > 384000.0
            || candidate.channels < 1
            || candidate.channels > 2
            || candidate.left.empty()
            || (candidate.channels > 1
                && candidate.right.size() != candidate.left.size()))
        {
            error = "Cabinet state contains invalid audio metadata.";
            return false;
        }
        const auto validationSampleRate = std::max(
            targetSampleRate,
            384000.0);
        const auto targetLength = static_cast<std::int64_t>(
            std::llround(
                static_cast<double>(candidate.left.size())
                * validationSampleRate
                / candidate.sampleRate));
        if (targetLength <= 0
            || targetLength > maximumImpulseSamples)
        {
            error =
                "Cabinet state is too long for the current sample rate.";
            return false;
        }
        auto peak = 0.0f;
        for (std::size_t index = 0; index < candidate.left.size(); ++index)
        {
            const auto left = candidate.left[index];
            const auto right = candidate.channels > 1
                ? candidate.right[index]
                : left;
            if (!std::isfinite(left) || !std::isfinite(right))
            {
                error = "Cabinet state contains non-finite samples.";
                return false;
            }
            peak = std::max({ peak, std::abs(left), std::abs(right) });
        }
        if (peak < 0.000001f)
        {
            error = "Cabinet state is silent.";
            return false;
        }
        return true;
    }

    bool publish(const CabinetAsset& candidate,
                 double targetSampleRate,
                 juce::String& error)
    {
        if (!validateAsset(candidate, targetSampleRate, error))
            return false;
        const auto targetLength = static_cast<int>(
            std::llround(
                static_cast<double>(candidate.left.size())
                * targetSampleRate
                / candidate.sampleRate));
        auto prepared = std::make_unique<KernelSlot>();
        juce::dsp::FFT preparationTransform(fftOrder);
        prepared->partitionCount = std::max(
            1,
            (targetLength + partitionSize - 1) / partitionSize);
        prepared->impulseSamples = targetLength;
        prepared->channels = candidate.channels;
        std::vector<float> resampled(
            static_cast<std::size_t>(targetLength));
        for (int channel = 0; channel < candidate.channels; ++channel)
        {
            const auto& source = channel == 0
                ? candidate.left
                : candidate.right;
            for (int sample = 0; sample < targetLength; ++sample)
            {
                const auto sourcePosition =
                    static_cast<double>(sample)
                    * candidate.sampleRate
                    / targetSampleRate;
                const auto before = std::min(
                    static_cast<int>(source.size()) - 1,
                    static_cast<int>(sourcePosition));
                const auto after = std::min(
                    static_cast<int>(source.size()) - 1,
                    before + 1);
                const auto fraction = static_cast<float>(
                    sourcePosition - static_cast<double>(before));
                resampled[static_cast<std::size_t>(sample)] =
                    source[static_cast<std::size_t>(before)]
                    + (source[static_cast<std::size_t>(after)]
                       - source[static_cast<std::size_t>(before)])
                        * fraction;
            }
            for (int partition = 0;
                 partition < prepared->partitionCount;
                 ++partition)
            {
                Spectrum time {};
                Spectrum transformed {};
                const auto sourceOffset = partition * partitionSize;
                const auto count = std::min(
                    partitionSize,
                    targetLength - sourceOffset);
                for (int sample = 0; sample < count; ++sample)
                {
                    time[static_cast<std::size_t>(sample)] = Complex(
                        resampled[static_cast<std::size_t>(
                            sourceOffset + sample)],
                        0.0f);
                }
                preparationTransform.perform(
                    time.data(),
                    transformed.data(),
                    false);
                prepared->spectra[static_cast<std::size_t>(channel)]
                                 [static_cast<std::size_t>(partition)] =
                    transformed;
            }
        }
        if (candidate.channels == 1)
            prepared->spectra[1] = prepared->spectra[0];

        const auto current =
            activeSlot.load(std::memory_order_acquire);
        auto destination = -1;
        for (int index = 0;
             index < static_cast<int>(slots.size());
             ++index)
        {
            if (index != current
                && readers[static_cast<std::size_t>(index)].load(
                       std::memory_order_acquire)
                    == 0)
            {
                destination = index;
                break;
            }
        }
        if (destination < 0)
        {
            error =
                "Cabinet kernel is busy; try loading the IR again.";
            return false;
        }
        prepared->generation =
            generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        slots[static_cast<std::size_t>(destination)] =
            std::move(*prepared);
        currentTailSeconds.store(
            static_cast<double>(targetLength - 1)
                / targetSampleRate,
            std::memory_order_release);
        activeSlot.store(destination, std::memory_order_release);
        return true;
    }

    juce::Result fail(const juce::String& error)
    {
        const juce::ScopedLock lock(assetLock);
        lastError = error;
        return juce::Result::fail(error);
    }

    AmpDeviceType type;
    juce::dsp::FFT fft;
    std::array<KernelSlot, 3> slots;
    std::array<std::atomic<int>, 3> readers {};
    std::atomic<int> activeSlot { 0 };
    std::atomic<std::uint64_t> generation { 0 };
    std::atomic<double> currentTailSeconds { 0.0 };
#if STUDIO_DUO_TESTING
    std::atomic<AmpDeviceProcessor::CabinetReaderBarrierForTesting*>
        readerBarrier { nullptr };
#endif
    std::array<ChannelState, 2> states;
    std::uint64_t appliedGeneration = 0;
    mutable juce::CriticalSection assetLock;
    CabinetAsset asset;
    juce::String lastError;
};

namespace
{
class AmpDeviceEditor final : public juce::AudioProcessorEditor
{
public:
    explicit AmpDeviceEditor(AmpDeviceProcessor& owner)
        : juce::AudioProcessorEditor(&owner),
          amp(owner),
          parameters(owner)
    {
        addAndMakeVisible(parameters);
        addAndMakeVisible(loadButton);
        addAndMakeVisible(defaultButton);
        addAndMakeVisible(status);
        loadButton.setButtonText("Load cabinet IR...");
        defaultButton.setButtonText("Use embedded cabinet");
        status.setText(
            amp.cabinetDescription(),
            juce::dontSendNotification);
        status.setJustificationType(juce::Justification::centredLeft);
        status.setColour(
            juce::Label::textColourId,
            juce::Colours::lightgrey);
        loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser>(
                "Load cabinet impulse response",
                juce::File(),
                "*.wav;*.aif;*.aiff;*.flac");
            const juce::Component::SafePointer<AmpDeviceEditor> safe(this);
            chooser->launchAsync(
                juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles,
                [safe](const juce::FileChooser& completed)
                {
                    if (safe == nullptr)
                        return;
                    const auto file = completed.getResult();
                    if (file == juce::File())
                        return;
                    const auto result =
                        safe->amp.loadCabinetFile(file);
                    safe->status.setText(
                        result.wasOk()
                            ? safe->amp.cabinetDescription()
                            : result.getErrorMessage(),
                        juce::dontSendNotification);
                    safe->status.setColour(
                        juce::Label::textColourId,
                        result.wasOk()
                            ? juce::Colours::lightgrey
                            : juce::Colours::orange);
                });
        };
        defaultButton.onClick = [this]
        {
            const auto result =
                amp.useEmbeddedDefaultCabinet();
            status.setText(
                result.wasOk()
                    ? amp.cabinetDescription()
                    : result.getErrorMessage(),
                juce::dontSendNotification);
            status.setColour(
                juce::Label::textColourId,
                result.wasOk()
                    ? juce::Colours::lightgrey
                    : juce::Colours::orange);
        };
        setSize(
            std::max(460, parameters.getWidth()),
            parameters.getHeight() + 82);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto controls = bounds.removeFromBottom(76).reduced(8, 5);
        auto buttons = controls.removeFromTop(28);
        loadButton.setBounds(buttons.removeFromLeft(170));
        buttons.removeFromLeft(8);
        defaultButton.setBounds(buttons.removeFromLeft(190));
        controls.removeFromTop(4);
        status.setBounds(controls.removeFromTop(28));
        parameters.setBounds(bounds);
    }

private:
    AmpDeviceProcessor& amp;
    juce::GenericAudioProcessorEditor parameters;
    juce::TextButton loadButton;
    juce::TextButton defaultButton;
    juce::Label status;
    std::unique_ptr<juce::FileChooser> chooser;
};
}

juce::AudioProcessor::BusesProperties AmpDeviceProcessor::buses()
{
    return BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true);
}

AmpDeviceProcessor::AmpDeviceProcessor(AmpDeviceType ampType)
    : juce::AudioProcessor(buses()),
      type(ampType),
      cabinet(std::make_unique<CabinetConvolver>(type))
{
    addFloat(ParameterSlot::gain,
             "gain",
             "Preamp gain",
             { 0.0f, 36.0f },
             type == AmpDeviceType::guitar ? 18.0f : 11.0f);
    addFloat(ParameterSlot::bass,
             "bass",
             "Bass",
             { -12.0f, 12.0f },
             type == AmpDeviceType::bass ? 2.0f : 0.0f);
    addFloat(ParameterSlot::mid,
             "mid",
             "Mid",
             { -12.0f, 12.0f },
             type == AmpDeviceType::guitar ? 1.5f : 0.0f);
    addFloat(ParameterSlot::treble,
             "treble",
             "Treble",
             { -12.0f, 12.0f },
             type == AmpDeviceType::guitar ? 1.0f : -0.5f);
    addFloat(ParameterSlot::presence,
             "presence",
             "Presence",
             { -12.0f, 12.0f },
             type == AmpDeviceType::guitar ? 1.0f : 0.0f);
    addFloat(ParameterSlot::blend,
             "blend",
             type == AmpDeviceType::guitar
                 ? "Saturation"
                 : "Drive blend",
             { 0.0f, 1.0f },
             type == AmpDeviceType::guitar ? 0.72f : 0.48f);
    addFloat(ParameterSlot::cabinetMix,
             "cabinetMix",
             "Cabinet mix",
             { 0.0f, 1.0f },
             1.0f);
    addFloat(ParameterSlot::output,
             "output",
             "Output",
             { -30.0f, 12.0f },
             type == AmpDeviceType::guitar ? -8.0f : -5.0f);
}

AmpDeviceProcessor::~AmpDeviceProcessor() = default;

juce::AudioParameterFloat* AmpDeviceProcessor::addFloat(
    ParameterSlot slot,
    const juce::String& id,
    const juce::String& name,
    juce::NormalisableRange<float> range,
    float defaultValue)
{
    auto value = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID(id, 1),
        name,
        range,
        defaultValue);
    auto* pointer = value.get();
    addParameter(pointer);
    value.release();
    parameterLookup.emplace_back(id, pointer);
    realtimeParameters[static_cast<std::size_t>(slot)] = pointer;
    return pointer;
}

const juce::String AmpDeviceProcessor::getName() const
{
    return type == AmpDeviceType::guitar
        ? "Guitar Amp"
        : "Bass Amp";
}

void AmpDeviceProcessor::prepareToPlay(double sampleRate,
                                       int maximumBlockSize)
{
    currentSampleRate.store(
        std::max(1.0, sampleRate),
        std::memory_order_release);
    maximumBlock = std::max(1, maximumBlockSize);
    dryBuffer.setSize(2, maximumBlock, false, true, false);
    dryDelay.setSize(
        2,
        CabinetConvolver::partitionSize + 1,
        false,
        true,
        false);
    cabinet->prepare(currentSampleRate.load(std::memory_order_acquire));
    reset();
    setLatencySamples(CabinetConvolver::partitionSize);
}

void AmpDeviceProcessor::releaseResources()
{
}

void AmpDeviceProcessor::reset()
{
    for (auto& channel : toneFilters)
        for (auto& filter : channel)
            filter.reset();
    inputHighPassState.fill(0.0f);
    bassLowState.fill(0.0f);
    dryBuffer.clear();
    dryDelay.clear();
    dryDelayWritePosition = 0;
    cabinet->reset();
}

void AmpDeviceProcessor::processBlock(
    juce::AudioBuffer<float>& audio,
    juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto input = getBusBuffer(audio, true, 0);
    auto output = getBusBuffer(audio, false, 0);
    const auto samples = std::min(
        output.getNumSamples(),
        maximumBlock);
    updateToneFilters();
    const auto sampleRate =
        currentSampleRate.load(std::memory_order_relaxed);
    const auto inputGain = juce::Decibels::decibelsToGain(
        parameter(ParameterSlot::gain));
    const auto drive = parameter(ParameterSlot::blend);
    const auto highPassCoefficient = static_cast<float>(
        std::exp(
            -juce::MathConstants<double>::twoPi
            * (type == AmpDeviceType::guitar ? 72.0 : 38.0)
            / sampleRate));
    const auto bassSplitCoefficient = static_cast<float>(
        std::exp(
            -juce::MathConstants<double>::twoPi
            * 180.0
            / sampleRate));

    for (int channel = 0;
         channel < std::min(2, output.getNumChannels());
         ++channel)
    {
        auto* destination = output.getWritePointer(channel);
        const auto* source = input.getNumChannels() > 0
            ? input.getReadPointer(
                  std::min(channel, input.getNumChannels() - 1))
            : nullptr;
        auto& highPass = inputHighPassState[
            static_cast<std::size_t>(channel)];
        auto& bassLow = bassLowState[
            static_cast<std::size_t>(channel)];
        auto& filters = toneFilters[static_cast<std::size_t>(
            channel)];
        for (int sample = 0; sample < samples; ++sample)
        {
            const auto raw = source != nullptr ? source[sample] : 0.0f;
            highPass = raw
                + highPassCoefficient
                    * (highPass - raw);
            const auto highPassed = raw - highPass;
            auto driven = highPassed
                * inputGain
                * (type == AmpDeviceType::guitar
                       ? 0.22f + drive * 0.18f
                       : 0.18f);
            if (type == AmpDeviceType::bass)
            {
                bassLow = driven
                    + bassSplitCoefficient
                        * (bassLow - driven);
                const auto distorted = saturate(
                    (driven - bassLow) * (1.2f + drive * 4.0f),
                    type);
                driven = bassLow + distorted * drive;
            }
            else
            {
                driven = saturate(
                    driven * (1.0f + drive * 5.5f),
                    type);
            }
            for (auto& filter : filters)
                driven = filter.process(driven);
            dryBuffer.setSample(channel, sample, driven);
            destination[sample] = driven;
        }
    }
    for (int channel = output.getNumChannels();
         channel < 2;
         ++channel)
        dryBuffer.clear(channel, 0, samples);

    cabinet->process(output);
    const auto cabinetMix =
        parameter(ParameterSlot::cabinetMix);
    const auto outputGain = juce::Decibels::decibelsToGain(
        parameter(ParameterSlot::output));
    const auto delayCapacity = dryDelay.getNumSamples();
    for (int sample = 0; sample < samples; ++sample)
    {
        auto read = dryDelayWritePosition
            - CabinetConvolver::partitionSize;
        if (read < 0)
            read += delayCapacity;
        for (int channel = 0;
             channel < std::min(2, output.getNumChannels());
             ++channel)
        {
            const auto dry = dryBuffer.getSample(channel, sample);
            const auto delayedDry = dryDelay.getSample(channel, read);
            dryDelay.setSample(
                channel,
                dryDelayWritePosition,
                dry);
            const auto wet =
                output.getSample(channel, sample) * 0.22f;
            output.setSample(
                channel,
                sample,
                (delayedDry + (wet - delayedDry) * cabinetMix)
                    * outputGain);
        }
        dryDelayWritePosition =
            (dryDelayWritePosition + 1) % delayCapacity;
    }
    for (int channel = 0; channel < output.getNumChannels(); ++channel)
        if (samples < output.getNumSamples())
            output.clear(channel,
                         samples,
                         output.getNumSamples() - samples);
}

float AmpDeviceProcessor::Biquad::process(float input) noexcept
{
    const auto output = input * b0 + z1;
    z1 = input * b1 - output * a1 + z2;
    z2 = input * b2 - output * a2;
    return output;
}

void AmpDeviceProcessor::Biquad::reset() noexcept
{
    z1 = 0.0f;
    z2 = 0.0f;
}

void AmpDeviceProcessor::updateToneFilters() noexcept
{
    const auto sampleRate =
        currentSampleRate.load(std::memory_order_relaxed);
    for (auto& filters : toneFilters)
    {
        setLowShelf(filters[0],
                    sampleRate,
                    type == AmpDeviceType::guitar ? 120.0 : 90.0,
                    parameter(ParameterSlot::bass));
        setPeaking(filters[1],
                   sampleRate,
                   type == AmpDeviceType::guitar ? 720.0 : 460.0,
                   type == AmpDeviceType::guitar ? 0.72 : 0.8,
                   parameter(ParameterSlot::mid));
        setHighShelf(
            filters[2],
            sampleRate,
            type == AmpDeviceType::guitar ? 3200.0 : 2400.0,
            parameter(ParameterSlot::treble)
                + parameter(ParameterSlot::presence) * 0.55f);
    }
}

void AmpDeviceProcessor::setPeaking(Biquad& filter,
                                    double sampleRate,
                                    double frequency,
                                    double q,
                                    double gainDecibels) noexcept
{
    const auto a = std::pow(10.0, gainDecibels / 40.0);
    const auto omega = juce::MathConstants<double>::twoPi
        * juce::jlimit(20.0, sampleRate * 0.45, frequency)
        / sampleRate;
    const auto alpha = std::sin(omega) / (2.0 * q);
    const auto cosine = std::cos(omega);
    const auto a0 = 1.0 + alpha / a;
    filter.b0 = static_cast<float>((1.0 + alpha * a) / a0);
    filter.b1 = static_cast<float>((-2.0 * cosine) / a0);
    filter.b2 = static_cast<float>((1.0 - alpha * a) / a0);
    filter.a1 = static_cast<float>((-2.0 * cosine) / a0);
    filter.a2 = static_cast<float>((1.0 - alpha / a) / a0);
}

void AmpDeviceProcessor::setLowShelf(Biquad& filter,
                                     double sampleRate,
                                     double frequency,
                                     double gainDecibels) noexcept
{
    const auto a = std::pow(10.0, gainDecibels / 40.0);
    const auto omega = juce::MathConstants<double>::twoPi
        * juce::jlimit(20.0, sampleRate * 0.45, frequency)
        / sampleRate;
    const auto cosine = std::cos(omega);
    const auto sine = std::sin(omega);
    const auto alpha = sine / std::sqrt(2.0);
    const auto beta = 2.0 * std::sqrt(a) * alpha;
    const auto a0 = (a + 1.0)
        + (a - 1.0) * cosine
        + beta;
    filter.b0 = static_cast<float>(
        a * ((a + 1.0) - (a - 1.0) * cosine + beta) / a0);
    filter.b1 = static_cast<float>(
        2.0 * a * ((a - 1.0) - (a + 1.0) * cosine) / a0);
    filter.b2 = static_cast<float>(
        a * ((a + 1.0) - (a - 1.0) * cosine - beta) / a0);
    filter.a1 = static_cast<float>(
        -2.0 * ((a - 1.0) + (a + 1.0) * cosine) / a0);
    filter.a2 = static_cast<float>(
        ((a + 1.0) + (a - 1.0) * cosine - beta) / a0);
}

void AmpDeviceProcessor::setHighShelf(Biquad& filter,
                                      double sampleRate,
                                      double frequency,
                                      double gainDecibels) noexcept
{
    const auto a = std::pow(10.0, gainDecibels / 40.0);
    const auto omega = juce::MathConstants<double>::twoPi
        * juce::jlimit(20.0, sampleRate * 0.45, frequency)
        / sampleRate;
    const auto cosine = std::cos(omega);
    const auto sine = std::sin(omega);
    const auto alpha = sine / std::sqrt(2.0);
    const auto beta = 2.0 * std::sqrt(a) * alpha;
    const auto a0 = (a + 1.0)
        - (a - 1.0) * cosine
        + beta;
    filter.b0 = static_cast<float>(
        a * ((a + 1.0) + (a - 1.0) * cosine + beta) / a0);
    filter.b1 = static_cast<float>(
        -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine) / a0);
    filter.b2 = static_cast<float>(
        a * ((a + 1.0) + (a - 1.0) * cosine - beta) / a0);
    filter.a1 = static_cast<float>(
        2.0 * ((a - 1.0) - (a + 1.0) * cosine) / a0);
    filter.a2 = static_cast<float>(
        ((a + 1.0) - (a - 1.0) * cosine - beta) / a0);
}

float AmpDeviceProcessor::parameter(ParameterSlot slot) const noexcept
{
    const auto* value =
        realtimeParameters[static_cast<std::size_t>(slot)];
    return value != nullptr ? value->get() : 0.0f;
}

double AmpDeviceProcessor::getTailLengthSeconds() const
{
    return cabinet->tailSeconds();
}

bool AmpDeviceProcessor::acceptsMidi() const
{
    return false;
}

bool AmpDeviceProcessor::producesMidi() const
{
    return false;
}

juce::AudioProcessorEditor* AmpDeviceProcessor::createEditor()
{
    return new AmpDeviceEditor(*this);
}

bool AmpDeviceProcessor::hasEditor() const
{
    return true;
}

int AmpDeviceProcessor::getNumPrograms()
{
    return 1;
}

int AmpDeviceProcessor::getCurrentProgram()
{
    return 0;
}

void AmpDeviceProcessor::setCurrentProgram(int)
{
}

const juce::String AmpDeviceProcessor::getProgramName(int)
{
    return type == AmpDeviceType::guitar
        ? "Modern 4x12"
        : "Tight 8x10";
}

void AmpDeviceProcessor::changeProgramName(int, const juce::String&)
{
}

void AmpDeviceProcessor::getStateInformation(
    juce::MemoryBlock& destination)
{
    if (saveValidatedState(destination).failed())
        destination.reset();
}

void AmpDeviceProcessor::setStateInformation(const void* data, int size)
{
    if (const auto result = restoreValidatedState(data, size);
        result.failed())
        cabinet->reportError(result.getErrorMessage());
}

juce::Result AmpDeviceProcessor::saveValidatedState(
    juce::MemoryBlock& destination)
{
    const auto asset = cabinet->assetSnapshot();
    destination.reset();
    juce::MemoryOutputStream output(destination, false);
    output.writeInt64(stateMagic);
    output.writeInt(stateVersion);
    output.writeInt(static_cast<int>(type));
    output.writeInt(static_cast<int>(parameterLookup.size()));
    for (const auto& [id, value] : parameterLookup)
    {
        juce::ignoreUnused(id);
        output.writeFloat(
            static_cast<juce::AudioProcessorParameter*>(value)
                ->getValue());
    }
    output.writeBool(asset.embedded);
    if (!writeBoundedString(output, asset.name))
        return juce::Result::fail("Cabinet name is too long to save.");
    output.writeDouble(asset.sampleRate);
    output.writeInt(asset.channels);
    output.writeInt(static_cast<int>(asset.left.size()));
    if (!asset.embedded)
    {
        output.write(
            asset.left.data(),
            asset.left.size() * sizeof(float));
        if (asset.channels > 1)
        {
            output.write(
                asset.right.data(),
                asset.right.size() * sizeof(float));
        }
    }
    output.flush();
    return destination.isEmpty()
        ? juce::Result::fail("Could not serialize amp device state.")
        : juce::Result::ok();
}

juce::Result AmpDeviceProcessor::restoreValidatedState(
    const void* data,
    int size)
{
    if (data == nullptr || size <= 0)
        return juce::Result::fail("Amp device state is empty.");
    juce::MemoryInputStream input(data, static_cast<std::size_t>(size), false);
    if (input.getNumBytesRemaining()
            < static_cast<juce::int64>(
                sizeof(juce::int64) + sizeof(int) * 3)
        || input.readInt64() != stateMagic
        || input.readInt() != stateVersion)
    {
        return juce::Result::fail(
            "Amp device state has an unsupported format.");
    }
    const auto savedType = input.readInt();
    const auto parameterCount = input.readInt();
    if (savedType != static_cast<int>(type)
        || parameterCount != static_cast<int>(parameterLookup.size()))
    {
        return juce::Result::fail(
            "Amp device state belongs to a different device.");
    }
    std::vector<float> parameters(
        static_cast<std::size_t>(parameterCount));
    for (auto& value : parameters)
    {
        if (input.getNumBytesRemaining()
            < static_cast<juce::int64>(sizeof(float)))
            return juce::Result::fail("Amp device state is truncated.");
        value = input.readFloat();
        if (!std::isfinite(value)
            || value < 0.0f
            || value > 1.0f)
        {
            return juce::Result::fail(
                "Amp device state contains an invalid parameter.");
        }
    }
    if (input.getNumBytesRemaining() < 1)
        return juce::Result::fail("Amp device state is truncated.");
    CabinetAsset asset;
    asset.embedded = input.readBool();
    const auto name = readBoundedString(input);
    if (!name.has_value()
        || input.getNumBytesRemaining()
            < static_cast<juce::int64>(
                sizeof(double) + sizeof(int) * 2))
    {
        return juce::Result::fail(
            "Amp device cabinet state is truncated.");
    }
    asset.name = *name;
    asset.sampleRate = input.readDouble();
    asset.channels = input.readInt();
    const auto samples = input.readInt();
    if (asset.embedded)
    {
        asset = makeEmbeddedCabinet(type);
    }
    else
    {
        if (samples <= 0
            || samples > CabinetConvolver::maximumImpulseSamples
            || asset.channels < 1
            || asset.channels > 2)
        {
            return juce::Result::fail(
                "Amp device cabinet state has invalid dimensions.");
        }
        const auto values = static_cast<std::size_t>(
            samples * asset.channels);
        const auto bytes = values * sizeof(float);
        if (input.getNumBytesRemaining()
            != static_cast<juce::int64>(bytes))
            return juce::Result::fail(
                "Amp device cabinet state is truncated.");
        asset.left.resize(static_cast<std::size_t>(samples));
        if (input.read(
                asset.left.data(),
                samples * static_cast<int>(sizeof(float)))
            != samples * static_cast<int>(sizeof(float)))
        {
            return juce::Result::fail(
                "Amp device cabinet state is truncated.");
        }
        if (asset.channels > 1)
        {
            asset.right.resize(static_cast<std::size_t>(samples));
            if (input.read(
                    asset.right.data(),
                    samples * static_cast<int>(sizeof(float)))
                != samples * static_cast<int>(sizeof(float)))
            {
                return juce::Result::fail(
                    "Amp device cabinet state is truncated.");
            }
        }
    }
    if (input.getNumBytesRemaining() != 0)
        return juce::Result::fail("Amp device state contains trailing data.");
    if (const auto result = cabinet->restoreAsset(
            std::move(asset),
            currentSampleRate.load(std::memory_order_acquire));
        result.failed())
        return result;
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        static_cast<juce::AudioProcessorParameter*>(
            parameterLookup[index].second)
            ->setValue(parameters[index]);
    }
    reset();
    return juce::Result::ok();
}

bool AmpDeviceProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return (input == juce::AudioChannelSet::mono()
            || input == juce::AudioChannelSet::stereo())
        && output == input;
}

AmpDeviceType AmpDeviceProcessor::ampType() const noexcept
{
    return type;
}

juce::Result AmpDeviceProcessor::loadCabinetFile(
    const juce::File& file)
{
    const auto result = cabinet->loadFile(
        file,
        currentSampleRate.load(std::memory_order_acquire));
    if (result.wasOk())
    {
        updateHostDisplay(
            juce::AudioProcessor::ChangeDetails {}
                .withProgramChanged(true));
    }
    return result;
}

juce::Result AmpDeviceProcessor::useEmbeddedDefaultCabinet()
{
    const auto result = cabinet->useEmbedded(
        currentSampleRate.load(std::memory_order_acquire));
    if (result.wasOk())
    {
        updateHostDisplay(
            juce::AudioProcessor::ChangeDetails {}
                .withProgramChanged(true));
    }
    return result;
}

juce::String AmpDeviceProcessor::cabinetDescription() const
{
    return cabinet->description();
}

#if STUDIO_DUO_TESTING
void AmpDeviceProcessor::setCabinetReaderBarrierForTesting(
    CabinetReaderBarrierForTesting* barrier) noexcept
{
    cabinet->setReaderBarrierForTesting(barrier);
}
#endif
}

#include "AraDocumentHost.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

#if JUCE_PLUGINHOST_ARA
#include <ARA_Library/Dispatch/ARAHostDispatch.h>

#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#endif

namespace studio
{
namespace
{
constexpr auto araStateMagic = static_cast<juce::int64>(
    0x5344415241535431ULL);

void appendFingerprintValue(juce::String& value,
                            const juce::String& key,
                            const juce::String& item)
{
    value << key << "=" << item << ";";
}
}

#if JUCE_PLUGINHOST_ARA
namespace
{
class FileAudioSource
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<
        FileAudioSource*,
        ARA::ARAAudioSourceHostRef>;

    static std::unique_ptr<FileAudioSource> create(
        ARA::Host::DocumentController& controller,
        const AraDocumentDescriptor::AudioRegion& descriptor,
        juce::String& error)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            formats.createReaderFor(descriptor.sourceFile));
        if (reader == nullptr)
        {
            error = "Could not open ARA audio source "
                + descriptor.sourceFile.getFullPathName();
            return {};
        }
        if (reader->lengthInSamples <= 0
            || reader->sampleRate <= 0.0
            || reader->numChannels == 0)
        {
            error = "ARA audio source metadata is invalid for "
                + descriptor.sourceFile.getFullPathName();
            return {};
        }
        return std::unique_ptr<FileAudioSource>(
            new FileAudioSource(
                controller,
                descriptor,
                reader->lengthInSamples,
                reader->sampleRate,
                static_cast<int>(reader->numChannels)));
    }

    [[nodiscard]] const juce::File& file() const noexcept
    {
        return sourceFile;
    }

    [[nodiscard]] int channels() const noexcept
    {
        return channelCount;
    }

    [[nodiscard]] ARA::ARAAudioSourceRef pluginRef() const noexcept
    {
        return source.getPluginRef();
    }

    [[nodiscard]] const juce::String& persistentId() const noexcept
    {
        return sourceId;
    }

    juce::ARAHostModel::AudioSource& model() noexcept
    {
        return source;
    }

    ~FileAudioSource()
    {
        source.enableAudioSourceSamplesAccess(false);
    }

private:
    FileAudioSource(
        ARA::Host::DocumentController& controller,
        const AraDocumentDescriptor::AudioRegion& descriptor,
        std::int64_t lengthInSamples,
        double sourceSampleRate,
        int sourceChannels)
        : sourceFile(descriptor.sourceFile),
          displayName(descriptor.clipName.isNotEmpty()
                          ? descriptor.clipName
                          : descriptor.sourceFile.getFileName()),
          sourceId(descriptor.sourceId),
          sampleCount(lengthInSamples),
          sampleRate(sourceSampleRate),
          channelCount(sourceChannels),
          source(Converter::toHostRef(this),
                 controller,
                 properties())
    {
        source.enableAudioSourceSamplesAccess(true);
    }

    [[nodiscard]] ARA::ARAAudioSourceProperties properties() const
    {
        auto value =
            juce::ARAHostModel::AudioSource::getEmptyProperties();
        value.name = displayName.toRawUTF8();
        value.persistentID = sourceId.toRawUTF8();
        value.sampleCount = sampleCount;
        value.sampleRate = sampleRate;
        value.channelCount = channelCount;
        value.merits64BitSamples = ARA::kARAFalse;
        value.channelArrangementDataType =
            ARA::kARAChannelArrangementUndefined;
        value.channelArrangement = nullptr;
        return value;
    }

    juce::File sourceFile;
    juce::String displayName;
    juce::String sourceId;
    std::int64_t sampleCount = 0;
    double sampleRate = 0.0;
    int channelCount = 0;
    juce::ARAHostModel::AudioSource source;
};

class AudioAccessController final
    : public ARA::Host::AudioAccessControllerInterface
{
public:
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(
        ARA::ARAAudioSourceHostRef sourceRef,
        bool use64BitSamples) noexcept override
    {
        auto* source = FileAudioSource::Converter::fromHostRef(
            sourceRef);
        if (source == nullptr)
            return nullptr;

        auto reader = std::make_unique<AudioReader>(
            source->file(),
            source->channels(),
            use64BitSamples);
        if (reader->reader == nullptr)
            return nullptr;

        const auto hostRef = ReaderConverter::toHostRef(reader.get());
        const std::lock_guard lock(readersMutex);
        readers.emplace(reader.get(), std::move(reader));
        return hostRef;
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef readerRef,
                          ARA::ARASamplePosition samplePosition,
                          ARA::ARASampleCount samplesPerChannel,
                          void* const* buffers) noexcept override
    {
        if (samplesPerChannel < 0 || buffers == nullptr)
            return false;
        const std::lock_guard lock(readersMutex);
        const auto found = readers.find(
            ReaderConverter::fromHostRef(readerRef));
        if (found == readers.end())
            return false;
        return found->second->read(
            samplePosition,
            samplesPerChannel,
            buffers);
    }

    void destroyAudioReader(
        ARA::ARAAudioReaderHostRef readerRef) noexcept override
    {
        const std::lock_guard lock(readersMutex);
        readers.erase(ReaderConverter::fromHostRef(readerRef));
    }

private:
    struct AudioReader
    {
        AudioReader(const juce::File& sourceFile,
                    int sourceChannels,
                    bool useDoublePrecision)
            : channels(sourceChannels),
              use64Bit(useDoublePrecision)
        {
            formats.registerBasicFormats();
            reader.reset(formats.createReaderFor(sourceFile));
        }

        bool read(ARA::ARASamplePosition startSample,
                  ARA::ARASampleCount sampleCount,
                  void* const* buffers)
        {
            auto remaining = sampleCount;
            auto offset = ARA::ARASampleCount {};
            while (remaining > 0)
            {
                const auto chunk = static_cast<int>(
                    std::min<ARA::ARASampleCount>(
                        remaining,
                        std::numeric_limits<int>::max()));
                scratch.setSize(
                    channels,
                    chunk,
                    false,
                    false,
                    true);
                if (!reader->read(
                        scratch.getArrayOfWritePointers(),
                        channels,
                        startSample + offset,
                        chunk))
                {
                    return false;
                }

                for (int channel = 0; channel < channels; ++channel)
                {
                    if (buffers[channel] == nullptr)
                        continue;
                    if (use64Bit)
                    {
                        auto* destination =
                            static_cast<double*>(buffers[channel])
                            + offset;
                        const auto* source =
                            scratch.getReadPointer(channel);
                        for (int sample = 0; sample < chunk; ++sample)
                        {
                            destination[sample] =
                                static_cast<double>(source[sample]);
                        }
                    }
                    else
                    {
                        std::copy_n(
                            scratch.getReadPointer(channel),
                            chunk,
                            static_cast<float*>(buffers[channel])
                                + offset);
                    }
                }
                remaining -= chunk;
                offset += chunk;
            }
            return true;
        }

        juce::AudioFormatManager formats;
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::AudioBuffer<float> scratch;
        int channels = 0;
        bool use64Bit = false;
    };

    using ReaderConverter = juce::ARAHostModel::ConversionFunctions<
        AudioReader*,
        ARA::ARAAudioReaderHostRef>;

    std::mutex readersMutex;
    std::map<AudioReader*, std::unique_ptr<AudioReader>> readers;
};

class ArchivingController final
    : public ARA::Host::ArchivingControllerInterface
{
public:
    using ReaderConverter =
        juce::ARAHostModel::ConversionFunctions<
            juce::MemoryBlock*,
            ARA::ARAArchiveReaderHostRef>;
    struct ArchiveWriter
    {
        juce::MemoryBlock data;
    };

    using WriterConverter =
        juce::ARAHostModel::ConversionFunctions<
            ArchiveWriter*,
            ARA::ARAArchiveWriterHostRef>;

    ARA::ARASize getArchiveSize(
        ARA::ARAArchiveReaderHostRef reader) noexcept override
    {
        return static_cast<ARA::ARASize>(
            ReaderConverter::fromHostRef(reader)->getSize());
    }

    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef reader,
                              ARA::ARASize position,
                              ARA::ARASize length,
                              ARA::ARAByte* buffer) noexcept override
    {
        auto* source = ReaderConverter::fromHostRef(reader);
        if (source == nullptr
            || (length > 0 && buffer == nullptr)
            || position > source->getSize()
            || length > source->getSize() - position)
            return false;
        std::memcpy(buffer,
                    juce::addBytesToPointer(
                        source->getData(),
                        static_cast<std::size_t>(position)),
                    static_cast<std::size_t>(length));
        return true;
    }

    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef writer,
                             ARA::ARASize position,
                             ARA::ARASize length,
                             const ARA::ARAByte* buffer) noexcept override
    {
        auto* destination = WriterConverter::fromHostRef(writer);
        if (destination == nullptr
            || (length > 0 && buffer == nullptr)
            || position
                   > std::numeric_limits<ARA::ARASize>::max()
                       - length
            || position + length
                   > static_cast<ARA::ARASize>(
                       std::numeric_limits<std::size_t>::max()))
        {
            return false;
        }

        const auto oldSize = destination->data.getSize();
        const auto writePosition =
            static_cast<std::size_t>(position);
        const auto writeLength =
            static_cast<std::size_t>(length);
        const auto requiredSize = writePosition + writeLength;
        if (requiredSize > oldSize)
            destination->data.setSize(requiredSize, true);
        if (writePosition > oldSize)
        {
            std::memset(
                juce::addBytesToPointer(
                    destination->data.getData(),
                    oldSize),
                0,
                writePosition - oldSize);
        }
        if (writeLength > 0)
        {
            std::memcpy(
                juce::addBytesToPointer(
                    destination->data.getData(),
                    writePosition),
                buffer,
                writeLength);
        }
        return true;
    }

    void notifyDocumentArchivingProgress(float) noexcept override
    {
    }

    void notifyDocumentUnarchivingProgress(float) noexcept override
    {
    }

    ARA::ARAPersistentID getDocumentArchiveID(
        ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return nullptr;
    }
};

class ContentAccessController final
    : public ARA::Host::ContentAccessControllerInterface
{
public:
    explicit ContentAccessController(
        const AraDocumentDescriptor& descriptor)
    {
        tempoEntries.reserve(descriptor.tempoEntries.size());
        for (const auto& entry : descriptor.tempoEntries)
        {
            tempoEntries.push_back({
                entry.timeSeconds,
                entry.quarterPosition
            });
        }
        meterEntries.reserve(descriptor.meterEntries.size());
        for (const auto& entry : descriptor.meterEntries)
        {
            meterEntries.push_back({
                entry.numerator,
                entry.denominator,
                entry.quarterPosition
            });
        }
    }

    bool isMusicalContextContentAvailable(
        ARA::ARAMusicalContextHostRef,
        ARA::ARAContentType type) noexcept override
    {
        return type == ARA::kARAContentTypeTempoEntries
            || type == ARA::kARAContentTypeBarSignatures;
    }

    ARA::ARAContentGrade getMusicalContextContentGrade(
        ARA::ARAMusicalContextHostRef,
        ARA::ARAContentType) noexcept override
    {
        return ARA::kARAContentGradeApproved;
    }

    ARA::ARAContentReaderHostRef createMusicalContextContentReader(
        ARA::ARAMusicalContextHostRef,
        ARA::ARAContentType type,
        const ARA::ARAContentTimeRange*) noexcept override
    {
        if (type != ARA::kARAContentTypeTempoEntries
            && type != ARA::kARAContentTypeBarSignatures)
        {
            return nullptr;
        }
        auto reader = std::make_unique<Reader>();
        reader->type = type;
        const auto hostRef = ReaderConverter::toHostRef(reader.get());
        const std::lock_guard lock(readersMutex);
        readers.emplace(reader.get(), std::move(reader));
        return hostRef;
    }

    bool isAudioSourceContentAvailable(
        ARA::ARAAudioSourceHostRef,
        ARA::ARAContentType) noexcept override
    {
        return false;
    }

    ARA::ARAContentGrade getAudioSourceContentGrade(
        ARA::ARAAudioSourceHostRef,
        ARA::ARAContentType) noexcept override
    {
        return ARA::kARAContentGradeInitial;
    }

    ARA::ARAContentReaderHostRef createAudioSourceContentReader(
        ARA::ARAAudioSourceHostRef,
        ARA::ARAContentType,
        const ARA::ARAContentTimeRange*) noexcept override
    {
        return nullptr;
    }

    ARA::ARAInt32 getContentReaderEventCount(
        ARA::ARAContentReaderHostRef readerRef) noexcept override
    {
        const std::lock_guard lock(readersMutex);
        const auto found = readers.find(
            ReaderConverter::fromHostRef(readerRef));
        if (found == readers.end())
            return 0;
        return static_cast<ARA::ARAInt32>(
            found->second->type == ARA::kARAContentTypeTempoEntries
                ? tempoEntries.size()
                : meterEntries.size());
    }

    const void* getContentReaderDataForEvent(
        ARA::ARAContentReaderHostRef readerRef,
        ARA::ARAInt32 eventIndex) noexcept override
    {
        if (eventIndex < 0)
            return nullptr;
        const std::lock_guard lock(readersMutex);
        const auto found = readers.find(
            ReaderConverter::fromHostRef(readerRef));
        if (found == readers.end())
            return nullptr;
        const auto index = static_cast<std::size_t>(eventIndex);
        if (found->second->type == ARA::kARAContentTypeTempoEntries)
        {
            return index < tempoEntries.size()
                ? &tempoEntries[index]
                : nullptr;
        }
        return index < meterEntries.size()
            ? &meterEntries[index]
            : nullptr;
    }

    void destroyContentReader(
        ARA::ARAContentReaderHostRef readerRef) noexcept override
    {
        const std::lock_guard lock(readersMutex);
        readers.erase(ReaderConverter::fromHostRef(readerRef));
    }

private:
    struct Reader
    {
        ARA::ARAContentType type =
            ARA::kARAContentTypeTempoEntries;
    };

    using ReaderConverter = juce::ARAHostModel::ConversionFunctions<
        Reader*,
        ARA::ARAContentReaderHostRef>;

    std::vector<ARA::ARAContentTempoEntry> tempoEntries;
    std::vector<ARA::ARAContentBarSignature> meterEntries;
    std::mutex readersMutex;
    std::map<Reader*, std::unique_ptr<Reader>> readers;
};

class ModelUpdateController final
    : public ARA::Host::ModelUpdateControllerInterface
{
public:
    void notifyAudioSourceAnalysisProgress(
        ARA::ARAAudioSourceHostRef,
        ARA::ARAAnalysisProgressState,
        float) noexcept override
    {
    }

    void notifyAudioSourceContentChanged(
        ARA::ARAAudioSourceHostRef,
        const ARA::ARAContentTimeRange*,
        ARA::ContentUpdateScopes) noexcept override
    {
    }

    void notifyAudioModificationContentChanged(
        ARA::ARAAudioModificationHostRef,
        const ARA::ARAContentTimeRange*,
        ARA::ContentUpdateScopes) noexcept override
    {
    }

    void notifyPlaybackRegionContentChanged(
        ARA::ARAPlaybackRegionHostRef,
        const ARA::ARAContentTimeRange*,
        ARA::ContentUpdateScopes) noexcept override
    {
    }

    void notifyDocumentDataChanged() noexcept override
    {
    }
};

class MusicalContext
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<
        MusicalContext*,
        ARA::ARAMusicalContextHostRef>;

    MusicalContext(ARA::Host::DocumentController& controller,
                   const juce::String& documentName)
        : name(documentName),
          context(Converter::toHostRef(this),
                  controller,
                  properties())
    {
    }

    [[nodiscard]] ARA::ARAMusicalContextRef pluginRef() const
    {
        return context.getPluginRef();
    }

private:
    [[nodiscard]] ARA::ARAMusicalContextProperties properties() const
    {
        auto value =
            juce::ARAHostModel::MusicalContext::getEmptyProperties();
        value.name = name.toRawUTF8();
        value.orderIndex = 0;
        value.color = nullptr;
        return value;
    }

    juce::String name;
    juce::ARAHostModel::MusicalContext context;
};

class RegionSequence
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<
        RegionSequence*,
        ARA::ARARegionSequenceHostRef>;

    RegionSequence(ARA::Host::DocumentController& controller,
                   MusicalContext& musicalContext,
                   juce::String sequenceName,
                   int order)
        : context(musicalContext),
          name(std::move(sequenceName)),
          orderIndex(order),
          sequence(Converter::toHostRef(this),
                   controller,
                   properties())
    {
    }

    [[nodiscard]] ARA::ARARegionSequenceRef pluginRef() const
    {
        return sequence.getPluginRef();
    }

    [[nodiscard]] ARA::ARAMusicalContextRef contextRef() const
    {
        return context.pluginRef();
    }

private:
    [[nodiscard]] ARA::ARARegionSequenceProperties properties() const
    {
        auto value =
            juce::ARAHostModel::RegionSequence::getEmptyProperties();
        value.name = name.toRawUTF8();
        value.orderIndex = orderIndex;
        value.musicalContextRef = context.pluginRef();
        value.color = nullptr;
        return value;
    }

    MusicalContext& context;
    juce::String name;
    int orderIndex = 0;
    juce::ARAHostModel::RegionSequence sequence;
};

class AudioModification
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<
        AudioModification*,
        ARA::ARAAudioModificationHostRef>;

    AudioModification(
        ARA::Host::DocumentController& controller,
        FileAudioSource& audioSource,
        const AraDocumentDescriptor::AudioRegion& descriptor)
        : name(descriptor.clipName),
          modificationId(descriptor.modificationId),
          modification(Converter::toHostRef(this),
                       controller,
                       audioSource.model(),
                       properties())
    {
    }

    [[nodiscard]] ARA::ARAAudioModificationRef pluginRef() const
    {
        return modification.getPluginRef();
    }

    [[nodiscard]] const juce::String& persistentId() const noexcept
    {
        return modificationId;
    }

    juce::ARAHostModel::AudioModification& model() noexcept
    {
        return modification;
    }

private:
    [[nodiscard]] ARA::ARAAudioModificationProperties properties() const
    {
        auto value =
            juce::ARAHostModel::AudioModification::getEmptyProperties();
        value.name = name.toRawUTF8();
        value.persistentID = modificationId.toRawUTF8();
        return value;
    }

    juce::String name;
    juce::String modificationId;
    juce::ARAHostModel::AudioModification modification;
};

class PlaybackRegion
{
public:
    using Converter = juce::ARAHostModel::ConversionFunctions<
        PlaybackRegion*,
        ARA::ARAPlaybackRegionHostRef>;

    PlaybackRegion(
        ARA::Host::DocumentController& controller,
        RegionSequence& regionSequence,
        AudioModification& audioModification,
        const AraDocumentDescriptor::AudioRegion& descriptor,
        ARA::ARAPlaybackTransformationFlags supportedTransformations)
        : sequence(regionSequence),
          name(descriptor.clipName),
          regionId(descriptor.regionId),
          startInModification(descriptor.startInModificationSeconds),
          durationInModification(
              descriptor.durationInModificationSeconds),
          startInPlayback(descriptor.startInPlaybackSeconds),
          durationInPlayback(descriptor.durationInPlaybackSeconds),
          transformations(
              std::abs(durationInModification - durationInPlayback)
                          > 0.0000001
                      && (supportedTransformations
                          & ARA::kARAPlaybackTransformationTimestretch)
                      != 0
                  ? ARA::kARAPlaybackTransformationTimestretch
                  : ARA::kARAPlaybackTransformationNoChanges),
          region(Converter::toHostRef(this),
                 controller,
                 audioModification.model(),
                 properties())
    {
    }

    juce::ARAHostModel::PlaybackRegion& model() noexcept
    {
        return region;
    }

private:
    [[nodiscard]] ARA::ARAPlaybackRegionProperties properties() const
    {
        auto value =
            juce::ARAHostModel::PlaybackRegion::getEmptyProperties();
        value.transformationFlags = transformations;
        value.startInModificationTime = startInModification;
        value.durationInModificationTime =
            transformations == ARA::kARAPlaybackTransformationNoChanges
            ? durationInPlayback
            : durationInModification;
        value.startInPlaybackTime = startInPlayback;
        value.durationInPlaybackTime = durationInPlayback;
        value.musicalContextRef = sequence.contextRef();
        value.regionSequenceRef = sequence.pluginRef();
        value.name = name.toRawUTF8();
        value.color = nullptr;
        return value;
    }

    RegionSequence& sequence;
    juce::String name;
    juce::String regionId;
    double startInModification = 0.0;
    double durationInModification = 0.0;
    double startInPlayback = 0.0;
    double durationInPlayback = 0.0;
    ARA::ARAPlaybackTransformationFlags transformations =
        ARA::kARAPlaybackTransformationNoChanges;
    juce::ARAHostModel::PlaybackRegion region;
};
}
#endif

#if JUCE_PLUGINHOST_ARA
struct AraDocumentHost::Impl
{
    ~Impl()
    {
        playbackRegions.clear();
        playbackRenderer =
            juce::ARAHostModel::PlaybackRendererInterface {};
        editorRenderer =
            juce::ARAHostModel::EditorRendererInterface {};
        modifications.clear();
        audioSources.clear();
        sequences.clear();
        musicalContext.reset();
        binding = juce::ARAHostModel::PlugInExtensionInstance {};
        controller.reset();
    }

    juce::Result createModel(
        ARA::ARAPlaybackTransformationFlags supportedTransformations,
        const juce::MemoryBlock& archivedState)
    {
        auto& documentController =
            controller->getDocumentController();
        {
            const juce::ARAEditGuard editGuard(documentController);
            musicalContext = std::make_unique<MusicalContext>(
                documentController,
                descriptor->name);

            std::map<juce::String, RegionSequence*> sequencesByTrack;
            for (const auto& audioRegion : descriptor->audioRegions)
            {
                if (sequencesByTrack.find(audioRegion.trackId)
                    != sequencesByTrack.end())
                {
                    continue;
                }
                auto sequence = std::make_unique<RegionSequence>(
                    documentController,
                    *musicalContext,
                    audioRegion.trackName,
                    static_cast<int>(sequences.size()));
                sequencesByTrack.emplace(
                    audioRegion.trackId,
                    sequence.get());
                sequences.push_back(std::move(sequence));
            }

            for (const auto& audioRegion : descriptor->audioRegions)
            {
                auto source = FileAudioSource::create(
                    documentController,
                    audioRegion,
                    error);
                if (source == nullptr)
                    return juce::Result::fail(error);
                auto modification =
                    std::make_unique<AudioModification>(
                        documentController,
                        *source,
                        audioRegion);
                auto sequence = sequencesByTrack.find(
                    audioRegion.trackId);
                if (sequence == sequencesByTrack.end())
                    return juce::Result::fail(
                        "ARA region sequence is unavailable.");
                auto playbackRegion =
                    std::make_unique<PlaybackRegion>(
                        documentController,
                        *sequence->second,
                        *modification,
                        audioRegion,
                        supportedTransformations);
                audioSources.push_back(std::move(source));
                modifications.push_back(std::move(modification));
                playbackRegions.push_back(
                    std::move(playbackRegion));
            }
        }

        if (!archivedState.isEmpty())
        {
            if (const auto restored = restore(archivedState);
                restored.failed())
            {
                return restored;
            }
        }

        playbackRenderer = binding.getPlaybackRendererInterface();
        editorRenderer = binding.getEditorRendererInterface();
        for (auto& region : playbackRegions)
        {
            playbackRenderer.add(region->model());
            editorRenderer.add(region->model());
        }
        documentController.notifyModelUpdates();
        return juce::Result::ok();
    }

    juce::Result archive(juce::MemoryBlock& state) const
    {
        state.reset();
        if (controller == nullptr)
            return juce::Result::fail(
                "ARA document controller is unavailable.");

        std::vector<ARA::ARAAudioSourceRef> sourceRefs;
        sourceRefs.reserve(audioSources.size());
        for (const auto& source : audioSources)
            sourceRefs.push_back(source->pluginRef());
        std::vector<ARA::ARAAudioModificationRef> modificationRefs;
        modificationRefs.reserve(modifications.size());
        for (const auto& modification : modifications)
            modificationRefs.push_back(modification->pluginRef());

        auto filter = juce::makeARASizedStruct(
            &ARA::ARAStoreObjectsFilter::audioModificationRefs);
        filter.documentData = ARA::kARATrue;
        filter.audioSourceRefsCount = sourceRefs.size();
        filter.audioSourceRefs = sourceRefs.data();
        filter.audioModificationRefsCount = modificationRefs.size();
        filter.audioModificationRefs = modificationRefs.data();

        ArchivingController::ArchiveWriter writer;
        auto& documentController =
            controller->getDocumentController();
        const auto stored = documentController.storeObjectsToArchive(
            ArchivingController::WriterConverter::toHostRef(&writer),
            &filter);
        if (!stored)
            return juce::Result::fail(
                "The ARA plugin rejected document archiving.");
        state = std::move(writer.data);
        return juce::Result::ok();
    }

    juce::Result restore(const juce::MemoryBlock& state)
    {
        if (controller == nullptr || state.isEmpty())
            return juce::Result::ok();

        std::vector<ARA::ARAPersistentID> sourceIds;
        sourceIds.reserve(audioSources.size());
        for (const auto& source : audioSources)
            sourceIds.push_back(source->persistentId().toRawUTF8());
        std::vector<ARA::ARAPersistentID> modificationIds;
        modificationIds.reserve(modifications.size());
        for (const auto& modification : modifications)
        {
            modificationIds.push_back(
                modification->persistentId().toRawUTF8());
        }

        auto filter = juce::makeARASizedStruct(
            &ARA::ARARestoreObjectsFilter::audioModificationCurrentIDs);
        filter.documentData = ARA::kARATrue;
        filter.audioSourceIDsCount = sourceIds.size();
        filter.audioSourceArchiveIDs = sourceIds.data();
        filter.audioSourceCurrentIDs = nullptr;
        filter.audioModificationIDsCount = modificationIds.size();
        filter.audioModificationArchiveIDs = modificationIds.data();
        filter.audioModificationCurrentIDs = nullptr;

        auto archive = state;
        auto& documentController =
            controller->getDocumentController();
        const juce::ARAEditGuard editGuard(documentController);
        const auto restored = documentController.restoreObjectsFromArchive(
            ArchivingController::ReaderConverter::toHostRef(
                &archive),
            &filter);
        return restored
            ? juce::Result::ok()
            : juce::Result::fail(
                  "The ARA plugin rejected document state restore.");
    }

    std::shared_ptr<const AraDocumentDescriptor> descriptor;
    std::unique_ptr<juce::ARAHostDocumentController> controller;
    juce::ARAHostModel::PlugInExtensionInstance binding;
    std::unique_ptr<MusicalContext> musicalContext;
    std::vector<std::unique_ptr<RegionSequence>> sequences;
    std::vector<std::unique_ptr<FileAudioSource>> audioSources;
    std::vector<std::unique_ptr<AudioModification>> modifications;
    std::vector<std::unique_ptr<PlaybackRegion>> playbackRegions;
    juce::ARAHostModel::PlaybackRendererInterface playbackRenderer;
    juce::ARAHostModel::EditorRendererInterface editorRenderer;
    juce::String error;
};
#endif

AraDocumentHost::AraDocumentHost() = default;

AraDocumentHost::~AraDocumentHost() = default;

juce::Result AraDocumentHost::bind(juce::AudioPluginInstance& instance)
{
    auto descriptor = std::make_shared<AraDocumentDescriptor>();
    descriptor->name = "Studio Duo Project";
    return bind(instance, std::move(descriptor));
}

juce::Result AraDocumentHost::bind(
    juce::AudioPluginInstance& instance,
    std::shared_ptr<const AraDocumentDescriptor> descriptor,
    const juce::MemoryBlock& archivedState)
{
#if JUCE_PLUGINHOST_ARA
    if (instance.getARAClient() == nullptr)
        return juce::Result::fail(
            "The plugin instance does not expose an ARA client.");

    struct FactoryState
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<juce::ARAFactoryWrapper> factory;
        bool completed = false;
    };
    const auto state = std::make_shared<FactoryState>();
    juce::createARAFactoryAsync(
        instance,
        [state](juce::ARAFactoryWrapper factory)
        {
            {
                const std::lock_guard lock(state->mutex);
                state->factory.emplace(std::move(factory));
                state->completed = true;
            }
            state->condition.notify_one();
        });

    std::unique_lock lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [state] { return state->completed; }))
        return juce::Result::fail("Timed out while creating the ARA 2 document.");
    auto factory = std::move(*state->factory);
    lock.unlock();
    if (factory.get() == nullptr)
        return juce::Result::fail(
            "The plugin did not provide an ARA 2 factory.");

    const auto supportedTransformations =
        factory.get()->supportedPlaybackTransformationFlags;
    auto next = std::make_unique<Impl>();
    next->descriptor = descriptor != nullptr
        ? std::move(descriptor)
        : std::make_shared<AraDocumentDescriptor>();
    next->controller = juce::ARAHostDocumentController::create(
        std::move(factory),
        next->descriptor->name.isNotEmpty()
            ? next->descriptor->name
            : juce::String("Studio Duo Project"),
        std::make_unique<AudioAccessController>(),
        std::make_unique<ArchivingController>(),
        std::make_unique<ContentAccessController>(*next->descriptor),
        std::make_unique<ModelUpdateController>());
    if (next->controller == nullptr)
        return juce::Result::fail(
            "Could not create the ARA document controller.");

    constexpr auto roles =
        ARA::kARAPlaybackRendererRole
        | ARA::kARAEditorRendererRole
        | ARA::kARAEditorViewRole;
    next->binding = next->controller->bindDocumentToPluginInstance(
        instance,
        roles,
        roles);
    if (!next->binding.isValid())
        return juce::Result::fail(
            "Could not bind the ARA document to the plugin.");
    if (const auto result = next->createModel(
            supportedTransformations,
            archivedState);
        result.failed())
    {
        return result;
    }
    impl = std::move(next);
    return juce::Result::ok();
#else
    juce::ignoreUnused(instance, descriptor, archivedState);
    return juce::Result::fail(
        "This build does not include ARA 2 hosting.");
#endif
}

juce::Result AraDocumentHost::archive(juce::MemoryBlock& state) const
{
#if JUCE_PLUGINHOST_ARA
    return impl != nullptr
        ? impl->archive(state)
        : juce::Result::fail("ARA document is not bound.");
#else
    state.reset();
    return juce::Result::fail(
        "This build does not include ARA 2 hosting.");
#endif
}

bool AraDocumentHost::isBound() const noexcept
{
#if JUCE_PLUGINHOST_ARA
    return impl != nullptr
        && impl->controller != nullptr
        && impl->binding.isValid();
#else
    return false;
#endif
}

std::size_t AraDocumentHost::audioSourceCount() const noexcept
{
#if JUCE_PLUGINHOST_ARA
    return impl != nullptr ? impl->audioSources.size() : 0;
#else
    return 0;
#endif
}

std::size_t AraDocumentHost::playbackRegionCount() const noexcept
{
#if JUCE_PLUGINHOST_ARA
    return impl != nullptr ? impl->playbackRegions.size() : 0;
#else
    return 0;
#endif
}

std::shared_ptr<const AraDocumentDescriptor>
AraDocumentHost::describeProject(const Project& project,
                                 const juce::String& trackId)
{
    auto descriptor = std::make_shared<AraDocumentDescriptor>();
    descriptor->name = project.name;
    auto fingerprint = juce::String();
    appendFingerprintValue(fingerprint, "project", project.name);
    appendFingerprintValue(fingerprint, "track", trackId);

    const auto addTempo = [&descriptor, &fingerprint](
                              double timeSeconds,
                              double quarterPosition)
    {
        if (!descriptor->tempoEntries.empty()
            && std::abs(
                   descriptor->tempoEntries.back().timeSeconds
                   - timeSeconds)
                   < 0.0000001)
        {
            descriptor->tempoEntries.back().quarterPosition =
                quarterPosition;
            return;
        }
        descriptor->tempoEntries.push_back({
            std::max(0.0, timeSeconds),
            std::max(0.0, quarterPosition)
        });
        appendFingerprintValue(
            fingerprint,
            "tempo",
            juce::String(timeSeconds, 9)
                + ","
                + juce::String(quarterPosition, 9));
    };
    addTempo(0.0, 0.0);
    for (const auto& change : project.tempoChanges)
    {
        addTempo(change.timeSeconds,
                 project.beatsAt(change.timeSeconds));
        appendFingerprintValue(
            fingerprint,
            "tempo-bpm",
            juce::String(change.bpm, 9)
                + ","
                + (change.rampToNext ? "ramp" : "step"));
    }
    const auto lastTempoTime = descriptor->tempoEntries.empty()
        ? 0.0
        : descriptor->tempoEntries.back().timeSeconds;
    const auto tempoEnd = std::max({
        1.0,
        project.lengthSeconds(),
        lastTempoTime + 1.0
    });
    addTempo(tempoEnd, project.beatsAt(tempoEnd));

    const auto addMeter = [&descriptor, &fingerprint](
                              double timeSeconds,
                              int numerator,
                              int denominator)
    {
        const auto entry = AraDocumentDescriptor::MeterEntry {
            std::max(0.0, timeSeconds),
            numerator,
            denominator
        };
        if (!descriptor->meterEntries.empty()
            && std::abs(
                   descriptor->meterEntries.back().quarterPosition
                   - entry.quarterPosition)
                   < 0.0000001)
        {
            descriptor->meterEntries.back() = entry;
        }
        else
        {
            descriptor->meterEntries.push_back(entry);
        }
        appendFingerprintValue(
            fingerprint,
            "meter",
            juce::String(entry.quarterPosition, 9)
                + ","
                + juce::String(numerator)
                + "/"
                + juce::String(denominator));
    };
    if (project.meterChanges.empty()
        || project.meterChanges.front().timeSeconds > 0.0)
    {
        addMeter(0.0,
                 project.timeSignatureNumerator,
                 project.timeSignatureDenominator);
    }
    for (const auto& change : project.meterChanges)
    {
        addMeter(project.beatsAt(change.timeSeconds),
                 change.numerator,
                 change.denominator);
    }

    for (const auto& track : project.tracks)
    {
        if (track.id != trackId && track.parentTrackId != trackId)
            continue;
        for (const auto& clip : track.clips)
        {
            if (clip.muted || !clip.sourceFile.existsAsFile())
                continue;

            const auto sourceStart = clip.sourceSecondsAt(0.0);
            const auto sourceEnd = clip.sourceSecondsAt(
                clip.durationSeconds);
            AraDocumentDescriptor::AudioRegion region;
            region.sourceId = clip.id + ":source";
            region.modificationId = clip.id + ":modification";
            region.regionId = clip.id;
            region.trackId = track.id;
            region.trackName = track.name;
            region.clipName = clip.name;
            region.sourceFile = clip.sourceFile;
            region.startInPlaybackSeconds =
                std::max(0.0, clip.startSeconds);
            region.startInModificationSeconds =
                std::max(0.0, std::min(sourceStart, sourceEnd));
            region.durationInPlaybackSeconds =
                std::max(0.0, clip.durationSeconds);
            region.durationInModificationSeconds =
                std::max(0.0, std::abs(sourceEnd - sourceStart));
            descriptor->audioRegions.push_back(std::move(region));

            appendFingerprintValue(fingerprint, "clip", clip.id);
            appendFingerprintValue(
                fingerprint,
                "file",
                clip.sourceFile.getFullPathName());
            appendFingerprintValue(
                fingerprint,
                "file-size",
                juce::String(clip.sourceFile.getSize()));
            appendFingerprintValue(
                fingerprint,
                "file-time",
                juce::String(
                    clip.sourceFile.getLastModificationTime()
                        .toMilliseconds()));
            appendFingerprintValue(
                fingerprint,
                "playback",
                juce::String(clip.startSeconds, 9)
                    + ","
                    + juce::String(sourceStart, 9)
                    + ","
                    + juce::String(sourceEnd, 9)
                    + ","
                    + juce::String(clip.durationSeconds, 9));
        }
    }

    descriptor->revision = juce::String::toHexString(
        static_cast<juce::int64>(fingerprint.hashCode64()));
    return descriptor;
}

juce::MemoryBlock AraDocumentHost::packState(
    const juce::MemoryBlock& processorState,
    const juce::MemoryBlock& araState)
{
    juce::MemoryBlock packed;
    juce::MemoryOutputStream stream(packed, true);
    stream.writeInt64(araStateMagic);
    stream.writeInt64(
        static_cast<juce::int64>(processorState.getSize()));
    stream.writeInt64(
        static_cast<juce::int64>(araState.getSize()));
    stream.write(processorState.getData(), processorState.getSize());
    stream.write(araState.getData(), araState.getSize());
    return packed;
}

bool AraDocumentHost::unpackState(const juce::MemoryBlock& storedState,
                                  juce::MemoryBlock& processorState,
                                  juce::MemoryBlock& araState)
{
    processorState = storedState;
    araState.reset();
    if (storedState.getSize() < sizeof(juce::int64) * 3)
        return false;

    juce::MemoryInputStream stream(storedState, false);
    if (stream.readInt64() != araStateMagic)
        return false;
    const auto processorSize = stream.readInt64();
    const auto araSize = stream.readInt64();
    if (processorSize < 0
        || araSize < 0
        || processorSize > std::numeric_limits<int>::max()
        || araSize > std::numeric_limits<int>::max()
        || static_cast<std::uint64_t>(processorSize)
               + static_cast<std::uint64_t>(araSize)
            > static_cast<std::uint64_t>(stream.getNumBytesRemaining()))
    {
        return false;
    }

    processorState.setSize(
        static_cast<std::size_t>(processorSize),
        false);
    araState.setSize(static_cast<std::size_t>(araSize), false);
    if (processorSize > 0)
        stream.read(
            processorState.getData(),
            static_cast<int>(processorSize));
    if (araSize > 0)
        stream.read(
            araState.getData(),
            static_cast<int>(araSize));
    return true;
}

juce::String AraDocumentHost::reducedIsolationWarning()
{
    return "ARA 2 compatibility mode runs the plugin in the Studio Duo process with reduced crash isolation. Save a recovery point before activation.";
}
}

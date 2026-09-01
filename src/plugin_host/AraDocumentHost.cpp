#include "AraDocumentHost.h"

#if JUCE_PLUGINHOST_ARA
#include <ARA_Library/Dispatch/ARAHostDispatch.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#endif

namespace studio
{
#if JUCE_PLUGINHOST_ARA
namespace
{
class AudioAccessController final
    : public ARA::Host::AudioAccessControllerInterface
{
public:
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(
        ARA::ARAAudioSourceHostRef,
        bool) noexcept override
    {
        return nullptr;
    }

    bool readAudioSamples(ARA::ARAAudioReaderHostRef,
                          ARA::ARASamplePosition,
                          ARA::ARASampleCount,
                          void* const*) noexcept override
    {
        return false;
    }

    void destroyAudioReader(ARA::ARAAudioReaderHostRef) noexcept override
    {
    }
};

class ArchivingController final
    : public ARA::Host::ArchivingControllerInterface
{
public:
    using ReaderConverter =
        juce::ARAHostModel::ConversionFunctions<
            juce::MemoryBlock*,
            ARA::ARAArchiveReaderHostRef>;
    using WriterConverter =
        juce::ARAHostModel::ConversionFunctions<
            juce::MemoryOutputStream*,
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
        if (position + length > source->getSize())
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
        return destination->setPosition(
                   static_cast<juce::int64>(position))
            && destination->write(
                buffer,
                static_cast<std::size_t>(length));
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
}
#endif

juce::Result AraDocumentHost::bind(juce::AudioPluginInstance& instance)
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

    documentController = juce::ARAHostDocumentController::create(
        std::move(factory),
        "Studio Duo Project",
        std::make_unique<AudioAccessController>(),
        std::make_unique<ArchivingController>());
    if (documentController == nullptr)
        return juce::Result::fail(
            "Could not create the ARA document controller.");

    constexpr auto roles =
        ARA::kARAPlaybackRendererRole
        | ARA::kARAEditorRendererRole
        | ARA::kARAEditorViewRole;
    binding = documentController->bindDocumentToPluginInstance(
        instance,
        roles,
        roles);
    return binding.isValid()
        ? juce::Result::ok()
        : juce::Result::fail(
              "Could not bind the ARA document to the plugin.");
#else
    juce::ignoreUnused(instance);
    return juce::Result::fail(
        "This build does not include ARA 2 hosting.");
#endif
}

bool AraDocumentHost::isBound() const noexcept
{
#if JUCE_PLUGINHOST_ARA
    return documentController != nullptr && binding.isValid();
#else
    return false;
#endif
}

juce::String AraDocumentHost::reducedIsolationWarning()
{
    return "ARA 2 compatibility mode runs the plugin in the Studio Duo process with reduced crash isolation. Save a recovery point before activation.";
}
}

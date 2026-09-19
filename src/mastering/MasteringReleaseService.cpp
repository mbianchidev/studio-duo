#include "MasteringReleaseService.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#if JUCE_MAC || JUCE_LINUX
#include <sys/stat.h>
#endif

#include <cmath>
#include <algorithm>
#include <array>
#include <memory>

namespace studio
{
namespace
{
bool writeAudio(const juce::AudioBuffer<float>& audio,
                const MasteringAlbum& album,
                const juce::File& destination,
                const MasteringExportSettings& settings,
                juce::String& error)
{
    juce::StringPairArray metadata;
    metadata.set("title", album.title);
    metadata.set("artist", album.artist);
    metadata.set("album", album.title);
    metadata.set("genre", album.genre);
    metadata.set("date", album.releaseDate);
    metadata.set("copyright", album.copyright);
    const auto result = AudioExport::write(audio, destination, settings, metadata);
    if (result.failed())
    {
        error = result.getErrorMessage();
        return false;
    }
    return true;
}

std::optional<MasteringMeasurement> analyseFile(
    const juce::File& file,
    juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(
        formats.createReaderFor(file));
    if (reader == nullptr
        || reader->lengthInSamples <= 0
        || reader->lengthInSamples
            > std::numeric_limits<int>::max())
    {
        error = "Could not decode the completed master for verification.";
        return std::nullopt;
    }
    juce::AudioBuffer<float> audio(
        static_cast<int>(juce::jlimit(
            static_cast<unsigned int>(1),
            static_cast<unsigned int>(2),
            reader->numChannels)),
        static_cast<int>(reader->lengthInSamples));
    if (!reader->read(
            &audio,
            0,
            audio.getNumSamples(),
            0,
            true,
            audio.getNumChannels() > 1))
    {
        error = "Could not read the completed master for verification.";
        return std::nullopt;
    }
    return MasteringEngine::analyse(audio, reader->sampleRate);
}

juce::String signingPayload(const RenderReport& report)
{
    auto value = report.toVar();
    if (auto* object = value.getDynamicObject())
        object->removeProperty("signature");
    return juce::JSON::toString(value, false);
}

struct SigningKeys
{
    juce::RSAKey publicKey;
    juce::RSAKey privateKey;
};

std::optional<SigningKeys> loadOrCreateKeys(
    const juce::File& directory,
    juce::String& error)
{
    if (!directory.createDirectory())
    {
        error = "Could not create render-report signing directory.";
        return std::nullopt;
    }
    const auto keyFile = directory.getChildFile(
        "render-report-key.json");
    if (keyFile.existsAsFile())
    {
        const auto value = juce::JSON::parse(
            keyFile.loadFileAsString());
        const auto* object = value.getDynamicObject();
        if (object == nullptr)
        {
            error = "Render-report signing key is not valid JSON.";
            return std::nullopt;
        }
        SigningKeys keys {
            juce::RSAKey(
                object->getProperty("publicKey").toString()),
            juce::RSAKey(
                object->getProperty("privateKey").toString())
        };
        if (!keys.publicKey.isValid()
            || !keys.privateKey.isValid())
        {
            error = "Render-report signing key is invalid.";
            return std::nullopt;
        }
        return keys;
    }

    SigningKeys keys;
    juce::RSAKey::createKeyPair(
        keys.publicKey,
        keys.privateKey,
        2048);
    if (!keys.publicKey.isValid() || !keys.privateKey.isValid())
    {
        error = "Could not generate a render-report signing key.";
        return std::nullopt;
    }
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("publicKey", keys.publicKey.toString());
    object->setProperty("privateKey", keys.privateKey.toString());
    if (!keyFile.replaceWithText(
            juce::JSON::toString(
                juce::var(object.release()),
                true),
            false,
            false,
            "\n"))
    {
        error = "Could not save the render-report signing key.";
        return std::nullopt;
    }
#if JUCE_MAC || JUCE_LINUX
    chmod(keyFile.getFullPathName().toRawUTF8(), 0600);
#endif
    return keys;
}

bool signReport(RenderReport& report,
                const juce::File& signingDirectory,
                juce::String& error)
{
    auto keys = loadOrCreateKeys(signingDirectory, error);
    if (!keys.has_value())
        return false;
    report.signingPublicKey = keys->publicKey.toString();
    const auto payloadHash = juce::SHA256(
        signingPayload(report).toUTF8());
    juce::BigInteger signature;
    signature.loadFromMemoryBlock(payloadHash.getRawData());
    if (!keys->privateKey.applyToValue(signature))
    {
        error = "Could not sign the render report.";
        return false;
    }
    report.signature = signature.toString(16);
    return true;
}

juce::String sourceHash(const MasteringAlbum& album)
{
    juce::String sourceIdentity;
    for (const auto& track : album.tracks)
        if (const auto* source = track.selectedSource())
            sourceIdentity << track.id << ":" << source->id << ":"
                           << source->sourceHash << "\n";
    return juce::SHA256(sourceIdentity.toUTF8()).toHexString();
}

const MasteringDistributionPreset* findPreset(
    const juce::String& id,
    const std::vector<MasteringDistributionPreset>& presets)
{
    const auto preset = std::find_if(
        presets.cbegin(),
        presets.cend(),
        [&id](const auto& candidate)
        {
            return candidate.id == id;
        });
    return preset != presets.cend() ? &*preset : nullptr;
}

juce::String cueTime(double seconds)
{
    const auto totalFrames = static_cast<int>(
        std::llround(seconds * 75.0));
    const auto minutes = totalFrames / (75 * 60);
    const auto secondsPart = (totalFrames / 75) % 60;
    const auto frames = totalFrames % 75;
    return juce::String(minutes).paddedLeft('0', 2)
        + ":"
        + juce::String(secondsPart).paddedLeft('0', 2)
        + ":"
        + juce::String(frames).paddedLeft('0', 2);
}

juce::String cueText(const juce::String& value)
{
    return value.replaceCharacter('"', '\'');
}

bool validMcn(const juce::String& mcn)
{
    if (mcn.isEmpty())
        return true;
    if (mcn.length() != 13 || !mcn.containsOnly("0123456789"))
        return false;
    auto sum = 0;
    for (int index = 0; index < 12; ++index)
    {
        const auto digit = static_cast<int>(mcn[index] - '0');
        sum += digit * (index % 2 == 0 ? 1 : 3);
    }
    const auto check = (10 - (sum % 10)) % 10;
    return check == static_cast<int>(mcn[12] - '0');
}

bool writeCueSheet(const MasteringAlbum& album,
                   const juce::File& wavFile,
                   const juce::File& cueFile,
                   juce::String& error)
{
    if (!validMcn(album.mcn))
    {
        error = "DDP export requires a valid 13-digit MCN/EAN check digit.";
        return false;
    }
    const auto placements = album.placements();
    juce::String cue;
    cue << "REM GENERATED BY STUDIO DUO\n";
    if (album.mcn.isNotEmpty())
        cue << "CATALOG " << album.mcn << "\n";
    cue << "TITLE \"" << cueText(album.title) << "\"\n";
    cue << "PERFORMER \"" << cueText(album.artist) << "\"\n";
    cue << "FILE \"" << wavFile.getFileName() << "\" WAVE\n";
    for (std::size_t index = 0; index < album.tracks.size(); ++index)
    {
        const auto exactFrame =
            placements[index].startSeconds * 75.0;
        if (std::abs(exactFrame - std::round(exactFrame)) > 0.000001)
        {
            error = "DDP track starts must align to 1/75-second CD sectors.";
            return false;
        }
        const auto& track = album.tracks[index];
        cue << "  TRACK "
            << juce::String(static_cast<int>(index + 1))
                   .paddedLeft('0', 2)
            << " AUDIO\n";
        cue << "    TITLE \"" << cueText(track.title) << "\"\n";
        cue << "    PERFORMER \""
            << cueText(track.artist.isNotEmpty()
                           ? track.artist
                           : album.artist)
            << "\"\n";
        if (track.isrc.isNotEmpty())
            cue << "    ISRC " << track.isrc << "\n";
        cue << "    INDEX 01 "
            << cueTime(placements[index].startSeconds)
            << "\n";
        auto ddpIndex = 2;
        for (const auto relativeMarker : track.indexMarkersSeconds)
        {
            if (relativeMarker <= 0.0)
                continue;
            if (ddpIndex > 99)
            {
                error = "DDP tracks support at most 98 secondary index markers.";
                return false;
            }
            const auto markerTime =
                placements[index].startSeconds
                + relativeMarker;
            const auto markerFrame = markerTime * 75.0;
            if (std::abs(markerFrame - std::round(markerFrame))
                > 0.000001)
            {
                error = "DDP index markers must align to 1/75-second CD sectors.";
                return false;
            }
            cue << "    INDEX "
                << juce::String(ddpIndex++)
                       .paddedLeft('0', 2)
                << " "
                << cueTime(markerTime)
                << "\n";
        }
    }
    if (!cueFile.replaceWithText(cue, false, false, "\n"))
    {
        error = "Could not write the DDP encoder CUE sheet.";
        return false;
    }
    return true;
}

bool validateDdpFileset(const juce::File& directory,
                        juce::String& error)
{
    const auto ddpid = directory.getChildFile("DDPID");
    const auto ddpms = directory.getChildFile("DDPMS");
    const auto pq = directory.getChildFile("PQ_DESCR");
    const auto images = directory.findChildFiles(
        juce::File::findFiles,
        false,
        "*.DAT");
    if (!ddpid.existsAsFile()
        || ddpid.getSize() <= 0
        || !ddpms.existsAsFile()
        || ddpms.getSize() <= 0
        || !pq.existsAsFile()
        || pq.getSize() <= 0
        || images.isEmpty())
    {
        error = "The external DDP encoder returned an incomplete fileset.";
        return false;
    }
    for (const auto& image : images)
    {
        if (image.getSize() <= 0)
        {
            error = "The external DDP encoder returned an empty image file.";
            return false;
        }
    }
    return true;
}

std::optional<juce::String> writeDdpChecksums(
    const juce::File& directory,
    juce::String& error)
{
    auto files = directory.findChildFiles(
        juce::File::findFiles,
        false);
    files.sort();
    juce::String checksums;
    for (const auto& file : files)
    {
        if (file.getFileName() == "CHECKSUMS.sha256")
            continue;
        checksums << juce::SHA256(file).toHexString()
                  << "  "
                  << file.getFileName()
                  << "\n";
    }
    const auto checksumFile =
        directory.getChildFile("CHECKSUMS.sha256");
    if (checksums.isEmpty()
        || !checksumFile.replaceWithText(
            checksums,
            false,
            false,
            "\n"))
    {
        error = "Could not write DDP transfer checksums.";
        return std::nullopt;
    }
    return juce::SHA256(checksumFile).toHexString();
}
}

juce::var MasteringExportSettings::toVar() const
{
    auto value = AudioExportSettings::toVar();
    value.getDynamicObject()->setProperty("distributionPresetId", distributionPresetId);
    return value;
}

std::vector<MasteringDistributionPreset>
MasteringReleaseService::distributionPresets()
{
    return {
        { "streaming-balanced",
          "Streaming balanced",
          -14.0,
          -1.0 },
        { "apple-digital-master",
          "Apple Digital Masters reference",
          -16.0,
          -1.0 },
        { "ebu-r128",
          "EBU R128 broadcast",
          -23.0,
          -1.0 },
        { "cd",
          "Audio CD",
          std::nullopt,
          std::nullopt }
    };
}

std::optional<RenderReport> MasteringReleaseService::exportMaster(
    const MasteringAlbum& album,
    const juce::File& destination,
    const MasteringExportSettings& settings,
    const juce::File& signingDirectory,
    juce::String& error)
{
    if (const auto validation = AudioExport::validate(settings); validation.failed())
    {
        error = validation.getErrorMessage();
        return std::nullopt;
    }
    auto rendered = MasteringEngine::renderAlbum(
        album,
        settings.sampleRate,
        error);
    if (!rendered.has_value())
        return std::nullopt;

    const auto settingsText = juce::JSON::toString(
        settings.toVar(),
        false);
    juce::TemporaryFile stagedMaster(destination);
    const auto stagedMasterFile = stagedMaster.getFile();
    if (!writeAudio(
            rendered->audio,
            album,
            stagedMasterFile,
            settings,
            error))
        return std::nullopt;
    rendered->audio.setSize(0, 0);
    rendered.reset();

    auto measurement = analyseFile(stagedMasterFile, error);
    if (!measurement.has_value())
        return std::nullopt;
    RenderReport report;
    report.scope = "mastering-album";
    report.outputFile = destination.getFullPathName();
    report.sourceHash = sourceHash(album);
    report.chainHash = juce::SHA256(
        juce::JSON::toString(album.toVar(), false).toUTF8())
                           .toHexString();
    report.outputHash =
        juce::SHA256(stagedMasterFile).toHexString();
    report.settingsHash =
        juce::SHA256(settingsText.toUTF8()).toHexString();
    report.mode = "offline";
    report.status = "success";
    report.durationSeconds = measurement->durationSeconds;
    report.createdAt =
        juce::Time::getCurrentTime().toISO8601(true);
    report.format = AudioExport::extension(settings.format);
    report.sampleRate = settings.sampleRate;
    report.bitDepth = settings.format == AudioExportFormat::mp3
            || settings.format == AudioExportFormat::oggVorbis
        ? 0
        : settings.bitDepth;
    report.integratedLoudnessLufs =
        measurement->integratedLoudnessLufs;
    report.loudnessRangeLu = measurement->loudnessRangeLu;
    report.truePeakDbtp = measurement->truePeakDbtp;
    report.samplePeakDbfs = measurement->samplePeakDbfs;
    report.correlation = measurement->correlation;

    const auto presets = distributionPresets();
    if (const auto* preset = findPreset(
            settings.distributionPresetId,
            presets))
    {
        if (preset->targetLoudnessLufs.has_value()
            && report.integratedLoudnessLufs.has_value())
        {
            report.warning << preset->name
                           << " target "
                           << juce::String(
                                  *preset->targetLoudnessLufs,
                                  1)
                           << " LUFS; measured "
                           << juce::String(
                                  *report.integratedLoudnessLufs,
                                  1)
                           << " LUFS. "
                           << (settings.normalizePeak
                                   ? "Sample-peak normalization was applied, not LUFS normalization."
                                   : "No loudness normalization was applied.");
        }
        if (preset->maximumTruePeakDbtp.has_value()
            && report.truePeakDbtp
                > *preset->maximumTruePeakDbtp)
        {
            if (report.warning.isNotEmpty())
                report.warning << " ";
            report.warning << "True peak exceeds the preset ceiling of "
                           << juce::String(
                                  *preset->maximumTruePeakDbtp,
                                  1)
                           << " dBTP.";
        }
    }
    if (!signReport(report, signingDirectory, error))
        return std::nullopt;
    const auto reportFile = destination.getSiblingFile(
        destination.getFileNameWithoutExtension()
        + ".report.json");
    juce::TemporaryFile stagedReport(reportFile);
    if (!stagedReport.getFile().replaceWithText(
            juce::JSON::toString(report.toVar(), true),
            false,
            false,
            "\n"))
    {
        error = "Could not write the signed render report.";
        return std::nullopt;
    }
    const auto hadPreviousMaster = destination.existsAsFile();
    const auto previousMaster = destination.getSiblingFile(
        destination.getFileName()
        + ".backup-"
        + juce::Uuid().toString());
    if (hadPreviousMaster
        && !destination.copyFileTo(previousMaster))
    {
        error = "Could not preserve the previous master before publishing.";
        return std::nullopt;
    }
    if (!stagedMaster.overwriteTargetFileWithTemporary())
    {
        previousMaster.deleteFile();
        error = "Could not publish the completed mastering export.";
        return std::nullopt;
    }
    if (!stagedReport.overwriteTargetFileWithTemporary())
    {
        auto restored = false;
        if (hadPreviousMaster)
        {
            juce::TemporaryFile restore(destination);
            restored = previousMaster.copyFileTo(restore.getFile())
                && restore.overwriteTargetFileWithTemporary();
        }
        else
        {
            restored = destination.deleteFile();
        }
        if (restored)
            previousMaster.deleteFile();
        error = restored
            ? "The signed report could not be published; the previous master was restored."
            : "The signed report could not be published, and the previous master could not be restored. Backup retained at "
                + previousMaster.getFullPathName();
        return std::nullopt;
    }
    previousMaster.deleteFile();
    return report;
}

std::optional<RenderReport> MasteringReleaseService::exportDdp(
    const MasteringAlbum& album,
    const juce::File& destinationDirectory,
    const juce::File& encoderExecutable,
    const juce::File& signingDirectory,
    juce::String& error)
{
    if (!encoderExecutable.existsAsFile())
    {
        error = "Select an installed licensed DDP encoder executable.";
        return std::nullopt;
    }
    if (destinationDirectory.exists()
        && destinationDirectory.getNumberOfChildFiles(
               juce::File::findFilesAndDirectories)
            > 0)
    {
        error = "DDP export destination must be empty.";
        return std::nullopt;
    }
    if (!destinationDirectory.getParentDirectory().createDirectory())
    {
        error = "Could not create the DDP destination parent directory.";
        return std::nullopt;
    }
    const auto stagingDirectory =
        destinationDirectory.getParentDirectory()
            .getNonexistentChildFile(
                destinationDirectory.getFileNameWithoutExtension()
                    + "-staging",
                ".ddp",
                false);
    if (!stagingDirectory.createDirectory())
    {
        error = "Could not create the DDP staging directory.";
        return std::nullopt;
    }
    const auto cleanup = [&stagingDirectory]
    {
        stagingDirectory.deleteRecursively();
    };
    const auto wavFile = stagingDirectory.getChildFile("album.wav");
    const auto cueFile = stagingDirectory.getChildFile("album.cue");
    const auto encodedDirectory =
        stagingDirectory.getChildFile("encoded");
    MasteringExportSettings settings;
    settings.format = MasteringExportFormat::wav;
    settings.sampleRate = 44100.0;
    settings.bitDepth = 16;
    settings.dither = MasteringDither::tpdf;
    settings.distributionPresetId = "cd";
    auto baseReport = exportMaster(
        album,
        wavFile,
        settings,
        signingDirectory,
        error);
    if (!baseReport.has_value()
        || !writeCueSheet(album, wavFile, cueFile, error))
    {
        cleanup();
        return std::nullopt;
    }

    juce::ChildProcess encoder;
    if (!encoder.start(
            juce::StringArray {
                encoderExecutable.getFullPathName(),
                "--input-wav",
                wavFile.getFullPathName(),
                "--cue",
                cueFile.getFullPathName(),
                "--output",
                encodedDirectory.getFullPathName()
            }))
    {
        cleanup();
        error = "Could not launch the external DDP encoder.";
        return std::nullopt;
    }
    juce::String output;
    const auto startedAt =
        juce::Time::getMillisecondCounterHiRes();
    std::array<char, 4096> outputBuffer {};
    while (encoder.isRunning()
           && juce::Time::getMillisecondCounterHiRes() - startedAt
               < 120000.0)
    {
        const auto bytes = encoder.readProcessOutput(
            outputBuffer.data(),
            static_cast<int>(outputBuffer.size()));
        if (bytes > 0)
            output += juce::String::fromUTF8(
                outputBuffer.data(),
                bytes);
        else
            juce::Thread::sleep(10);
    }
    if (encoder.isRunning())
    {
        encoder.kill();
        cleanup();
        error = "The external DDP encoder timed out.";
        return std::nullopt;
    }
    output += encoder.readAllProcessOutput();
    output = output.trim();
    if (encoder.getExitCode() != 0)
    {
        cleanup();
        error = "The external DDP encoder failed"
            + (output.isNotEmpty() ? ": " + output : juce::String("."));
        return std::nullopt;
    }
    if (!validateDdpFileset(encodedDirectory, error))
    {
        cleanup();
        return std::nullopt;
    }
    const auto filesetHash = writeDdpChecksums(
        encodedDirectory,
        error);
    if (!filesetHash.has_value())
    {
        cleanup();
        return std::nullopt;
    }

    auto report = std::move(*baseReport);
    report.id = juce::Uuid().toString();
    report.scope = "mastering-ddp";
    report.outputFile =
        destinationDirectory.getFullPathName();
    report.outputHash = *filesetHash;
    report.format = "ddp";
    report.warning =
        "DDP files were produced by the selected external encoder. "
        "Studio Duo validated required files and transfer hashes; "
        "plant acceptance still requires a licensed independent validator.";
    report.signingPublicKey.clear();
    report.signature.clear();
    if (!signReport(report, signingDirectory, error))
    {
        cleanup();
        return std::nullopt;
    }
    if (!encodedDirectory.getChildFile("render-report.json")
             .replaceWithText(
                 juce::JSON::toString(report.toVar(), true),
                 false,
                 false,
                 "\n"))
    {
        cleanup();
        error = "Could not write the DDP render report.";
        return std::nullopt;
    }
    if (destinationDirectory.exists()
        && (destinationDirectory.getNumberOfChildFiles(
                juce::File::findFilesAndDirectories)
                != 0
            || !destinationDirectory.deleteFile()))
    {
        cleanup();
        error = "The DDP destination changed during export and was not replaced.";
        return std::nullopt;
    }
    if (!encodedDirectory.moveFileTo(destinationDirectory))
    {
        cleanup();
        error = "Could not publish the validated DDP fileset.";
        return std::nullopt;
    }
    cleanup();
    return report;
}

bool MasteringReleaseService::verifyReport(
    const RenderReport& report,
    const juce::File& trustedSigningDirectory,
    juce::String& error)
{
    const auto keyFile = trustedSigningDirectory.getChildFile(
        "render-report-key.json");
    const auto keyValue = juce::JSON::parse(
        keyFile.loadFileAsString());
    const auto* keyObject = keyValue.getDynamicObject();
    if (!keyFile.existsAsFile() || keyObject == nullptr)
    {
        error = "The trusted render-report public key is unavailable.";
        return false;
    }
    juce::RSAKey publicKey(
        keyObject->getProperty("publicKey").toString());
    juce::BigInteger signature;
    signature.parseString(report.signature, 16);
    if (!publicKey.isValid()
        || report.signingPublicKey != publicKey.toString()
        || signature.isZero()
        || !publicKey.applyToValue(signature))
    {
        error = "The render-report signature is invalid.";
        return false;
    }
    juce::BigInteger expected;
    expected.loadFromMemoryBlock(
        juce::SHA256(signingPayload(report).toUTF8()).getRawData());
    if (signature != expected)
    {
        error = "The render report has changed since it was signed.";
        return false;
    }
    return true;
}
}

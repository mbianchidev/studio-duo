#include "ProjectArchiveReader.h"

#include "ZipUtilities.h"

#include <algorithm>
#include <array>

namespace studio
{
using project_archive::normalizedArchivePath;
using project_archive::readLittleEndian16;
using project_archive::readLittleEndian32;

ProjectArchiveReader::ProjectArchiveReader(
    const juce::File& archive,
    juce::String formatLabel)
    : source(archive), label(std::move(formatLabel)), zip(archive)
{
}

juce::Result ProjectArchiveReader::validate(
    const juce::StringArray& requiredEntries,
    bool allowDirectoryEntries)
{
    paths.clear();
    if (!source.existsAsFile())
        return juce::Result::fail("The selected " + label + " archive does not exist.");
    if (zip.getNumEntries() <= 0)
        return juce::Result::fail("The selected file is not a readable ZIP archive.");
    if (const auto central = parseCentralDirectory(allowDirectoryEntries);
        central.failed())
        return central;
    for (const auto& path : requiredEntries)
        if (!has(path))
            return juce::Result::fail(
                "The " + label + " archive must contain "
                + requiredEntries.joinIntoString(" and ")
                + " at its expected locations.");
    return juce::Result::ok();
}

bool ProjectArchiveReader::has(const juce::String& path) const
{
    return paths.find(normalizedArchivePath(path)) != paths.cend();
}

std::optional<juce::String> ProjectArchiveReader::readText(
    const juce::String& path,
    juce::String& error,
    juce::int64 maximumBytes)
{
    juce::MemoryBlock data;
    if (!readData(path, data, error, maximumBytes))
        return std::nullopt;
    return juce::String::fromUTF8(
        static_cast<const char*>(data.getData()),
        static_cast<int>(data.getSize()));
}

bool ProjectArchiveReader::readData(
    const juce::String& path,
    juce::MemoryBlock& data,
    juce::String& error,
    juce::int64 maximumBytes)
{
    const auto normalized = normalizedArchivePath(path);
    const auto found = paths.find(normalized);
    if (found == paths.cend())
    {
        error = "Archive entry is missing: " + normalized;
        return false;
    }
    if (found->second.uncompressedSize > maximumBytes)
    {
        error = "Archive entry exceeds the supported size: " + normalized;
        return false;
    }
    data.reset();
    return readEntry(
        normalized, found->second, error,
        [&data](const void* bytes, std::size_t size)
        {
            data.append(bytes, size);
            return true;
        });
}

bool ProjectArchiveReader::copyTo(
    const juce::String& path,
    const juce::File& destination,
    juce::String& error)
{
    const auto normalized = normalizedArchivePath(path);
    const auto found = paths.find(normalized);
    if (found == paths.cend())
    {
        error = "Archive entry is missing: " + normalized;
        return false;
    }
    if (!destination.getParentDirectory().createDirectory())
    {
        error = "Could not prepare imported media: " + destination.getFullPathName();
        return false;
    }
    const auto temporary = destination.getSiblingFile(
        destination.getFileName() + ".tmp-" + juce::Uuid().toString());
    {
        auto output = temporary.createOutputStream();
        if (output == nullptr)
        {
            error = "Could not create imported media: " + temporary.getFullPathName();
            return false;
        }
        if (!readEntry(
                normalized, found->second, error,
                [&output](const void* bytes, std::size_t size)
                {
                    return output->write(bytes, size);
                }))
        {
            output.reset();
            if (!temporary.deleteFile())
                error += " Could not remove temporary media: " + temporary.getFullPathName();
            return false;
        }
        output->flush();
        if (output->getStatus().failed())
        {
            error = output->getStatus().getErrorMessage();
            output.reset();
            if (!temporary.deleteFile())
                error += " Could not remove temporary media: " + temporary.getFullPathName();
            return false;
        }
    }
    const auto published = destination.existsAsFile()
        ? temporary.replaceFileIn(destination)
        : temporary.moveFileTo(destination);
    if (!published)
    {
        error = "Could not publish imported media: " + destination.getFullPathName();
        if (!temporary.deleteFile())
            error += " Could not remove temporary media: " + temporary.getFullPathName();
        return false;
    }
    return true;
}

juce::Result ProjectArchiveReader::parseCentralDirectory(bool allowDirectoryEntries)
{
    const auto fileSize = source.getSize();
    if (fileSize < 22 || fileSize > maximumArchiveBytes)
        return juce::Result::fail("The " + label + " archive exceeds the supported classic ZIP size.");
    auto input = source.createInputStream();
    if (input == nullptr)
        return juce::Result::fail("Could not read the " + label + " archive.");
    const auto tailSize = static_cast<std::size_t>(
        std::min<juce::int64>(fileSize, 1024 * 1024));
    juce::MemoryBlock tail(tailSize, true);
    if (!input->setPosition(fileSize - static_cast<juce::int64>(tailSize))
        || input->read(tail.getData(), static_cast<int>(tailSize))
            != static_cast<int>(tailSize))
        return juce::Result::fail("Could not read the ZIP central directory.");
    const auto* tailBytes = static_cast<const std::uint8_t*>(tail.getData());
    std::optional<std::size_t> endOffset;
    for (auto offset = tailSize - 22;; --offset)
    {
        if (readLittleEndian32(tailBytes + offset) == 0x06054b50u)
        {
            endOffset = offset;
            break;
        }
        if (offset == 0)
            break;
    }
    if (!endOffset.has_value())
        return juce::Result::fail("The ZIP end-of-directory record is missing.");
    const auto* end = tailBytes + *endOffset;
    if (readLittleEndian16(end + 4) != 0 || readLittleEndian16(end + 6) != 0)
        return juce::Result::fail("Multi-disk ZIP archives are not supported.");
    const auto entriesOnDisk = readLittleEndian16(end + 8);
    const auto entryCount = readLittleEndian16(end + 10);
    const auto directorySize = readLittleEndian32(end + 12);
    const auto directoryOffset = readLittleEndian32(end + 16);
    if (entriesOnDisk == 0xffffu || entryCount == 0xffffu
        || directorySize == 0xffffffffu || directoryOffset == 0xffffffffu)
        return juce::Result::fail("ZIP64 " + label + " archives are not supported by this build.");
    if (entryCount != entriesOnDisk || entryCount == 0 || entryCount > 10000
        || static_cast<juce::int64>(directoryOffset) + directorySize > fileSize
        || directorySize > 64 * 1024 * 1024)
        return juce::Result::fail("The ZIP central directory is invalid or too large.");
    juce::MemoryBlock directory(directorySize, true);
    if (!input->setPosition(directoryOffset)
        || input->read(directory.getData(), static_cast<int>(directorySize))
            != static_cast<int>(directorySize))
        return juce::Result::fail("The ZIP central directory is truncated.");
    const auto* bytes = static_cast<const std::uint8_t*>(directory.getData());
    std::size_t position = 0;
    juce::int64 totalSize = 0;
    for (auto index = 0; index < entryCount; ++index)
    {
        if (position + 46 > directorySize
            || readLittleEndian32(bytes + position) != 0x02014b50u)
            return juce::Result::fail("The ZIP central directory contains a malformed entry.");
        const auto flags = readLittleEndian16(bytes + position + 8);
        const auto method = readLittleEndian16(bytes + position + 10);
        const auto crc = readLittleEndian32(bytes + position + 16);
        const auto compressedSize = readLittleEndian32(bytes + position + 20);
        const auto uncompressedSize = readLittleEndian32(bytes + position + 24);
        const auto nameLength = readLittleEndian16(bytes + position + 28);
        const auto extraLength = readLittleEndian16(bytes + position + 30);
        const auto commentLength = readLittleEndian16(bytes + position + 32);
        const auto localOffset = readLittleEndian32(bytes + position + 42);
        const auto entrySize = static_cast<std::size_t>(46)
            + nameLength + extraLength + commentLength;
        if (position + entrySize > directorySize || nameLength == 0
            || (flags & 1u) != 0 || (method != 0 && method != 8)
            || localOffset >= static_cast<std::uint64_t>(fileSize)
            || compressedSize > maximumArchiveBytes
            || uncompressedSize > maximumArchiveBytes
            || totalSize > maximumArchiveBytes - uncompressedSize)
            return juce::Result::fail("The ZIP entry is encrypted, unsupported, oversized, or malformed.");
        const auto path = normalizedArchivePath(
            juce::String::fromUTF8(
                reinterpret_cast<const char*>(bytes + position + 46), nameLength));
        if (!project_archive::safeArchivePath(path, allowDirectoryEntries))
            return juce::Result::fail("The " + label + " archive contains an unsafe path: " + path);
        const auto* juceEntry = zip.getEntry(index);
        if (juceEntry == nullptr || juceEntry->isSymbolicLink
            || normalizedArchivePath(juceEntry->filename) != path
            || juceEntry->uncompressedSize != static_cast<juce::int64>(uncompressedSize)
            || (path.endsWithChar('/') && uncompressedSize != 0))
            return juce::Result::fail("The ZIP entry metadata is inconsistent.");
        if (!paths.emplace(path, EntryMetadata { index, static_cast<juce::int64>(uncompressedSize), crc }).second)
            return juce::Result::fail("The " + label + " archive contains a duplicate path: " + path);
        totalSize += uncompressedSize;
        position += entrySize;
    }
    if (zip.getNumEntries() != entryCount)
        return juce::Result::fail("The ZIP entry count is inconsistent.");
    return juce::Result::ok();
}

bool ProjectArchiveReader::readEntry(
    const juce::String& path,
    const EntryMetadata& metadata,
    juce::String& error,
    const std::function<bool(const void*, std::size_t)>& consumer)
{
    std::unique_ptr<juce::InputStream> input(zip.createStreamForEntry(metadata.index));
    if (input == nullptr)
    {
        error = "Could not read archive entry: " + path;
        return false;
    }
    project_archive::Crc32 crc;
    std::array<std::uint8_t, 64 * 1024> buffer {};
    juce::int64 remaining = metadata.uncompressedSize;
    while (remaining > 0)
    {
        const auto requested = static_cast<int>(
            std::min<juce::int64>(remaining, static_cast<juce::int64>(buffer.size())));
        const auto read = input->read(buffer.data(), requested);
        if (read <= 0)
        {
            error = "Archive entry is truncated: " + path;
            return false;
        }
        crc.update(buffer.data(), static_cast<std::size_t>(read));
        if (!consumer(buffer.data(), static_cast<std::size_t>(read)))
        {
            error = "Could not consume archive entry: " + path;
            return false;
        }
        remaining -= read;
    }
    std::uint8_t extra = 0;
    if (input->read(&extra, 1) > 0)
    {
        error = "Archive entry expands beyond its declared size: " + path;
        return false;
    }
    if (crc.result() != metadata.crc)
    {
        error = "Archive entry failed its CRC check: " + path;
        return false;
    }
    return true;
}
}

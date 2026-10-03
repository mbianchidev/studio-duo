#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>

namespace studio::project_archive
{
class Crc32
{
public:
    void update(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index)
            value = table()[(value ^ bytes[index]) & 0xff] ^ (value >> 8);
    }

    [[nodiscard]] std::uint32_t result() const noexcept
    {
        return value ^ 0xffffffffu;
    }

private:
    std::uint32_t value = 0xffffffffu;

    static const std::array<std::uint32_t, 256>& table()
    {
        static const auto values = []
        {
            std::array<std::uint32_t, 256> result {};
            for (std::uint32_t index = 0; index < result.size(); ++index)
            {
                auto crc = index;
                for (auto bit = 0; bit < 8; ++bit)
                    crc = (crc & 1u) != 0
                        ? 0xedb88320u ^ (crc >> 1)
                        : crc >> 1;
                result[index] = crc;
            }
            return result;
        }();
        return values;
    }
};

inline std::uint16_t readLittleEndian16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(
        bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

inline std::uint32_t readLittleEndian32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(
        bytes[0]
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24));
}

inline juce::String normalizedArchivePath(juce::String path)
{
    path = path.replaceCharacter('\\', '/');
    while (path.startsWith("./"))
        path = path.substring(2);
    return path;
}

inline bool safeArchivePath(const juce::String& original,
                            bool allowDirectoryEntries = false)
{
    auto path = normalizedArchivePath(original);
    if (allowDirectoryEntries && path.endsWithChar('/'))
        path = path.dropLastCharacters(1);
    if (path.isEmpty() || path.startsWithChar('/')
        || juce::File::isAbsolutePath(path) || path.containsChar(':'))
        return false;
    const auto segments = juce::StringArray::fromTokens(path, "/", {});
    if (segments.isEmpty())
        return false;
    for (const auto& segment : segments)
        if (segment.isEmpty() || segment == "." || segment == "..")
            return false;
    return true;
}
}

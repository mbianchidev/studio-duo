#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <optional>

namespace studio
{
enum class UpdatePlatform
{
    macOS,
    windows,
    unsupported
};

struct UpdateAsset
{
    juce::String url;
    juce::String fileName;
    juce::String sha256;
    std::int64_t sizeBytes = 0;
};

struct UpdateRelease
{
    juce::String version;
    juce::String publishedAt;
    juce::String releaseNotesUrl;
    UpdateAsset asset;
};

[[nodiscard]] UpdatePlatform currentUpdatePlatform();
[[nodiscard]] juce::String updatePlatformName(UpdatePlatform platform);
[[nodiscard]] juce::String expectedUpdateAssetFileName(
    UpdatePlatform platform,
    const juce::String& version);
[[nodiscard]] std::optional<std::array<int, 3>>
    parseSemanticVersion(const juce::String& version);
[[nodiscard]] bool isNewerSemanticVersion(
    const juce::String& candidate,
    const juce::String& current);
[[nodiscard]] bool isTrustedStudioDuoReleaseUrl(
    const juce::String& url);
[[nodiscard]] std::optional<UpdateRelease> parseUpdateManifest(
    const juce::String& json,
    UpdatePlatform platform,
    juce::String& error);
}

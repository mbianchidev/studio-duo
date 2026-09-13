#include "UpdateManifest.h"

#include <limits>

namespace studio
{
namespace
{
constexpr std::int64_t maximumUpdatePackageBytes =
    static_cast<std::int64_t>(2) * 1024 * 1024 * 1024;

bool numericValue(const juce::var& value)
{
    return value.isInt() || value.isInt64() || value.isDouble();
}

bool validSha256(const juce::String& value)
{
    return value.length() == 64
        && value.containsOnly("0123456789abcdefABCDEF");
}

std::optional<int> parseVersionPart(const juce::String& part)
{
    if (part.isEmpty()
        || !part.containsOnly("0123456789")
        || (part.length() > 1 && part.startsWithChar('0')))
        return std::nullopt;

    const auto value = part.getLargeIntValue();
    if (value < 0
        || value > static_cast<juce::int64>(
            std::numeric_limits<int>::max()))
        return std::nullopt;
    return static_cast<int>(value);
}

bool validReleaseNotesUrl(
    const juce::String& urlText,
    const juce::String& version)
{
    if (urlText.isEmpty())
        return true;

    const juce::URL url(urlText);
    return url.isWellFormed()
        && url.getScheme().equalsIgnoreCase("https")
        && url.getDomain().equalsIgnoreCase("github.com")
        && url.getSubPath(false)
            == "mbianchidev/studio-duo/releases/tag/v" + version;
}
}

UpdatePlatform currentUpdatePlatform()
{
#if JUCE_MAC
    return UpdatePlatform::macOS;
#elif JUCE_WINDOWS
    return UpdatePlatform::windows;
#else
    return UpdatePlatform::unsupported;
#endif
}

juce::String updatePlatformName(UpdatePlatform platform)
{
    switch (platform)
    {
        case UpdatePlatform::macOS:
            return "macOS";
        case UpdatePlatform::windows:
            return "Windows";
        case UpdatePlatform::unsupported:
            return "unsupported";
    }
    return "unsupported";
}

juce::String expectedUpdateAssetFileName(
    UpdatePlatform platform,
    const juce::String& version)
{
    switch (platform)
    {
        case UpdatePlatform::macOS:
            return "Studio-Duo-" + version + "-macOS-universal.zip";
        case UpdatePlatform::windows:
            return "Studio-Duo-" + version + "-Windows-x64-Setup.exe";
        case UpdatePlatform::unsupported:
            return {};
    }
    return {};
}

std::optional<std::array<int, 3>> parseSemanticVersion(
    const juce::String& version)
{
    const auto firstDot = version.indexOfChar('.');
    const auto secondDot = version.indexOfChar(firstDot + 1, '.');
    if (firstDot <= 0
        || secondDot <= firstDot + 1
        || secondDot >= version.length() - 1
        || version.indexOfChar(secondDot + 1, '.') >= 0)
        return std::nullopt;

    const auto major = parseVersionPart(
        version.substring(0, firstDot));
    const auto minor = parseVersionPart(
        version.substring(firstDot + 1, secondDot));
    const auto patch = parseVersionPart(
        version.substring(secondDot + 1));
    if (!major.has_value()
        || !minor.has_value()
        || !patch.has_value())
        return std::nullopt;

    return std::array<int, 3> {
        *major,
        *minor,
        *patch
    };
}

bool isNewerSemanticVersion(
    const juce::String& candidate,
    const juce::String& current)
{
    const auto candidateParts = parseSemanticVersion(candidate);
    const auto currentParts = parseSemanticVersion(current);
    return candidateParts.has_value()
        && currentParts.has_value()
        && *candidateParts > *currentParts;
}

bool isTrustedStudioDuoReleaseUrl(const juce::String& urlText)
{
    const juce::URL url(urlText);
    return url.isWellFormed()
        && url.getScheme().equalsIgnoreCase("https")
        && url.getDomain().equalsIgnoreCase("github.com")
        && url.getSubPath(false).startsWith(
            "mbianchidev/studio-duo/releases/download/v");
}

std::optional<UpdateRelease> parseUpdateManifest(
    const juce::String& json,
    UpdatePlatform platform,
    juce::String& error)
{
    error.clear();
    if (platform == UpdatePlatform::unsupported)
    {
        error = "Automatic updates are not available on this platform.";
        return std::nullopt;
    }

    const auto rootValue = juce::JSON::parse(json);
    const auto* root = rootValue.getDynamicObject();
    if (root == nullptr
        || !numericValue(root->getProperty("schemaVersion"))
        || static_cast<int>(root->getProperty("schemaVersion")) != 1)
    {
        error = "The update manifest schema is missing or unsupported.";
        return std::nullopt;
    }

    UpdateRelease release;
    release.version = root->getProperty("version").toString().trim();
    release.publishedAt =
        root->getProperty("publishedAt").toString().trim();
    release.releaseNotesUrl =
        root->getProperty("releaseNotesUrl").toString().trim();
    if (!parseSemanticVersion(release.version).has_value())
    {
        error = "The update manifest contains an invalid version.";
        return std::nullopt;
    }
    if (!validReleaseNotesUrl(
            release.releaseNotesUrl,
            release.version))
    {
        error = "The update manifest contains an untrusted release-notes URL.";
        return std::nullopt;
    }

    const auto* platforms =
        root->getProperty("platforms").getDynamicObject();
    const auto platformKey =
        platform == UpdatePlatform::macOS ? "macos" : "windows";
    const auto* assetObject = platforms != nullptr
        ? platforms->getProperty(platformKey).getDynamicObject()
        : nullptr;
    if (assetObject == nullptr)
    {
        error = "The update manifest does not contain a "
            + updatePlatformName(platform)
            + " package.";
        return std::nullopt;
    }

    release.asset.url =
        assetObject->getProperty("url").toString().trim();
    release.asset.fileName =
        assetObject->getProperty("fileName").toString().trim();
    release.asset.sha256 =
        assetObject->getProperty("sha256").toString().trim().toLowerCase();
    const auto sizeValue = assetObject->getProperty("sizeBytes");
    if (!numericValue(sizeValue))
    {
        error = "The update package size is missing.";
        return std::nullopt;
    }
    release.asset.sizeBytes = static_cast<juce::int64>(sizeValue);

    const auto expectedFileName =
        expectedUpdateAssetFileName(platform, release.version);
    const juce::URL assetUrl(release.asset.url);
    const auto expectedPathPrefix =
        "mbianchidev/studio-duo/releases/download/v"
        + release.version
        + "/";
    if (!isTrustedStudioDuoReleaseUrl(release.asset.url)
        || !assetUrl.getSubPath(false).startsWith(expectedPathPrefix))
    {
        error = "The update manifest contains an untrusted package URL.";
        return std::nullopt;
    }
    if (release.asset.fileName != expectedFileName
        || assetUrl.getFileName() != expectedFileName)
    {
        error = "The update manifest contains an unexpected package name.";
        return std::nullopt;
    }
    if (!validSha256(release.asset.sha256))
    {
        error = "The update manifest contains an invalid SHA-256 checksum.";
        return std::nullopt;
    }
    if (release.asset.sizeBytes <= 0
        || release.asset.sizeBytes > maximumUpdatePackageBytes)
    {
        error = "The update manifest contains an invalid package size.";
        return std::nullopt;
    }

    return release;
}
}

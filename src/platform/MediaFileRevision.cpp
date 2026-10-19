#include "MediaFileRevision.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#if JUCE_MAC
#include <sys/mount.h>
#endif
#endif

namespace studio
{
std::optional<MediaFileRevision> readMediaFileRevision(
    const juce::File& file, juce::String& error)
{
#if JUCE_WINDOWS
    auto handle = CreateFileW(file.getFullPathName().toWideCharPointer(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto stableReader = handle != INVALID_HANDLE_VALUE;
    if (!stableReader && GetLastError() == ERROR_SHARING_VIOLATION)
        handle = CreateFileW(file.getFullPathName().toWideCharPointer(),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const auto openError = GetLastError();
        error = "Cannot inspect media revision: " + file.getFileName()
            + " (Windows error " + juce::String(openError) + ").";
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION identity {};
    FILE_BASIC_INFO times {};
    const auto inspected = GetFileInformationByHandle(handle, &identity)
        && GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof(times));
    const auto inspectionError = inspected ? ERROR_SUCCESS : GetLastError();
    MediaFileRevision revision;
    if (inspected)
    {
        const auto ticks = [](const FILETIME& value)
        {
            return (static_cast<juce::uint64>(value.dwHighDateTime) << 32)
                | value.dwLowDateTime;
        };
        revision.key << juce::String(identity.dwVolumeSerialNumber) << ":"
                     << juce::String(identity.nFileIndexHigh) << ":"
                     << juce::String(identity.nFileIndexLow) << ":"
                     << juce::String(identity.nFileSizeHigh) << ":"
                     << juce::String(identity.nFileSizeLow) << ":"
                     << juce::String(ticks(identity.ftLastWriteTime)) << ":"
                     << juce::String(times.ChangeTime.QuadPart);

        std::array<wchar_t, MAX_PATH> root {};
        std::array<wchar_t, 32> filesystem {};
        const auto localVolume = stableReader && GetVolumePathNameW(file.getFullPathName().toWideCharPointer(),
            root.data(), static_cast<DWORD>(root.size()))
            && GetDriveTypeW(root.data()) != DRIVE_REMOTE
            && GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr,
                filesystem.data(), static_cast<DWORD>(filesystem.size()))
            && juce::String(filesystem.data()) == "NTFS";
        std::array<std::byte, 4096> journal {};
        DWORD returned = 0;
        if (localVolume && DeviceIoControl(handle, FSCTL_READ_FILE_USN_DATA,
                nullptr, 0, journal.data(), static_cast<DWORD>(journal.size()), &returned, nullptr)
            && returned >= sizeof(USN_RECORD_V2))
        {
            USN_RECORD_V2 record {};
            std::memcpy(&record, journal.data(), sizeof(record));
            if (record.MajorVersion == 2 && record.Usn > 0 && times.ChangeTime.QuadPart > 0)
            {
                revision.key << ":" << juce::String(record.Usn);
                revision.reusable = true;
            }
        }
    }
    if (!CloseHandle(handle))
    {
        error = "Cannot close the media revision handle: " + file.getFileName()
            + " (Windows error " + juce::String(GetLastError()) + ").";
        return std::nullopt;
    }
    if (!inspected)
    {
        error = "Cannot read media identity/change time: " + file.getFileName()
            + " (Windows error " + juce::String(inspectionError) + ").";
        return std::nullopt;
    }
    return revision;
#else
    struct stat info {};
    if (stat(file.getFullPathName().toRawUTF8(), &info) != 0)
    {
        error = "Cannot inspect media revision: " + file.getFileName()
            + " (" + juce::String(std::strerror(errno)) + ").";
        return std::nullopt;
    }
    MediaFileRevision revision;
    revision.key << juce::String(static_cast<juce::uint64>(info.st_dev)) << ":"
                 << juce::String(static_cast<juce::uint64>(info.st_ino)) << ":"
                 << juce::String(static_cast<juce::int64>(info.st_size));
#if JUCE_MAC
    revision.key << ":" << juce::String(static_cast<juce::int64>(info.st_mtimespec.tv_sec))
                 << ":" << juce::String(static_cast<juce::int64>(info.st_mtimespec.tv_nsec))
                 << ":" << juce::String(static_cast<juce::int64>(info.st_ctimespec.tv_sec))
                 << ":" << juce::String(static_cast<juce::int64>(info.st_ctimespec.tv_nsec));
    struct statfs filesystem {};
    if (statfs(file.getFullPathName().toRawUTF8(), &filesystem) != 0)
    {
        error = "Cannot inspect the media filesystem: " + file.getFileName()
            + " (" + juce::String(std::strerror(errno)) + ").";
        return std::nullopt;
    }
    revision.reusable = (filesystem.f_flags & MNT_LOCAL) != 0
        && juce::String(filesystem.f_fstypename) == "apfs"
        && info.st_ctimespec.tv_nsec != 0;
#else
    revision.key << ":" << juce::String(static_cast<juce::int64>(info.st_mtime))
                 << ":" << juce::String(static_cast<juce::int64>(info.st_ctime));
#endif
    return revision;
#endif
}
}

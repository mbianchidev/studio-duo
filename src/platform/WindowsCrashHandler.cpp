#include "WindowsCrashHandler.h"

#include "logging/StudioLogger.h"

#include <windows.h>
#include <strsafe.h>

#include <atomic>
#include <cwchar>
#include <string>

namespace studio
{
struct WindowsCrashHandler::State
{
    std::wstring reportPath;
    std::wstring version;
    std::atomic<bool> showDialog { false };
    LONG handlingCrash = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
    inline static std::atomic<State*> active { nullptr };

    static LONG WINAPI handleCrash(EXCEPTION_POINTERS* exception)
    {
        auto* current = active.load(std::memory_order_acquire);
        if (current == nullptr
            || InterlockedCompareExchange(&current->handlingCrash, 1, 0) != 0)
        {
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // Use fixed buffers and bypass JUCE/logger locks on the faulting thread.
        const auto* record = exception != nullptr ? exception->ExceptionRecord : nullptr;
        const auto code = record != nullptr ? record->ExceptionCode : 0;
        const auto address = record != nullptr ? record->ExceptionAddress : nullptr;
        wchar_t modulePath[MAX_PATH] {};
        const wchar_t* moduleName = L"<unknown>";
        HMODULE module = nullptr;
        if (address != nullptr
            && GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address),
                &module)
            && GetModuleFileNameW(module, modulePath, MAX_PATH) > 0)
        {
            modulePath[MAX_PATH - 1] = L'\0';
            moduleName = modulePath;
            for (const auto* character = modulePath; *character != L'\0'; ++character)
                if (*character == L'\\' || *character == L'/')
                    moduleName = character + 1;
        }

        SYSTEMTIME now {};
        GetSystemTime(&now);
        wchar_t report[2048] {};
        StringCchPrintfW(
            report,
            2048,
            L"Studio Duo %ls native crash\r\n"
            L"UTC: %04u-%02u-%02uT%02u:%02u:%02uZ\r\n"
            L"Exception: 0x%08lx\r\nAddress: %p\r\nModule: %ls\r\nThread: %lu\r\n",
            current->version.c_str(),
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond,
            code, address, moduleName, GetCurrentThreadId());

        auto persisted = false;
        auto writeError = DWORD { ERROR_SUCCESS };
        const auto file = CreateFileW(
            current->reportPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            const wchar_t bom = 0xfeff;
            DWORD written = 0;
            const auto bytes = static_cast<DWORD>(std::wcslen(report) * sizeof(wchar_t));
            persisted = WriteFile(file, &bom, sizeof(bom), &written, nullptr)
                && written == sizeof(bom)
                && WriteFile(file, report, bytes, &written, nullptr)
                && written == bytes
                && FlushFileBuffers(file);
            if (!persisted)
                writeError = GetLastError();
            if (!CloseHandle(file))
            {
                persisted = false;
                writeError = GetLastError();
            }
        }
        else
        {
            writeError = GetLastError();
        }
        OutputDebugStringW(report);

        if (current->showDialog.load(std::memory_order_relaxed))
        {
            wchar_t message[4096] {};
            if (persisted)
            {
                StringCchPrintfW(
                    message,
                    4096,
                    L"Studio Duo stopped unexpectedly.\n\n"
                    L"Exception: 0x%08lx\nModule: %ls\n\nCrash report:\n%ls\n\n"
                    L"If this happened during audio startup, launch Studio Duo with "
                    L"--safe-audio, then choose another driver in Settings.",
                    code, moduleName, current->reportPath.c_str());
            }
            else
            {
                StringCchPrintfW(
                    message,
                    4096,
                    L"Studio Duo stopped unexpectedly.\n\n"
                    L"Exception: 0x%08lx\nModule: %ls\n\n"
                    L"The crash report could not be saved (Windows error %lu).\n"
                    L"Launch Studio Duo with --safe-audio to skip audio startup.",
                    code, moduleName, writeError);
            }
            MessageBoxW(nullptr, message, L"Studio Duo startup or runtime error", MB_OK | MB_ICONERROR);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
};

WindowsCrashHandler::WindowsCrashHandler() = default;

WindowsCrashHandler::~WindowsCrashHandler()
{
    if (state != nullptr)
    {
        SetUnhandledExceptionFilter(state->previous);
        State::active.store(nullptr, std::memory_order_release);
    }
}

juce::Result WindowsCrashHandler::initialise(const juce::File& logDirectory)
{
    if (state != nullptr || State::active.load(std::memory_order_acquire) != nullptr)
        return juce::Result::fail("Windows crash reporting is already installed.");
    if (const auto created = logDirectory.createDirectory(); created.failed())
        return juce::Result::fail(
            "Could not create the Windows crash report directory: "
            + created.getErrorMessage());

    state = std::make_unique<State>();
    state->reportPath = logDirectory.getChildFile(
        "studio-duo-crash-"
        + juce::Time::getCurrentTime().formatted("%Y-%m-%d")
        + "-" + juce::Uuid().toString() + ".log").getFullPathName().toWideCharPointer();
    state->version = juce::String(STUDIO_DUO_VERSION).toWideCharPointer();
    ULONG stackReserve = 65536;
    if (!SetThreadStackGuarantee(&stackReserve))
        logError("app.crash", "Could not reserve stack space for Windows crash reporting.");
    State::active.store(state.get(), std::memory_order_release);
    state->previous = SetUnhandledExceptionFilter(State::handleCrash);
    return juce::Result::ok();
}

void WindowsCrashHandler::enableNativeDialog() noexcept
{
    if (state != nullptr)
        state->showDialog.store(true, std::memory_order_relaxed);
}
}

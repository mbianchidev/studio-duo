#include "WindowsCrashHandler.h"

#include "logging/StudioLogger.h"

#include <windows.h>
#include <dbghelp.h>
#include <strsafe.h>

#include <atomic>
#include <cwchar>
#include <string>

namespace studio
{
struct WindowsCrashHandler::State
{
    using MiniDumpWriteDumpFunction = decltype(&MiniDumpWriteDump);

    std::wstring reportPath;
    std::wstring dumpPath;
    std::wstring version;
    std::atomic<bool> showDialog { false };
    std::atomic<WindowsCrashContext> context {
        WindowsCrashContext::processStartup
    };
    LONG handlingCrash = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER previous = nullptr;
    HMODULE debugHelpLibrary = nullptr;
    MiniDumpWriteDumpFunction writeMiniDump = nullptr;
    inline static std::atomic<State*> active { nullptr };

    static const wchar_t* contextName(
        WindowsCrashContext value) noexcept
    {
        switch (value)
        {
            case WindowsCrashContext::processStartup:
                return L"Process startup";
            case WindowsCrashContext::audioDeviceProbe:
                return L"Audio/MIDI probe worker";
            case WindowsCrashContext::pluginWorker:
                return L"Plug-in worker";
            case WindowsCrashContext::mainWindowStartup:
                return L"Main window startup";
            case WindowsCrashContext::audioStartup:
                return L"Audio/MIDI startup";
            case WindowsCrashContext::runtime:
                return L"Main application runtime";
            case WindowsCrashContext::shutdown:
                return L"Application shutdown";
        }
        return L"Unknown";
    }

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
        const auto crashContext =
            current->context.load(std::memory_order_relaxed);
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

        auto dumpPersisted = false;
        auto dumpError = DWORD { ERROR_PROC_NOT_FOUND };
        if (current->writeMiniDump != nullptr)
        {
            const auto dump = CreateFileW(
                current->dumpPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                nullptr);
            if (dump != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION exceptionInformation {
                    GetCurrentThreadId(),
                    exception,
                    FALSE
                };
                constexpr auto dumpType = static_cast<MINIDUMP_TYPE>(
                    MiniDumpNormal
                    | MiniDumpWithThreadInfo
                    | MiniDumpWithUnloadedModules
                    | MiniDumpWithIndirectlyReferencedMemory
                    | MiniDumpScanMemory);
                dumpPersisted = current->writeMiniDump(
                    GetCurrentProcess(),
                    GetCurrentProcessId(),
                    dump,
                    dumpType,
                    exception != nullptr ? &exceptionInformation : nullptr,
                    nullptr,
                    nullptr)
                    && FlushFileBuffers(dump);
                if (!dumpPersisted)
                    dumpError = GetLastError();
                if (!CloseHandle(dump) && dumpPersisted)
                {
                    dumpPersisted = false;
                    dumpError = GetLastError();
                }
                if (!dumpPersisted)
                    DeleteFileW(current->dumpPath.c_str());
            }
            else
            {
                dumpError = GetLastError();
            }
        }

        SYSTEMTIME now {};
        GetSystemTime(&now);
        wchar_t dumpStatus[1024] {};
        if (dumpPersisted)
        {
            StringCchPrintfW(
                dumpStatus,
                1024,
                L"%ls",
                current->dumpPath.c_str());
        }
        else
        {
            StringCchPrintfW(
                dumpStatus,
                1024,
                L"<unavailable: Windows error %lu>",
                dumpError);
        }

        wchar_t report[4096] {};
        StringCchPrintfW(
            report,
            4096,
            L"Studio Duo %ls native crash\r\n"
            L"UTC: %04u-%02u-%02uT%02u:%02u:%02uZ\r\n"
            L"Exception: 0x%08lx\r\nAddress: %p\r\nModule: %ls\r\nThread: %lu\r\n"
            L"Context: %ls\r\nNative dump: %ls\r\n",
            current->version.c_str(),
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond,
            code, address, moduleName, GetCurrentThreadId(),
            contextName(crashContext), dumpStatus);

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
            const auto* recovery =
                crashContext == WindowsCrashContext::audioStartup
                ? L"If this happened during audio startup, relaunch Studio Duo with "
                  L"--safe-audio, then choose another driver in Settings."
                : L"Restart Studio Duo and attach the crash report and native dump "
                  L"when reporting the problem.";
            wchar_t message[6144] {};
            if (persisted)
            {
                StringCchPrintfW(
                    message,
                    6144,
                    L"Studio Duo stopped unexpectedly.\n\n"
                    L"Exception: 0x%08lx\nModule: %ls\nContext: %ls\n\n"
                    L"Crash report:\n%ls\n\nNative dump:\n%ls\n\n%ls",
                    code,
                    moduleName,
                    contextName(crashContext),
                    current->reportPath.c_str(),
                    dumpStatus,
                    recovery);
            }
            else
            {
                StringCchPrintfW(
                    message,
                    6144,
                    L"Studio Duo stopped unexpectedly.\n\n"
                    L"Exception: 0x%08lx\nModule: %ls\nContext: %ls\n\n"
                    L"The crash report could not be saved (Windows error %lu).\n"
                    L"Native dump: %ls\n\n%ls",
                    code,
                    moduleName,
                    contextName(crashContext),
                    writeError,
                    dumpStatus,
                    recovery);
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
        if (state->debugHelpLibrary != nullptr)
            FreeLibrary(state->debugHelpLibrary);
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

    const auto baseName =
        "studio-duo-crash-"
        + juce::Time::getCurrentTime().formatted("%Y-%m-%d")
        + "-" + juce::Uuid().toString();
    state = std::make_unique<State>();
    state->reportPath = logDirectory.getChildFile(
        baseName + ".log").getFullPathName().toWideCharPointer();
    state->dumpPath = logDirectory.getChildFile(
        baseName + ".dmp").getFullPathName().toWideCharPointer();
    state->version = juce::String(STUDIO_DUO_VERSION).toWideCharPointer();
    state->debugHelpLibrary = LoadLibraryExW(
        L"dbghelp.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (state->debugHelpLibrary != nullptr)
    {
        state->writeMiniDump =
            reinterpret_cast<State::MiniDumpWriteDumpFunction>(
                GetProcAddress(
                    state->debugHelpLibrary,
                    "MiniDumpWriteDump"));
    }
    if (state->writeMiniDump == nullptr)
    {
        logError(
            "app.crash",
            "Windows native dump support is unavailable (Windows error "
                + juce::String(static_cast<int>(GetLastError()))
                + ").");
    }
    ULONG stackReserve = 262144;
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

WindowsCrashContext WindowsCrashHandler::exchangeContext(
    WindowsCrashContext context) noexcept
{
    if (auto* current = State::active.load(std::memory_order_acquire))
        return current->context.exchange(
            context,
            std::memory_order_relaxed);
    return WindowsCrashContext::processStartup;
}
}

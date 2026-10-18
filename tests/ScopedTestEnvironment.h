#pragma once

#include "TestHarness.h"
#include <juce_core/juce_core.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

class ScopedTestEnvironment
{
public:
    ScopedTestEnvironment(std::string key, const std::string& value) : name(std::move(key))
    {
#if JUCE_WINDOWS
        char* data = nullptr;
        std::size_t size = 0;
        const auto error = _dupenv_s(&data, &size, name.c_str());
        const std::unique_ptr<char, decltype(&std::free)> owned(data, &std::free);
        if (error != 0)
        {
            result = juce::Result::fail("Cannot preserve the synthetic test environment (code " + juce::String(error) + ").");
            return;
        }
        const auto* previous = owned.get();
#else
        const auto* previous = std::getenv(name.c_str());
#endif
        if (previous != nullptr)
            original = previous;
        result = set(value.c_str());
        changed = result.wasOk();
    }

    ~ScopedTestEnvironment()
    {
        const auto restored = restore();
        expect(restored.wasOk(), restored.getErrorMessage().toRawUTF8());
    }
    ScopedTestEnvironment(const ScopedTestEnvironment&) = delete;
    ScopedTestEnvironment& operator=(const ScopedTestEnvironment&) = delete;

    [[nodiscard]] const juce::Result& status() const noexcept { return result; }

    juce::Result restore()
    {
        if (!changed)
            return juce::Result::ok();
        const auto restored = set(original ? original->c_str() : nullptr);
        if (restored.wasOk())
            changed = false;
        return restored;
    }

private:
    juce::Result set(const char* value)
    {
#if JUCE_WINDOWS
        const auto code = _putenv_s(name.c_str(), value != nullptr ? value : "");
#else
        const auto code = value != nullptr ? setenv(name.c_str(), value, 1) : unsetenv(name.c_str());
#endif
        return code == 0 ? juce::Result::ok()
            : juce::Result::fail("Cannot update the synthetic test environment (code " + juce::String(code) + ").");
    }
    std::string name;
    std::optional<std::string> original;
    juce::Result result { juce::Result::ok() };
    bool changed = false;
};

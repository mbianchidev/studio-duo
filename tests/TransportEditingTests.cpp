#include "TestHarness.h"
#include "TestSuites.h"

#include "model/TransportEditing.h"

#include <cmath>
#include <limits>

void transportEditingTests()
{
    auto project = studio::Project::createDefault();
    juce::String error;
    const auto expectPosition = [&](const char* text, double expected)
    {
        const auto result = studio::TransportEditing::secondsAtMusicalPosition(
            project, text, error);
        expect(result.has_value() && std::abs(*result - expected) < 1.0e-8,
               ("Musical loop position " + juce::String(text) + " resolves through tempo/meter maps: "
                + error).toRawUTF8());
    };
    expectPosition("1", 0.0);
    expectPosition("5:1", 8.0);
    expectPosition("2:3:480", 3.25);
    expect(studio::TransportEditing::musicalPositionText(project, 0.0) == "1:1:000",
           "New projects display a 4/4 loop origin using one-based bar and beat.");

    project.meterChanges = { { 0.0, 4, 4 }, { 4.0, 3, 8 } };
    expectPosition("3:1", 4.0);
    expectPosition("4:2:480", 5.125);
    expect(studio::TransportEditing::musicalPositionText(project, 5.125) == "4:2:480",
           "Musical loop positions round-trip after a time-signature change.");
    expect(!studio::TransportEditing::secondsAtMusicalPosition(project, "4:4", error),
           "A loop beat outside the active time signature is rejected.");

    project.meterChanges = { { 1.25, 3, 4 } };
    expectPosition("1:3:240", 1.125);
    expectPosition("2:1", 1.25);
    expect(!studio::TransportEditing::secondsAtMusicalPosition(project, "1:3:480", error),
           "Truncated bars cannot select a beat replaced by a meter-change boundary.");

    project.meterChanges.clear();
    project.tempoChanges = { { 0.0, 120.0, true }, { 4.0, 240.0, false } };
    expectPosition("2:1", std::sqrt(32.0) - 4.0);
    for (const auto* invalid : { "", "0", "-1", "1:0", "1:1:960", "1::2",
                                "1.5:1", "1:1:2x", "1:1:0:0", "2147483648:1" })
        expect(!studio::TransportEditing::secondsAtMusicalPosition(project, invalid, error)
                   && error.isNotEmpty(),
               "Malformed or out-of-range musical loop positions fail explicitly.");

    expect(studio::TransportEditing::validateLoopRange(
               { true, 2.125, 19.375 }, 48000.0).wasOk()
               && studio::TransportEditing::validateLoopRange(
                   { false, 2.125, 19.375 }, 44100.0).wasOk(),
           "Loop bounds are arbitrary timeline positions, not a fixed number of bars or sections.");
    expect(studio::TransportEditing::validateLoopRange(
               { true, 0.0, 1.0 / 48000.0 }, 48000.0).wasOk(),
           "A loop can contain exactly one playback sample.");
    for (const auto& invalid : {
             studio::LoopRangeSettings { true, -1.0, 8.0 },
             studio::LoopRangeSettings { true, 8.0, 8.0 },
             studio::LoopRangeSettings { true, 9.0, 8.0 },
             studio::LoopRangeSettings { true, 0.0, 0.1 / 48000.0 },
             studio::LoopRangeSettings { true, 0.0, std::numeric_limits<double>::infinity() },
             studio::LoopRangeSettings { true, std::numeric_limits<double>::quiet_NaN(), 8.0 },
             studio::LoopRangeSettings { true, 0.0, 1.0e100 } })
        expect(studio::TransportEditing::validateLoopRange(invalid, 48000.0).failed(),
               "Invalid loop bounds never silently fall back to a default loop.");
    expect(studio::TransportEditing::validateLoopRange({ true, 0.0, 8.0 }, 0.0).failed()
               && studio::TransportEditing::validateLoopRange(
                   { true, 0.0, 8.0 }, std::numeric_limits<double>::quiet_NaN()).failed(),
           "Loop configuration requires a finite positive sample rate.");

    project.sections = { { "end", "Boundary", 7.375 }, { "start", "Boundary", 1.25 } };
    const auto markers = studio::TransportEditing::markerRange(project, "start", "end", error);
    expect(markers.has_value() && std::abs(markers->getStart() - 1.25) < 1.0e-9
               && std::abs(markers->getEnd() - 7.375) < 1.0e-9,
           "Loop markers resolve by stable IDs regardless of name or vector order.");
    expect(!studio::TransportEditing::markerRange(project, "end", "start", error)
               && !studio::TransportEditing::markerRange(project, "start", "start", error)
               && !studio::TransportEditing::markerRange(project, "missing", "end", error),
           "Missing, equal, and reversed loop markers fail rather than choosing another range.");
}

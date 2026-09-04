#pragma once

#include <iostream>

inline int failures = 0;

inline void expect(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

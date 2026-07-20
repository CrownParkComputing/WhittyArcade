// Minimal bit-preserving floating-point helpers used by the standalone i960.
#pragma once

#include <cstdint>
#include <cstring>

inline double u2d(uint64_t value) {
    double result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline uint64_t d2u(double value) {
    uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

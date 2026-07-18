#pragma once

#include <chrono>

namespace qas {

/// @brief Converts a steady-clock interval to the profiling millisecond unit.
/// @param begin Inclusive interval start.
/// @param end Exclusive interval end, defaulting to the current steady-clock time.
/// @return Elapsed interval in milliseconds.
[[nodiscard]] inline double elapsed_milliseconds(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

}  // namespace qas

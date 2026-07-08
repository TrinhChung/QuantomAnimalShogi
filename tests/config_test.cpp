#include "core/config.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include "search/alpha_beta.hpp"

namespace {
int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}  // namespace

int main() {
    const auto debug = qas::profile_config("local_debug");
    const auto benchmark = qas::profile_config("local_benchmark");
    const auto safe = qas::profile_config("contest_safe");
    const auto high = qas::profile_config("contest_high_ram");
    const auto low = qas::profile_config("low_ram");
    check(!debug.safety.competition_mode && debug.tt.size_mb <= 256,
          "local_debug is readable and bounded");
    check(benchmark.instrumentation.benchmark_mode_enabled &&
              benchmark.instrumentation.csv_log_enabled &&
              benchmark.instrumentation.tt_matrix_enabled &&
              benchmark.instrumentation.benchmark_repeat_count == 7,
          "local_benchmark enables benchmark instrumentation");
    check(safe.search.l_eq_min_legal_count == 24 && safe.search.l_eq_min_depth_remaining == 4 &&
              safe.search.l_eq_min_duplicate_ratio == 0.25,
          "contest-safe L_eq trigger uses benchmarked selective thresholds");
    check(safe.tt.auto_size_enabled && safe.tt.max_memory_ratio <= 0.25,
          "contest_safe uses conservative RAM-aware TT");
    check(high.tt.max_memory_ratio == 0.35 && high.tt.max_size_mb == 2048 &&
              high.memory.reserve_memory_mb >= 1024,
          "contest_high_ram keeps a large reserve");
    check(!low.tt.auto_size_enabled && low.tt.size_mb <= 256, "low_ram has a bounded fixed TT");

    qas::EngineConfig fixed = low;
    fixed.tt.size_mb = 128;
    const auto resources = qas::estimate_resources(fixed, sizeof(qas::TTEntry));
    check(resources.tt_size_mb <= 128 && resources.tt_entry_count > 0,
          "fixed TT request is rounded down without exceeding memory");
    check((resources.tt_entry_count & (resources.tt_entry_count - 1)) == 0,
          "TT entry count is a power of two");
    check(sizeof(qas::TTEntry) <= 24, "TT entry remains compact");

    if (failures != 0)
        return EXIT_FAILURE;
    std::cout << "All configuration/resource tests passed\n";
    return EXIT_SUCCESS;
}

#include "core/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

void test_json_overrides_each_config_section() {
    const std::string path = "qas_config_test_override.json";
    {
        std::ofstream output(path);
        output << R"({
            "max_depth": 11,
            "l_eq_min_duplicate_ratio": 0.5,
            "tt_move_ordering_enabled": false,
            "tt_size_mb": 32,
            "tt_entry_replace_policy": "always",
            "reserve_memory_mb": 64,
            "benchmark_repeat_count": 3,
            "log_file_path": "test.csv",
            "competition_mode": false
        })";
    }

    const qas::EngineConfig config = qas::load_engine_config(path, "contest_safe");
    std::remove(path.c_str());

    check(config.search.max_depth == 11 && config.search.l_eq_min_duplicate_ratio == 0.5,
          "JSON overrides search configuration");
    check(!config.move_ordering.tt_move_ordering_enabled,
          "JSON overrides move-ordering configuration");
    check(config.tt.size_mb == 32 && config.tt.entry_replace_policy == "always",
          "JSON overrides transposition-table configuration");
    check(config.memory.reserve_memory_mb == 64, "JSON overrides memory configuration");
    check(config.instrumentation.benchmark_repeat_count == 3 &&
              config.instrumentation.log_file_path == "test.csv",
          "JSON overrides instrumentation configuration");
    check(!config.safety.competition_mode, "JSON overrides safety configuration");
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

    test_json_overrides_each_config_section();

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

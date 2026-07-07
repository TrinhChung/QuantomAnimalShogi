#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qas {

struct SearchConfig {
    int max_depth{64};
    int soft_time_limit_ms{25'000};
    int hard_time_limit_ms{29'000};
    bool iterative_deepening_enabled{true};
    bool pvs_enabled{true};
    bool aspiration_enabled{true};
    int aspiration_initial_window{50};
    int aspiration_max_retries{3};
    bool l_eq_enabled{true};
    bool l_eq_trigger_enabled{true};
    int l_eq_min_depth_remaining{2};
    std::size_t l_eq_min_legal_count{12};
    bool l_eq_require_duplicate_hand_hint{true};
};

struct MoveOrderingConfig {
    bool tt_move_ordering_enabled{true};
    bool immediate_win_ordering_enabled{true};
    bool prevent_loss_ordering_enabled{true};
    bool capture_ordering_enabled{true};
    bool lion_reduction_ordering_enabled{true};
    bool try_threat_ordering_enabled{true};
    bool mask_collapse_ordering_enabled{true};
    bool killer_enabled{true};
    bool history_enabled{true};
    std::uint64_t history_decay_interval{65'536};
};

struct TranspositionConfig {
    bool enabled{true};
    std::size_t size_mb{256};
    bool auto_size_enabled{true};
    double max_memory_ratio{0.25};
    std::string entry_replace_policy{"depth_age"};
    bool clear_each_move{false};
    bool age_enabled{true};
};

struct MemoryConfig {
    bool profile_enabled{false};
    std::size_t max_total_memory_mb{4096};
    std::size_t reserve_memory_mb{512};
    bool generation_cache_enabled{false};
    std::size_t generation_cache_size_mb{128};
    bool l_eq_cache_enabled{false};
    std::size_t l_eq_cache_size_mb{128};
    bool use_memory_pool{true};
    bool move_list_pool_enabled{true};
};

struct InstrumentationConfig {
    bool debug_counters_enabled{true};
    bool resource_estimation_enabled{false};
    bool benchmark_mode_enabled{false};
    bool per_depth_log_enabled{false};
    bool csv_log_enabled{false};
    bool stderr_log_enabled{true};
    std::string log_file_path{"engine_benchmark.csv"};
};

struct SafetyConfig {
    bool competition_mode{true};
    bool disable_expensive_asserts{true};
    bool disable_resource_estimation_during_search{true};
    bool disable_l_eq_when_low_time{true};
    bool emergency_depth_fallback{true};
    bool safe_fallback_move_enabled{true};
};

struct EngineConfig {
    std::string profile{"contest_safe"};
    SearchConfig search{};
    MoveOrderingConfig move_ordering{};
    TranspositionConfig tt{};
    MemoryConfig memory{};
    InstrumentationConfig instrumentation{};
    SafetyConfig safety{};
};

struct ResourceEstimate {
    std::size_t detected_ram_mb{0};
    std::size_t available_for_engine_mb{0};
    std::size_t tt_size_mb{0};
    std::size_t tt_entry_count{0};
    std::size_t tt_allocated_bytes{0};
};

EngineConfig profile_config(const std::string& profile);
EngineConfig load_engine_config(const std::string& path, const std::string& profile_override = {});
std::size_t detect_physical_ram_mb();
ResourceEstimate estimate_resources(const EngineConfig& config, std::size_t tt_entry_size,
                                    std::size_t min_tt_mb = 16,
                                    std::size_t max_tt_mb = 4096);

}  // namespace qas

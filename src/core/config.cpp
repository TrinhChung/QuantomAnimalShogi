#include "core/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace qas {
namespace {

/// @brief Finds the value separator associated with a flat JSON key lookup.
/// @param json JSON text to scan.
/// @param key Property name without quotes.
/// @return Colon position, or `std::string::npos` when the key or separator is absent.
std::size_t find_json_value_separator(const std::string& json, const std::string& key) {
    const auto position = json.find('"' + key + '"');
    return position == std::string::npos ? std::string::npos : json.find(':', position);
}

/// @brief Reads a Boolean property with fallback semantics.
/// @param json JSON text to scan.
/// @param key Property name.
/// @param fallback Value returned for missing or malformed input.
/// @return Parsed Boolean or fallback.
bool read_bool(const std::string& json, const std::string& key, bool fallback) {
    const auto colon = find_json_value_separator(json, key);
    if (colon == std::string::npos)
        return fallback;
    const auto value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos)
        return fallback;
    if (json.compare(value, 4, "true") == 0)
        return true;
    if (json.compare(value, 5, "false") == 0)
        return false;
    return fallback;
}

/// @brief Reads a numeric property with fallback semantics.
/// @param json JSON text to scan.
/// @param key Property name.
/// @param fallback Value returned for missing or malformed input.
/// @return Parsed number or fallback.
double read_number(const std::string& json, const std::string& key, double fallback) {
    const auto colon = find_json_value_separator(json, key);
    if (colon == std::string::npos)
        return fallback;
    const auto begin = json.find_first_of("-0123456789", colon + 1);
    if (begin == std::string::npos)
        return fallback;
    std::size_t end = begin;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
                                 json[end] == '.' || json[end] == '-'))
        ++end;
    try {
        return std::stod(json.substr(begin, end - begin));
    } catch (...) {
        return fallback;
    }
}

/// @brief Reads a quoted string property with fallback semantics.
/// @param json JSON text to scan.
/// @param key Property name.
/// @param fallback Value returned for missing or malformed input.
/// @return Parsed string or fallback.
std::string read_string(const std::string& json,
                        const std::string& key,
                        const std::string& fallback) {
    const auto colon = find_json_value_separator(json, key);
    if (colon == std::string::npos)
        return fallback;
    const auto quote = json.find('"', colon + 1);
    const auto end = json.find('"', quote + 1);
    if (quote == std::string::npos || end == std::string::npos) {
        return fallback;
    }
    return json.substr(quote + 1, end - quote - 1);
}

/// @brief Reads a numeric property and converts it to the destination type.
/// @tparam T Numeric destination type.
/// @param json JSON text to scan.
/// @param key Property name.
/// @param fallback Value returned for missing or malformed input.
/// @return Parsed and converted value or fallback.
template <typename T>
T number(const std::string& json, const std::string& key, T fallback) {
    return static_cast<T>(read_number(json, key, static_cast<double>(fallback)));
}

/// @brief Rounds a nonnegative size down to a power of two.
/// @param value Upper bound to round.
/// @return Greatest power of two not exceeding value, or zero for zero.
std::size_t floor_power_of_two(std::size_t value) {
    if (value == 0)
        return 0;
    std::size_t result = 1;
    while (result <= value / 2)
        result *= 2;
    return result;
}

/// @brief Applies JSON overrides owned by SearchConfig.
/// @param config Search configuration to mutate.
/// @param json JSON override text.
void apply_search_json(SearchConfig& config, const std::string& json) {
    config.max_depth = number(json, "max_depth", config.max_depth);
    config.soft_time_limit_ms = number(json, "soft_time_limit_ms", config.soft_time_limit_ms);
    config.hard_time_limit_ms = number(json, "hard_time_limit_ms", config.hard_time_limit_ms);
    config.iterative_deepening_enabled =
        read_bool(json, "iterative_deepening_enabled", config.iterative_deepening_enabled);
    config.pvs_enabled = read_bool(json, "pvs_enabled", config.pvs_enabled);
    config.aspiration_enabled = read_bool(json, "aspiration_enabled", config.aspiration_enabled);
    config.aspiration_initial_window =
        number(json, "aspiration_initial_window", config.aspiration_initial_window);
    config.aspiration_max_retries =
        number(json, "aspiration_max_retries", config.aspiration_max_retries);
    config.l_eq_enabled = read_bool(json, "l_eq_enabled", config.l_eq_enabled);
    config.l_eq_trigger_enabled =
        read_bool(json, "l_eq_trigger_enabled", config.l_eq_trigger_enabled);
    config.l_eq_min_depth_remaining =
        number(json, "l_eq_min_depth_remaining", config.l_eq_min_depth_remaining);
    config.l_eq_min_legal_count = number(json, "l_eq_min_legal_count", config.l_eq_min_legal_count);
    config.l_eq_min_duplicate_ratio =
        read_number(json, "l_eq_min_duplicate_ratio", config.l_eq_min_duplicate_ratio);
    config.l_eq_require_duplicate_hand_hint = read_bool(
        json, "l_eq_require_duplicate_hand_hint", config.l_eq_require_duplicate_hand_hint);
}

/// @brief Applies JSON overrides owned by MoveOrderingConfig.
/// @param config Move-ordering configuration to mutate.
/// @param json JSON override text.
void apply_move_ordering_json(MoveOrderingConfig& config, const std::string& json) {
    config.tt_move_ordering_enabled =
        read_bool(json, "tt_move_ordering_enabled", config.tt_move_ordering_enabled);
    config.immediate_win_ordering_enabled =
        read_bool(json, "immediate_win_ordering_enabled", config.immediate_win_ordering_enabled);
    config.prevent_loss_ordering_enabled =
        read_bool(json, "prevent_loss_ordering_enabled", config.prevent_loss_ordering_enabled);
    config.capture_ordering_enabled =
        read_bool(json, "capture_ordering_enabled", config.capture_ordering_enabled);
    config.lion_reduction_ordering_enabled =
        read_bool(json, "lion_reduction_ordering_enabled", config.lion_reduction_ordering_enabled);
    config.try_threat_ordering_enabled =
        read_bool(json, "try_threat_ordering_enabled", config.try_threat_ordering_enabled);
    config.mask_collapse_ordering_enabled =
        read_bool(json, "mask_collapse_ordering_enabled", config.mask_collapse_ordering_enabled);
    config.killer_enabled = read_bool(json, "killer_enabled", config.killer_enabled);
    config.history_enabled = read_bool(json, "history_enabled", config.history_enabled);
    config.history_decay_interval =
        number(json, "history_decay_interval", config.history_decay_interval);
}

/// @brief Applies JSON overrides owned by TranspositionConfig.
/// @param config Transposition-table configuration to mutate.
/// @param json JSON override text.
void apply_transposition_json(TranspositionConfig& config, const std::string& json) {
    config.enabled = read_bool(json, "tt_enabled", config.enabled);
    config.size_mb = number(json, "tt_size_mb", config.size_mb);
    config.max_size_mb = number(json, "tt_max_size_mb", config.max_size_mb);
    config.auto_size_enabled = read_bool(json, "tt_auto_size_enabled", config.auto_size_enabled);
    config.max_memory_ratio = read_number(json, "tt_max_memory_ratio", config.max_memory_ratio);
    config.entry_replace_policy =
        read_string(json, "tt_entry_replace_policy", config.entry_replace_policy);
    config.clear_each_move = read_bool(json, "tt_clear_each_move", config.clear_each_move);
    config.age_enabled = read_bool(json, "tt_age_enabled", config.age_enabled);
}

/// @brief Applies JSON overrides owned by MemoryConfig.
/// @param config Memory configuration to mutate.
/// @param json JSON override text.
void apply_memory_json(MemoryConfig& config, const std::string& json) {
    config.profile_enabled = read_bool(json, "memory_profile_enabled", config.profile_enabled);
    config.max_total_memory_mb = number(json, "max_total_memory_mb", config.max_total_memory_mb);
    config.reserve_memory_mb = number(json, "reserve_memory_mb", config.reserve_memory_mb);
    config.generation_cache_enabled =
        read_bool(json, "generation_cache_enabled", config.generation_cache_enabled);
    config.generation_cache_size_mb =
        number(json, "generation_cache_size_mb", config.generation_cache_size_mb);
    config.l_eq_cache_enabled = read_bool(json, "l_eq_cache_enabled", config.l_eq_cache_enabled);
    config.l_eq_cache_size_mb = number(json, "l_eq_cache_size_mb", config.l_eq_cache_size_mb);
    config.use_memory_pool = read_bool(json, "use_memory_pool", config.use_memory_pool);
    config.move_list_pool_enabled =
        read_bool(json, "move_list_pool_enabled", config.move_list_pool_enabled);
}

/// @brief Applies JSON overrides owned by InstrumentationConfig.
/// @param config Instrumentation configuration to mutate.
/// @param json JSON override text.
void apply_instrumentation_json(InstrumentationConfig& config, const std::string& json) {
    config.debug_counters_enabled =
        read_bool(json, "debug_counters_enabled", config.debug_counters_enabled);
    config.resource_estimation_enabled =
        read_bool(json, "resource_estimation_enabled", config.resource_estimation_enabled);
    config.benchmark_mode_enabled =
        read_bool(json, "benchmark_mode_enabled", config.benchmark_mode_enabled);
    config.per_depth_log_enabled =
        read_bool(json, "per_depth_log_enabled", config.per_depth_log_enabled);
    config.csv_log_enabled = read_bool(json, "csv_log_enabled", config.csv_log_enabled);
    config.stderr_log_enabled = read_bool(json, "stderr_log_enabled", config.stderr_log_enabled);
    config.tt_matrix_enabled = read_bool(json, "tt_matrix_enabled", config.tt_matrix_enabled);
    config.benchmark_repeat_count =
        number(json, "benchmark_repeat_count", config.benchmark_repeat_count);
    config.log_file_path = read_string(json, "log_file_path", config.log_file_path);
}

/// @brief Applies JSON overrides owned by SafetyConfig.
/// @param config Safety configuration to mutate.
/// @param json JSON override text.
void apply_safety_json(SafetyConfig& config, const std::string& json) {
    config.competition_mode = read_bool(json, "competition_mode", config.competition_mode);
    config.disable_expensive_asserts =
        read_bool(json, "disable_expensive_asserts", config.disable_expensive_asserts);
    config.disable_resource_estimation_during_search =
        read_bool(json,
                  "disable_resource_estimation_during_search",
                  config.disable_resource_estimation_during_search);
    config.disable_l_eq_when_low_time =
        read_bool(json, "disable_l_eq_when_low_time", config.disable_l_eq_when_low_time);
    config.emergency_depth_fallback =
        read_bool(json, "emergency_depth_fallback", config.emergency_depth_fallback);
    config.safe_fallback_move_enabled =
        read_bool(json, "safe_fallback_move_enabled", config.safe_fallback_move_enabled);
}

/// @brief Dispatches JSON overrides to every configuration owner.
/// @param config Complete engine configuration to mutate.
/// @param json JSON override text.
void apply_json(EngineConfig& config, const std::string& json) {
    apply_search_json(config.search, json);
    apply_move_ordering_json(config.move_ordering, json);
    apply_transposition_json(config.tt, json);
    apply_memory_json(config.memory, json);
    apply_instrumentation_json(config.instrumentation, json);
    apply_safety_json(config.safety, json);
}

}  // namespace

EngineConfig profile_config(const std::string& profile) {
    EngineConfig c;
    c.profile = profile;
    if (profile == "local_debug") {
        c.search.soft_time_limit_ms = 2000;
        c.search.hard_time_limit_ms = 2500;
        c.tt.size_mb = 64;
        c.tt.max_size_mb = 256;
        c.tt.auto_size_enabled = false;
        c.instrumentation.resource_estimation_enabled = true;
        c.instrumentation.per_depth_log_enabled = true;
        c.safety.competition_mode = false;
        c.safety.disable_expensive_asserts = false;
    } else if (profile == "local_benchmark") {
        c.tt.size_mb = 512;
        c.tt.auto_size_enabled = false;
        c.instrumentation.resource_estimation_enabled = true;
        c.instrumentation.benchmark_mode_enabled = true;
        c.instrumentation.tt_matrix_enabled = true;
        c.instrumentation.benchmark_repeat_count = 7;
        c.instrumentation.per_depth_log_enabled = true;
        c.instrumentation.csv_log_enabled = true;
        c.safety.competition_mode = false;
    } else if (profile == "contest_high_ram") {
        c.tt.size_mb = 1024;
        c.tt.max_memory_ratio = 0.35;
        c.tt.max_size_mb = 2048;
        c.memory.reserve_memory_mb = 1024;
    } else if (profile == "low_ram") {
        c.tt.size_mb = 128;
        c.tt.auto_size_enabled = false;
        c.tt.max_size_mb = c.tt.size_mb;
        c.memory.max_total_memory_mb = 512;
        c.memory.reserve_memory_mb = 128;
        c.instrumentation.debug_counters_enabled = false;
    } else if (profile == "contest_safe" || profile.empty()) {
        c.tt.max_size_mb = 1024;
        c.memory.max_total_memory_mb = 1536;
        c.memory.reserve_memory_mb = 512;
    } else {
        throw std::invalid_argument("unknown engine profile: " + profile);
    }
    return c;
}

EngineConfig load_engine_config(const std::string& path, const std::string& override_profile) {
    std::string json;
    if (!path.empty()) {
        std::ifstream input(path);
        if (input) {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            json = buffer.str();
        }
    }
    const std::string profile =
        override_profile.empty() ? read_string(json, "profile", "contest_safe") : override_profile;
    EngineConfig config = profile_config(profile);
    if (!json.empty())
        apply_json(config, json);
    config.profile = profile;
    return config;
}

std::size_t detect_physical_ram_mb() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<std::size_t>(status.ullTotalPhys >> 20U);
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && size > 0)
        return static_cast<std::size_t>(pages) * static_cast<std::size_t>(size) >> 20U;
#endif
    return 0;
}

ResourceEstimate estimate_resources(const EngineConfig& config,
                                    std::size_t entry_size,
                                    std::size_t min_tt_mb,
                                    std::size_t max_tt_mb) {
    ResourceEstimate result;
    result.detected_ram_mb = detect_physical_ram_mb();
    const std::size_t detected =
        result.detected_ram_mb == 0 ? config.memory.max_total_memory_mb : result.detected_ram_mb;
    const std::size_t capped = std::min(detected, config.memory.max_total_memory_mb);
    result.available_for_engine_mb =
        capped > config.memory.reserve_memory_mb ? capped - config.memory.reserve_memory_mb : 0;
    std::size_t target = config.tt.size_mb;
    if (config.tt.auto_size_enabled) {
        target = static_cast<std::size_t>(
            std::floor(result.available_for_engine_mb * config.tt.max_memory_ratio));
    }
    const std::size_t configured_max = std::min(max_tt_mb, config.tt.max_size_mb);
    const std::size_t safe_max = std::min(configured_max, result.available_for_engine_mb);
    target = safe_max < min_tt_mb ? safe_max : std::clamp(target, min_tt_mb, safe_max);
    const std::size_t bytes = target * 1024U * 1024U;
    result.tt_entry_count = floor_power_of_two(entry_size == 0 ? 0 : bytes / entry_size);
    result.tt_allocated_bytes = result.tt_entry_count * entry_size;
    result.tt_size_mb = result.tt_allocated_bytes >> 20U;
    return result;
}

}  // namespace qas

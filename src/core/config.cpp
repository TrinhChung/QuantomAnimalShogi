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

bool read_bool(const std::string& json, const std::string& key, bool fallback) {
    const auto position = json.find('"' + key + '"');
    if (position == std::string::npos) return fallback;
    const auto colon = json.find(':', position);
    if (colon == std::string::npos) return fallback;
    const auto value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos) return fallback;
    if (json.compare(value, 4, "true") == 0) return true;
    if (json.compare(value, 5, "false") == 0) return false;
    return fallback;
}

double read_number(const std::string& json, const std::string& key, double fallback) {
    const auto position = json.find('"' + key + '"');
    if (position == std::string::npos) return fallback;
    const auto colon = json.find(':', position);
    if (colon == std::string::npos) return fallback;
    const auto begin = json.find_first_of("-0123456789", colon + 1);
    if (begin == std::string::npos) return fallback;
    std::size_t end = begin;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
           json[end] == '.' || json[end] == '-')) ++end;
    try { return std::stod(json.substr(begin, end - begin)); }
    catch (...) { return fallback; }
}

std::string read_string(const std::string& json, const std::string& key,
                        const std::string& fallback) {
    const auto position = json.find('"' + key + '"');
    if (position == std::string::npos) return fallback;
    const auto colon = json.find(':', position);
    const auto quote = json.find('"', colon + 1);
    const auto end = json.find('"', quote + 1);
    if (colon == std::string::npos || quote == std::string::npos || end == std::string::npos) {
        return fallback;
    }
    return json.substr(quote + 1, end - quote - 1);
}

template <typename T>
T number(const std::string& json, const std::string& key, T fallback) {
    return static_cast<T>(read_number(json, key, static_cast<double>(fallback)));
}

std::size_t floor_power_of_two(std::size_t value) {
    if (value == 0) return 0;
    std::size_t result = 1;
    while (result <= value / 2) result *= 2;
    return result;
}

void apply_json(EngineConfig& c, const std::string& j) {
    c.search.max_depth = number(j, "max_depth", c.search.max_depth);
    c.search.soft_time_limit_ms = number(j, "soft_time_limit_ms", c.search.soft_time_limit_ms);
    c.search.hard_time_limit_ms = number(j, "hard_time_limit_ms", c.search.hard_time_limit_ms);
    c.search.iterative_deepening_enabled = read_bool(j, "iterative_deepening_enabled", c.search.iterative_deepening_enabled);
    c.search.pvs_enabled = read_bool(j, "pvs_enabled", c.search.pvs_enabled);
    c.search.aspiration_enabled = read_bool(j, "aspiration_enabled", c.search.aspiration_enabled);
    c.search.aspiration_initial_window = number(j, "aspiration_initial_window", c.search.aspiration_initial_window);
    c.search.aspiration_max_retries = number(j, "aspiration_max_retries", c.search.aspiration_max_retries);
    c.search.l_eq_enabled = read_bool(j, "l_eq_enabled", c.search.l_eq_enabled);
    c.search.l_eq_trigger_enabled = read_bool(j, "l_eq_trigger_enabled", c.search.l_eq_trigger_enabled);
    c.search.l_eq_min_depth_remaining = number(j, "l_eq_min_depth_remaining", c.search.l_eq_min_depth_remaining);
    c.search.l_eq_min_legal_count = number(j, "l_eq_min_legal_count", c.search.l_eq_min_legal_count);
    c.search.l_eq_require_duplicate_hand_hint = read_bool(j, "l_eq_require_duplicate_hand_hint", c.search.l_eq_require_duplicate_hand_hint);
    c.move_ordering.tt_move_ordering_enabled = read_bool(j, "tt_move_ordering_enabled", c.move_ordering.tt_move_ordering_enabled);
    c.move_ordering.immediate_win_ordering_enabled = read_bool(j, "immediate_win_ordering_enabled", c.move_ordering.immediate_win_ordering_enabled);
    c.move_ordering.prevent_loss_ordering_enabled = read_bool(j, "prevent_loss_ordering_enabled", c.move_ordering.prevent_loss_ordering_enabled);
    c.move_ordering.capture_ordering_enabled = read_bool(j, "capture_ordering_enabled", c.move_ordering.capture_ordering_enabled);
    c.move_ordering.lion_reduction_ordering_enabled = read_bool(j, "lion_reduction_ordering_enabled", c.move_ordering.lion_reduction_ordering_enabled);
    c.move_ordering.try_threat_ordering_enabled = read_bool(j, "try_threat_ordering_enabled", c.move_ordering.try_threat_ordering_enabled);
    c.move_ordering.mask_collapse_ordering_enabled = read_bool(j, "mask_collapse_ordering_enabled", c.move_ordering.mask_collapse_ordering_enabled);
    c.move_ordering.killer_enabled = read_bool(j, "killer_enabled", c.move_ordering.killer_enabled);
    c.move_ordering.history_enabled = read_bool(j, "history_enabled", c.move_ordering.history_enabled);
    c.move_ordering.history_decay_interval = number(j, "history_decay_interval", c.move_ordering.history_decay_interval);
    c.tt.enabled = read_bool(j, "tt_enabled", c.tt.enabled);
    c.tt.size_mb = number(j, "tt_size_mb", c.tt.size_mb);
    c.tt.auto_size_enabled = read_bool(j, "tt_auto_size_enabled", c.tt.auto_size_enabled);
    c.tt.max_memory_ratio = read_number(j, "tt_max_memory_ratio", c.tt.max_memory_ratio);
    c.tt.entry_replace_policy = read_string(j, "tt_entry_replace_policy", c.tt.entry_replace_policy);
    c.tt.clear_each_move = read_bool(j, "tt_clear_each_move", c.tt.clear_each_move);
    c.tt.age_enabled = read_bool(j, "tt_age_enabled", c.tt.age_enabled);
    c.memory.profile_enabled = read_bool(j, "memory_profile_enabled", c.memory.profile_enabled);
    c.memory.max_total_memory_mb = number(j, "max_total_memory_mb", c.memory.max_total_memory_mb);
    c.memory.reserve_memory_mb = number(j, "reserve_memory_mb", c.memory.reserve_memory_mb);
    c.memory.generation_cache_enabled = read_bool(j, "generation_cache_enabled", c.memory.generation_cache_enabled);
    c.memory.generation_cache_size_mb = number(j, "generation_cache_size_mb", c.memory.generation_cache_size_mb);
    c.memory.l_eq_cache_enabled = read_bool(j, "l_eq_cache_enabled", c.memory.l_eq_cache_enabled);
    c.memory.l_eq_cache_size_mb = number(j, "l_eq_cache_size_mb", c.memory.l_eq_cache_size_mb);
    c.memory.use_memory_pool = read_bool(j, "use_memory_pool", c.memory.use_memory_pool);
    c.memory.move_list_pool_enabled = read_bool(j, "move_list_pool_enabled", c.memory.move_list_pool_enabled);
    c.instrumentation.debug_counters_enabled = read_bool(j, "debug_counters_enabled", c.instrumentation.debug_counters_enabled);
    c.instrumentation.resource_estimation_enabled = read_bool(j, "resource_estimation_enabled", c.instrumentation.resource_estimation_enabled);
    c.instrumentation.benchmark_mode_enabled = read_bool(j, "benchmark_mode_enabled", c.instrumentation.benchmark_mode_enabled);
    c.instrumentation.per_depth_log_enabled = read_bool(j, "per_depth_log_enabled", c.instrumentation.per_depth_log_enabled);
    c.instrumentation.csv_log_enabled = read_bool(j, "csv_log_enabled", c.instrumentation.csv_log_enabled);
    c.instrumentation.stderr_log_enabled = read_bool(j, "stderr_log_enabled", c.instrumentation.stderr_log_enabled);
    c.instrumentation.log_file_path = read_string(j, "log_file_path", c.instrumentation.log_file_path);
    c.safety.competition_mode = read_bool(j, "competition_mode", c.safety.competition_mode);
    c.safety.disable_expensive_asserts = read_bool(j, "disable_expensive_asserts", c.safety.disable_expensive_asserts);
    c.safety.disable_resource_estimation_during_search = read_bool(j, "disable_resource_estimation_during_search", c.safety.disable_resource_estimation_during_search);
    c.safety.disable_l_eq_when_low_time = read_bool(j, "disable_l_eq_when_low_time", c.safety.disable_l_eq_when_low_time);
    c.safety.emergency_depth_fallback = read_bool(j, "emergency_depth_fallback", c.safety.emergency_depth_fallback);
    c.safety.safe_fallback_move_enabled = read_bool(j, "safe_fallback_move_enabled", c.safety.safe_fallback_move_enabled);
}

}  // namespace

EngineConfig profile_config(const std::string& profile) {
    EngineConfig c;
    c.profile = profile;
    if (profile == "local_debug") {
        c.search.soft_time_limit_ms = 2000; c.search.hard_time_limit_ms = 2500;
        c.tt.size_mb = 64; c.tt.auto_size_enabled = false;
        c.instrumentation.resource_estimation_enabled = true;
        c.instrumentation.per_depth_log_enabled = true;
        c.safety.competition_mode = false; c.safety.disable_expensive_asserts = false;
    } else if (profile == "local_benchmark") {
        c.tt.size_mb = 512; c.tt.auto_size_enabled = false;
        c.instrumentation.resource_estimation_enabled = true;
        c.instrumentation.benchmark_mode_enabled = true;
        c.instrumentation.per_depth_log_enabled = true;
        c.instrumentation.csv_log_enabled = true;
        c.safety.competition_mode = false;
    } else if (profile == "contest_high_ram") {
        c.tt.size_mb = 1024; c.tt.max_memory_ratio = 0.35;
        c.memory.reserve_memory_mb = 1024;
    } else if (profile == "low_ram") {
        c.tt.size_mb = 128; c.tt.auto_size_enabled = false;
        c.memory.max_total_memory_mb = 512; c.memory.reserve_memory_mb = 128;
        c.instrumentation.debug_counters_enabled = false;
    } else if (profile == "contest_safe" || profile.empty()) {
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
        if (input) { std::ostringstream buffer; buffer << input.rdbuf(); json = buffer.str(); }
    }
    const std::string profile = override_profile.empty()
                                    ? read_string(json, "profile", "contest_safe")
                                    : override_profile;
    EngineConfig config = profile_config(profile);
    if (!json.empty()) apply_json(config, json);
    config.profile = profile;
    return config;
}

std::size_t detect_physical_ram_mb() {
#ifdef _WIN32
    MEMORYSTATUSEX status{}; status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) return static_cast<std::size_t>(status.ullTotalPhys >> 20U);
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES); const long size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && size > 0) return static_cast<std::size_t>(pages) * static_cast<std::size_t>(size) >> 20U;
#endif
    return 0;
}

ResourceEstimate estimate_resources(const EngineConfig& config, std::size_t entry_size,
                                    std::size_t min_tt_mb, std::size_t max_tt_mb) {
    ResourceEstimate result;
    result.detected_ram_mb = detect_physical_ram_mb();
    const std::size_t detected = result.detected_ram_mb == 0
                                     ? config.memory.max_total_memory_mb
                                     : result.detected_ram_mb;
    const std::size_t capped = std::min(detected, config.memory.max_total_memory_mb);
    result.available_for_engine_mb = capped > config.memory.reserve_memory_mb
                                         ? capped - config.memory.reserve_memory_mb : 0;
    std::size_t target = config.tt.size_mb;
    if (config.tt.auto_size_enabled) {
        target = static_cast<std::size_t>(std::floor(result.available_for_engine_mb *
                                                     config.tt.max_memory_ratio));
    }
    const std::size_t safe_max = std::min(max_tt_mb, result.available_for_engine_mb);
    target = safe_max < min_tt_mb ? safe_max : std::clamp(target, min_tt_mb, safe_max);
    const std::size_t bytes = target * 1024U * 1024U;
    result.tt_entry_count = floor_power_of_two(entry_size == 0 ? 0 : bytes / entry_size);
    result.tt_allocated_bytes = result.tt_entry_count * entry_size;
    result.tt_size_mb = result.tt_allocated_bytes >> 20U;
    return result;
}

}  // namespace qas

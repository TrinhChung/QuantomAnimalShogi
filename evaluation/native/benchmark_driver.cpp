#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
// clang-format off: psapi.h requires Windows base types to be declared first.
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/resource.h>
#endif

#include "core/config.hpp"
#include "exact/equivalence.hpp"
#include "io/protocol.hpp"
#include "json_output.hpp"
#include "rules/game.hpp"
#include "search/alpha_beta.hpp"

namespace {

struct Arguments {
    std::string mode{"fixed"};
    std::string config_path{"engine_config.json"};
    int depth{6};
    int soft_ms{1000};
    int hard_ms{1100};
    std::size_t tt_size_mb{0};
    bool diagnostic{false};
};

Arguments parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--mode" && index + 1 < argc)
            arguments.mode = argv[++index];
        else if (argument == "--config" && index + 1 < argc)
            arguments.config_path = argv[++index];
        else if (argument == "--depth" && index + 1 < argc)
            arguments.depth = std::stoi(argv[++index]);
        else if (argument == "--soft-ms" && index + 1 < argc)
            arguments.soft_ms = std::stoi(argv[++index]);
        else if (argument == "--hard-ms" && index + 1 < argc)
            arguments.hard_ms = std::stoi(argv[++index]);
        else if (argument == "--tt-size-mb" && index + 1 < argc)
            arguments.tt_size_mb = std::stoull(argv[++index]);
        else if (argument == "--diagnostic")
            arguments.diagnostic = true;
        else
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
    }
    if (arguments.mode != "fixed" && arguments.mode != "time")
        throw std::invalid_argument("--mode must be fixed or time");
    if (arguments.depth < 1 || arguments.soft_ms < 1 || arguments.hard_ms < arguments.soft_ms)
        throw std::invalid_argument("invalid depth or time limit");
    return arguments;
}

qas::SearchOptions configured_options(const qas::EngineConfig& config) {
    qas::SearchOptions options;
    options.max_depth = config.search.max_depth;
    options.soft_time_limit_ms = config.search.soft_time_limit_ms;
    options.hard_time_limit_ms = config.search.hard_time_limit_ms;
    options.time_limit_ms = config.search.hard_time_limit_ms;
    options.iterative_deepening_enabled = config.search.iterative_deepening_enabled;
    options.pvs_enabled = config.search.pvs_enabled;
    options.aspiration_enabled = config.search.aspiration_enabled;
    options.aspiration_initial_window = config.search.aspiration_initial_window;
    options.aspiration_max_retries = config.search.aspiration_max_retries;
    options.use_tt = config.tt.enabled;
    options.tt_clear_each_move = config.tt.clear_each_move;
    options.tt_age_enabled = config.tt.age_enabled;
    options.killer_enabled = config.move_ordering.killer_enabled;
    options.history_enabled = config.move_ordering.history_enabled;
    options.history_decay_interval = config.move_ordering.history_decay_interval;
    options.tt_move_ordering_enabled = config.move_ordering.tt_move_ordering_enabled;
    options.immediate_win_ordering_enabled = config.move_ordering.immediate_win_ordering_enabled;
    options.prevent_loss_ordering_enabled = config.move_ordering.prevent_loss_ordering_enabled;
    options.capture_ordering_enabled = config.move_ordering.capture_ordering_enabled;
    options.lion_reduction_ordering_enabled = config.move_ordering.lion_reduction_ordering_enabled;
    options.try_threat_ordering_enabled = config.move_ordering.try_threat_ordering_enabled;
    options.mask_collapse_ordering_enabled = config.move_ordering.mask_collapse_ordering_enabled;
    if (config.search.l_eq_enabled && config.search.l_eq_trigger_enabled) {
        qas::enable_successor_equivalence(options,
                                          config.search.l_eq_min_legal_count,
                                          config.search.l_eq_require_duplicate_hand_hint);
        options.reducer_min_depth = config.search.l_eq_min_depth_remaining;
        options.reducer_min_duplicate_ratio = config.search.l_eq_min_duplicate_ratio;
        options.reducer_disable_low_time = config.safety.disable_l_eq_when_low_time;
    }
    return options;
}

double process_cpu_ms() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return -1.0;
    ULARGE_INTEGER kernel_time{}, user_time{};
    kernel_time.LowPart = kernel.dwLowDateTime;
    kernel_time.HighPart = kernel.dwHighDateTime;
    user_time.LowPart = user.dwLowDateTime;
    user_time.HighPart = user.dwHighDateTime;
    return static_cast<double>(kernel_time.QuadPart + user_time.QuadPart) / 10'000.0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return -1.0;
    return usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0 +
           usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
#endif
}

std::uint64_t peak_memory_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
}

template <typename Container, typename Encoder>
void write_array(std::ostream& output, const Container& values, Encoder encoder) {
    output << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first)
            output << ',';
        encoder(output, value);
        first = false;
    }
    output << ']';
}

void write_optional_timing(std::ostream& output, bool available, double value) {
    if (available)
        output << std::fixed << std::setprecision(6) << value;
    else
        output << "null";
}

void write_timed_counter(std::ostream& output, const qas::TimedCounter& counter) {
    output << "{\"calls\":" << counter.calls << ",\"elapsed_ms\":" << std::fixed
           << std::setprecision(6) << counter.elapsed_ms << '}';
}

void write_depth_reports(std::ostream& output, const std::vector<qas::DepthReport>& reports) {
    write_array(output, reports, [](std::ostream& stream, const qas::DepthReport& report) {
        stream << "{\"depth\":" << report.depth << ",\"score\":" << report.score
               << ",\"best_action\":" << qas::encode_action_move(report.best_move)
               << ",\"best_move\":";
        qas::evaluation::write_json_string(stream, qas::move_string(report.best_move));
        stream << ",\"nodes\":" << report.nodes << ",\"elapsed_ms\":" << std::fixed
               << std::setprecision(6) << report.elapsed_ms << ",\"pv_actions\":";
        write_array(stream, report.pv_line, [](std::ostream& pv_stream, const qas::Move& move) {
            pv_stream << qas::encode_action_move(move);
        });
        stream << '}';
    });
}

void write_rule_metrics(std::ostream& output, const qas::RuleMetrics& metrics) {
    output << "{\"pseudo_move_generation_calls\":" << metrics.pseudo_move_generation_calls
           << ",\"pseudo_moves_generated\":" << metrics.pseudo_moves_generated
           << ",\"legal_filter_calls\":" << metrics.legal_filter_calls
           << ",\"legal_moves_generated\":" << metrics.legal_moves_generated
           << ",\"pseudo_moves_rejected\":" << metrics.pseudo_moves_rejected
           << ",\"apply_move_internal_calls\":" << metrics.apply_move_internal_calls
           << ",\"apply_move_internal_successes\":" << metrics.apply_move_internal_successes
           << ",\"apply_move_internal_failures\":" << metrics.apply_move_internal_failures
           << ",\"propagation_calls\":" << metrics.propagation_calls
           << ",\"propagation_iterations\":" << metrics.propagation_iterations
           << ",\"hash_recompute_calls\":" << metrics.hash_recompute_calls
           << ",\"pseudo_move_generation_ms\":" << metrics.pseudo_move_generation_ms
           << ",\"legal_filter_ms\":" << metrics.legal_filter_ms
           << ",\"apply_move_internal_ms\":" << metrics.apply_move_internal_ms
           << ",\"propagation_ms\":" << metrics.propagation_ms
           << ",\"hash_recompute_ms\":" << metrics.hash_recompute_ms << '}';
}

void write_evaluation_components(std::ostream& output,
                                 const qas::EvalComponentProfile& components) {
    output << "{\"terminal\":";
    write_timed_counter(output, components.terminal);
    output << ",\"material_mask\":";
    write_timed_counter(output, components.material_mask);
    output << ",\"mobility\":";
    write_timed_counter(output, components.mobility);
    output << ",\"lion_safety\":";
    write_timed_counter(output, components.lion_safety);
    output << ",\"immediate_setup\":";
    write_timed_counter(output, components.immediate_setup);
    output << ",\"immediate_movegen\":";
    write_timed_counter(output, components.immediate_movegen);
    output << ",\"immediate_filter\":";
    write_timed_counter(output, components.immediate_filter);
    output << ",\"immediate_transition\":";
    write_timed_counter(output, components.immediate_transition);
    output << '}';
}

void write_result(const qas::State& root,
                  const qas::SearchResult& result,
                  const Arguments& arguments,
                  double cpu_ms,
                  std::uint64_t peak_memory) {
    const auto legal = qas::generate_legal_moves(root);
    const int action = result.has_move ? qas::encode_action_move(result.best_move) : -1;
    const bool legal_result =
        result.has_move && std::find(legal.begin(), legal.end(), result.best_move) != legal.end();
    qas::State after = root;
    if (legal_result) {
        qas::Undo undo;
        if (!qas::apply_move(after, result.best_move, undo))
            throw std::runtime_error("selected legal move did not apply");
    }
    const auto& stats = result.stats;
    const bool fixed = arguments.mode == "fixed";
    const bool completed = fixed ? stats.depth_reached == arguments.depth : stats.depth_reached > 0;
    const bool current_depth_completed =
        completed && (fixed || stats.started_depth == stats.depth_reached);
    const bool fallback_used = result.has_move && stats.depth_reached == 0;
    const double nps = stats.elapsed_ms <= 0.0
                           ? 0.0
                           : static_cast<double>(stats.searched_nodes) * 1000.0 / stats.elapsed_ms;
    const double raw_deadline_overshoot_ms =
        stats.elapsed_ms - static_cast<double>(arguments.hard_ms);
    const double deadline_overshoot_ms =
        raw_deadline_overshoot_ms > 0.0 ? raw_deadline_overshoot_ms : 0.0;
    const bool soft_limit_reached = stats.elapsed_ms >= static_cast<double>(arguments.soft_ms);
    const bool hard_limit_reached = stats.timeout_hit;
    std::cout << "{\"schema_version\":2,\"mode\":\"" << arguments.mode
              << "\",\"best_action\":" << action << ",\"decoded_move\":";
    qas::evaluation::write_json_string(std::cout, qas::move_string(result.best_move));
    std::cout << ",\"score\":" << result.score
              << ",\"completed\":" << (completed ? "true" : "false")
              << ",\"search_completed\":" << (current_depth_completed ? "true" : "false")
              << ",\"legal_result\":" << (legal_result ? "true" : "false")
              << ",\"legal_count\":" << legal.size() << ",\"legal_actions\":";
    write_array(std::cout, legal, [](std::ostream& output, const qas::Move& move) {
        output << qas::encode_action_move(move);
    });
    std::cout << ",\"terminal_result\":\"" << qas::terminal_name(root.terminal)
              << "\",\"state_hash_before\":" << root.hash << ",\"state_hash_after\":" << after.hash
              << ",\"completed_depth\":" << stats.depth_reached
              << ",\"started_depth\":" << stats.started_depth
              << ",\"requested_depth\":" << arguments.depth
              << ",\"soft_limit_ms\":" << arguments.soft_ms
              << ",\"hard_limit_ms\":" << arguments.hard_ms
              << ",\"soft_limit_reached\":" << (soft_limit_reached ? "true" : "false")
              << ",\"hard_limit_reached\":" << (hard_limit_reached ? "true" : "false")
              << ",\"deadline_overshoot_ms\":" << deadline_overshoot_ms
              << ",\"timeout_depth\":" << stats.timeout_depth
              << ",\"timeout\":" << (stats.timeout_hit ? "true" : "false")
              << ",\"elapsed_wall_ms\":" << std::fixed << std::setprecision(6) << stats.elapsed_ms
              << ",\"elapsed_process_cpu_ms\":";
    write_optional_timing(std::cout, cpu_ms >= 0.0, cpu_ms);
    std::cout << ",\"fallback_used\":" << (fallback_used ? "true" : "false")
              << ",\"fallback_reason\":";
    if (fallback_used)
        qas::evaluation::write_json_string(std::cout, "no_completed_depth");
    else
        std::cout << "null";
    std::cout << ",\"nodes\":" << stats.searched_nodes << ",\"nps\":" << nps << ",\"pv_actions\":";
    write_array(std::cout, result.pv_line, [](std::ostream& output, const qas::Move& move) {
        output << qas::encode_action_move(move);
    });
    std::cout << ",\"pv_decoded\":";
    write_array(std::cout, result.pv_line, [](std::ostream& output, const qas::Move& move) {
        qas::evaluation::write_json_string(output, qas::move_string(move));
    });
    std::cout << ",\"instrumentation_enabled\":" << (arguments.diagnostic ? "true" : "false")
              << ",\"completed_depth_reports\":";
    write_depth_reports(std::cout, stats.completed_depths);
    std::cout << ",\"expanded_nodes\":" << stats.expanded_nodes
              << ",\"generated_legal_moves\":" << stats.generated_legal_moves
              << ",\"max_legal_moves\":" << stats.max_legal_moves
              << ",\"equivalent_successor_moves\":" << stats.equivalent_successor_moves
              << ",\"tt_probes\":" << stats.tt_probes << ",\"tt_hits\":" << stats.tt_hits
              << ",\"tt_hit_rate\":"
              << (stats.tt_probes == 0 ? 0.0 : static_cast<double>(stats.tt_hits) / stats.tt_probes)
              << ",\"tt_exact_hits\":" << stats.tt_exact_hits
              << ",\"tt_lower_hits\":" << stats.tt_lower_hits
              << ",\"tt_upper_hits\":" << stats.tt_upper_hits
              << ",\"tt_cutoffs\":" << stats.tt_move_cutoffs
              << ",\"tt_replacements\":" << stats.tt_replacements
              << ",\"tt_collisions\":" << stats.tt_collisions_detected
              << ",\"tt_age_replacements\":" << stats.tt_age_replacements
              << ",\"tt_depth_replacements\":" << stats.tt_depth_replacements
              << ",\"tt_move_used\":" << stats.tt_move_used << ",\"cutoffs\":" << stats.cutoffs
              << ",\"first_move_cutoffs\":" << stats.first_move_cutoffs
              << ",\"first_move_cutoff_rate\":"
              << (stats.cutoffs == 0
                      ? 0.0
                      : static_cast<double>(stats.first_move_cutoffs) / stats.cutoffs)
              << ",\"average_cutoff_rank\":" << stats.average_cutoff_rank()
              << ",\"killer_cutoffs\":" << stats.killer_cutoffs
              << ",\"history_cutoffs\":" << stats.history_cutoffs
              << ",\"pvs_researches\":" << stats.pvs_researches
              << ",\"aspiration_retries\":" << stats.aspiration_retries
              << ",\"leq_calls\":" << stats.leq_grouped_nodes
              << ",\"leq_input_moves\":" << stats.leq_raw_moves
              << ",\"leq_output_representatives\":" << stats.leq_group_moves
              << ",\"leq_duplicate_ratio\":" << stats.duplicate_ratio()
              << ",\"leq_skipped_moves\":" << stats.leq_skipped_moves << ",\"leq_grouping_ms\":";
    write_optional_timing(std::cout, arguments.diagnostic, stats.leq_grouping_ms);
    std::cout << ",\"canonicalize_calls\":";
    if (arguments.diagnostic)
        std::cout << stats.canonicalize_calls;
    else
        std::cout << "null";
    std::cout << ",\"canonicalize_ms\":";
    write_optional_timing(std::cout, arguments.diagnostic, stats.canonicalize_ms);
    std::cout << ",\"propagation_calls\":";
    if (arguments.diagnostic)
        std::cout << stats.propagation_calls;
    else
        std::cout << "null";
    std::cout << ",\"propagation_ms\":";
    write_optional_timing(std::cout, arguments.diagnostic, stats.propagation_ms);
    std::cout << ",\"propagation_iterations\":";
    if (arguments.diagnostic)
        std::cout << stats.rule_metrics.propagation_iterations;
    else
        std::cout << "null";
    std::cout << ",\"movegen_calls\":";
    if (arguments.diagnostic)
        std::cout << stats.movegen_calls;
    else
        std::cout << "null";
    std::cout << ",\"movegen_ms\":";
    write_optional_timing(std::cout, arguments.diagnostic, stats.movegen_ms);
    std::cout << ",\"move_order_calls\":";
    if (arguments.diagnostic)
        std::cout << stats.move_order_calls;
    else
        std::cout << "null";
    std::cout << ",\"move_order_ms\":";
    write_optional_timing(std::cout, arguments.diagnostic, stats.move_order_ms);
    std::cout << ",\"evaluation_calls\":";
    if (arguments.diagnostic)
        std::cout << stats.eval_calls;
    else
        std::cout << "null";
    std::cout << ",\"evaluation_ms\":";
    write_optional_timing(std::cout, arguments.diagnostic, stats.eval_ms);
    std::cout << ",\"rule_metrics\":";
    if (arguments.diagnostic)
        write_rule_metrics(std::cout, stats.rule_metrics);
    else
        std::cout << "null";
    std::cout << ",\"evaluation_components\":";
    if (arguments.diagnostic)
        write_evaluation_components(std::cout, stats.eval_components);
    else
        std::cout << "null";
    std::cout << ",\"peak_memory_bytes\":" << peak_memory << "}" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        qas::EngineConfig config = qas::load_engine_config(arguments.config_path);
        if (arguments.tt_size_mb != 0) {
            config.tt.size_mb = arguments.tt_size_mb;
            config.tt.auto_size_enabled = false;
        }
        qas::SearchOptions options = configured_options(config);
        options.benchmark_instrumentation_enabled = arguments.diagnostic;
        if (arguments.mode == "fixed") {
            options.max_depth = arguments.depth;
            options.iterative_deepening_enabled = false;
            options.aspiration_enabled = false;
            options.soft_time_limit_ms = arguments.hard_ms;
            options.hard_time_limit_ms = arguments.hard_ms;
            options.tt_clear_each_move = true;
        } else {
            options.max_depth = arguments.depth;
            options.iterative_deepening_enabled = true;
            options.soft_time_limit_ms = arguments.soft_ms;
            options.hard_time_limit_ms = arguments.hard_ms;
        }
        const auto resources =
            qas::estimate_resources(config, sizeof(qas::TTEntry), 16, config.tt.max_size_mb);
        qas::AlphaBetaEngine engine(std::max<std::size_t>(1, resources.tt_entry_count));
        const qas::State state = qas::parse_state(std::cin);
        const double cpu_before = process_cpu_ms();
        const qas::SearchResult result = engine.find_best_move(state, options);
        const double cpu_after = process_cpu_ms();
        write_result(state,
                     result,
                     arguments,
                     cpu_before >= 0.0 && cpu_after >= 0.0 ? cpu_after - cpu_before : -1.0,
                     peak_memory_bytes());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "evaluation benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

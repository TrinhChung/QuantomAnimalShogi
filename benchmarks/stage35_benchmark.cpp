#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "exact/equivalence.hpp"
#include "io/protocol.hpp"
#include "search/alpha_beta.hpp"
#include "stage35_fixture.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: psapi.h requires Windows base types.
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kMebibyte = 1024U * 1024U;

struct Arguments {
    std::string suite{"smoke"};
    std::string fixture_directory{"benchmarks/fixtures/stage35"};
    std::string output_path{"reports/stage35-benchmark.csv"};
    std::string position_set{"required"};
    std::string position_filter;
    std::string mode_filter;
    std::string eval_mode{"optimized"};
    std::vector<int> depths{9, 10, 11, 12};
    std::vector<int> time_limits_ms{5000, 10000, 30000};
    std::vector<std::size_t> tt_sizes_mb{6, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    std::size_t default_tt_size_mb{256};
    std::size_t l_eq_threshold{24};
    int l_eq_min_depth{4};
    double l_eq_min_duplicate_ratio{0.25};
    int repeats{7};
    int hard_timeout_ms{30000};
    bool generate_fixtures{false};
    bool skip_guards{false};
};

struct Mode {
    std::string name;
    bool tt{false};
    bool pvs{false};
    bool strong_ordering{false};
    bool l_eq{false};
    std::size_t l_eq_threshold{24};
    int l_eq_min_depth{4};
    double l_eq_min_duplicate_ratio{0.25};
};

struct BenchmarkCase {
    std::string suite;
    const qas::benchmark::Fixture* fixture{nullptr};
    Mode mode{};
    int depth{0};
    int time_limit_ms{0};
    std::size_t tt_size_mb{0};
    bool iterative{false};
    bool optimized_eval{true};
};

struct ProcessMemory {
    double rss_mb{0.0};
    std::uint64_t page_faults{0};
};

struct Record {
    std::string suite;
    int run_id{0};
    std::string median_group_id;
    std::string mode;
    std::string eval_mode;
    std::size_t l_eq_threshold{0};
    int l_eq_min_depth{0};
    double l_eq_min_duplicate_ratio{0.0};
    std::string position;
    int depth{0};
    int time_limit_ms{0};
    std::size_t tt_size_requested_mb{0};
    std::size_t tt_size_mb{0};
    std::size_t tt_entries{0};
    bool completed{false};
    double elapsed_ms{0.0};
    double peak_rss_mb{0.0};
    double allocated_memory_mb{0.0};
    std::uint64_t page_faults{0};
    int external_action{-1};
    std::string best_move;
    int root_score{0};
    std::string pv_line;
    qas::SearchStats stats{};
    int illegal_move_count{0};
    bool score_stability{false};
    bool best_move_stability{false};
    std::string per_depth_time_ms;
    std::string per_depth_nodes;
    std::string per_depth_score;
    std::string per_depth_best_move;
    bool is_median{false};
    double group_median_ms{0.0};
    double group_min_ms{0.0};
    double group_max_ms{0.0};
    bool unstable{false};
    bool noisy{false};
};

std::vector<int> parse_int_list(const std::string& text) {
    std::vector<int> values;
    std::istringstream input(text);
    std::string item;
    while (std::getline(input, item, ','))
        values.push_back(std::stoi(item));
    if (values.empty())
        throw std::invalid_argument("empty integer list");
    return values;
}

std::vector<std::size_t> parse_size_list(const std::string& text) {
    std::vector<std::size_t> values;
    for (int value : parse_int_list(text)) {
        if (value <= 0)
            throw std::invalid_argument("TT sizes must be positive");
        values.push_back(static_cast<std::size_t>(value));
    }
    return values;
}

Arguments parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        auto value = [&]() -> std::string {
            if (++index >= argc)
                throw std::invalid_argument("missing value for " + option);
            return argv[index];
        };
        if (option == "--suite")
            arguments.suite = value();
        else if (option == "--fixtures")
            arguments.fixture_directory = value();
        else if (option == "--output")
            arguments.output_path = value();
        else if (option == "--position-set")
            arguments.position_set = value();
        else if (option == "--position")
            arguments.position_filter = value();
        else if (option == "--mode")
            arguments.mode_filter = value();
        else if (option == "--eval-mode")
            arguments.eval_mode = value();
        else if (option == "--depths")
            arguments.depths = parse_int_list(value());
        else if (option == "--time-limits-ms")
            arguments.time_limits_ms = parse_int_list(value());
        else if (option == "--tt-sizes-mb")
            arguments.tt_sizes_mb = parse_size_list(value());
        else if (option == "--default-tt-size-mb") {
            arguments.default_tt_size_mb = static_cast<std::size_t>(std::stoull(value()));
        } else if (option == "--repeats")
            arguments.repeats = std::stoi(value());
        else if (option == "--leq-threshold")
            arguments.l_eq_threshold = static_cast<std::size_t>(std::stoull(value()));
        else if (option == "--leq-min-depth")
            arguments.l_eq_min_depth = std::stoi(value());
        else if (option == "--leq-min-duplicate-ratio")
            arguments.l_eq_min_duplicate_ratio = std::stod(value());
        else if (option == "--hard-timeout-ms")
            arguments.hard_timeout_ms = std::stoi(value());
        else if (option == "--generate-fixtures")
            arguments.generate_fixtures = true;
        else if (option == "--skip-guards")
            arguments.skip_guards = true;
        else
            throw std::invalid_argument("unknown option: " + option);
    }
    if (arguments.repeats <= 0 || arguments.hard_timeout_ms <= 0 || arguments.l_eq_min_depth <= 0 ||
        arguments.l_eq_min_duplicate_ratio < 0.0 || arguments.l_eq_min_duplicate_ratio > 1.0) {
        throw std::invalid_argument("repeats and timeout must be positive");
    }
    if (arguments.eval_mode != "baseline" && arguments.eval_mode != "optimized")
        throw std::invalid_argument("eval mode must be baseline or optimized");
    return arguments;
}

std::size_t floor_power_of_two(std::size_t value) {
    if (value == 0)
        return 0;
    std::size_t result = 1;
    while (result <= value / 2)
        result *= 2;
    return result;
}

std::size_t tt_entries(std::size_t requested_mb) {
    return floor_power_of_two(requested_mb * kMebibyte / sizeof(qas::TTEntry));
}

ProcessMemory process_memory() {
    ProcessMemory result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        result.rss_mb = static_cast<double>(counters.WorkingSetSize) / kMebibyte;
        result.page_faults = counters.PageFaultCount;
    }
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t ignored = 0;
    std::uint64_t resident = 0;
    if (statm >> ignored >> resident) {
        result.rss_mb =
            static_cast<double>(resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE))) /
            kMebibyte;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        result.page_faults = static_cast<std::uint64_t>(usage.ru_minflt + usage.ru_majflt);
    }
#endif
    return result;
}

class MemoryMonitor {
   public:
    MemoryMonitor() : peak_mb_(process_memory().rss_mb), worker_([this]() { sample(); }) {}
    ~MemoryMonitor() { stop(); }

    void stop() {
        if (!running_.exchange(false))
            return;
        if (worker_.joinable())
            worker_.join();
        peak_mb_ = std::max(peak_mb_, process_memory().rss_mb);
    }
    double peak_mb() const { return peak_mb_; }

   private:
    void sample() {
        while (running_) {
            peak_mb_ = std::max(peak_mb_, process_memory().rss_mb);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::atomic<bool> running_{true};
    double peak_mb_{0.0};
    std::thread worker_;
};

std::vector<Mode> modes() {
    return {{"AB", false, false, false, false},
            {"AB+TT", true, false, false, false},
            {"AB+TT+PVS", true, true, false, false},
            {"AB+TT+PVS+ordering", true, true, true, false},
            {"AB+TT+PVS+ordering+L_eq_trigger", true, true, true, true}};
}

void disable_ordering(qas::SearchOptions& options) {
    options.strong_ordering_enabled = false;
    options.tt_move_ordering_enabled = false;
    options.immediate_win_ordering_enabled = false;
    options.prevent_loss_ordering_enabled = false;
    options.capture_ordering_enabled = false;
    options.lion_reduction_ordering_enabled = false;
    options.try_threat_ordering_enabled = false;
    options.mask_collapse_ordering_enabled = false;
    options.killer_enabled = false;
    options.history_enabled = false;
}

qas::SearchOptions search_options(const BenchmarkCase& benchmark_case, int hard_timeout_ms) {
    qas::SearchOptions options;
    options.max_depth = benchmark_case.depth;
    options.time_limit_ms =
        benchmark_case.time_limit_ms > 0 ? benchmark_case.time_limit_ms : hard_timeout_ms;
    options.soft_time_limit_ms =
        benchmark_case.iterative ? std::max(1, options.time_limit_ms * 97 / 100) : hard_timeout_ms;
    options.hard_time_limit_ms = benchmark_case.iterative ? options.time_limit_ms : hard_timeout_ms;
    options.iterative_deepening_enabled = benchmark_case.iterative;
    options.pvs_enabled = benchmark_case.mode.pvs;
    options.aspiration_enabled = benchmark_case.iterative && benchmark_case.mode.pvs;
    options.use_tt = benchmark_case.mode.tt;
    options.benchmark_instrumentation_enabled = true;
    options.optimized_eval_enabled = benchmark_case.optimized_eval;
    disable_ordering(options);
    if (benchmark_case.mode.name == "AB+TT+PVS") {
        options.tt_move_ordering_enabled = true;
        options.capture_ordering_enabled = true;
    }
    if (benchmark_case.mode.strong_ordering) {
        options.strong_ordering_enabled = true;
        options.tt_move_ordering_enabled = true;
        options.immediate_win_ordering_enabled = true;
        options.prevent_loss_ordering_enabled = true;
        options.capture_ordering_enabled = true;
        options.lion_reduction_ordering_enabled = true;
        options.try_threat_ordering_enabled = true;
        options.mask_collapse_ordering_enabled = true;
        options.killer_enabled = true;
        options.history_enabled = true;
    }
    if (benchmark_case.mode.l_eq) {
        qas::enable_successor_equivalence(options, benchmark_case.mode.l_eq_threshold, true);
        options.reducer_min_depth = benchmark_case.mode.l_eq_min_depth;
        options.reducer_min_duplicate_ratio = benchmark_case.mode.l_eq_min_duplicate_ratio;
    }
    return options;
}

std::string pv_string(const std::vector<qas::Move>& pv) {
    std::ostringstream output;
    for (std::size_t index = 0; index < pv.size(); ++index) {
        if (index != 0)
            output << ';';
        output << qas::move_string(pv[index]);
    }
    return output.str();
}

template <typename Function>
std::string depth_values(const std::vector<qas::DepthReport>& depths, Function function) {
    std::ostringstream output;
    for (std::size_t index = 0; index < depths.size(); ++index) {
        if (index != 0)
            output << ';';
        output << depths[index].depth << ':' << function(depths[index]);
    }
    return output.str();
}

bool legal_result(const qas::benchmark::Fixture& fixture,
                  const qas::SearchResult& result,
                  int& external_action) {
    if (!result.has_move)
        return false;
    const auto legal = qas::generate_legal_moves(fixture.state);
    if (std::find(legal.begin(), legal.end(), result.best_move) == legal.end())
        return false;
    external_action = qas::encode_action_move(result.best_move);
    if (external_action < 0 || external_action >= qas::external_action_count)
        return false;
    if (fixture.legal_action_mask[static_cast<std::size_t>(external_action)] == 0)
        return false;
    return qas::decode_action_move(fixture.state, external_action) == result.best_move;
}

Record run_once(const BenchmarkCase& benchmark_case, int run_id, int hard_timeout_ms) {
    const std::size_t entries = benchmark_case.mode.tt ? tt_entries(benchmark_case.tt_size_mb) : 1;
    if (entries == 0)
        throw std::runtime_error("TT request rounds to zero entries");
    qas::AlphaBetaEngine engine(entries);
    const ProcessMemory before = process_memory();
    MemoryMonitor monitor;
    const qas::State before_state = benchmark_case.fixture->state;
    const auto result = engine.find_best_move(benchmark_case.fixture->state,
                                              search_options(benchmark_case, hard_timeout_ms));
    monitor.stop();
    const ProcessMemory after = process_memory();

    Record record;
    record.suite = benchmark_case.suite;
    record.run_id = run_id;
    record.mode = benchmark_case.mode.name;
    record.eval_mode = benchmark_case.optimized_eval ? "optimized" : "baseline";
    record.l_eq_threshold = benchmark_case.mode.l_eq ? benchmark_case.mode.l_eq_threshold : 0;
    record.l_eq_min_depth = benchmark_case.mode.l_eq ? benchmark_case.mode.l_eq_min_depth : 0;
    record.l_eq_min_duplicate_ratio =
        benchmark_case.mode.l_eq ? benchmark_case.mode.l_eq_min_duplicate_ratio : 0.0;
    record.position = benchmark_case.fixture->name;
    record.depth = benchmark_case.depth;
    record.time_limit_ms = benchmark_case.time_limit_ms;
    record.tt_size_requested_mb = benchmark_case.mode.tt ? benchmark_case.tt_size_mb : 0;
    record.tt_size_mb = benchmark_case.mode.tt ? engine.tt_bytes() / kMebibyte : 0;
    record.tt_entries = benchmark_case.mode.tt ? engine.tt_entry_count() : 0;
    record.completed = benchmark_case.iterative
                           ? result.stats.depth_reached > 0
                           : result.stats.depth_reached == benchmark_case.depth;
    record.elapsed_ms = result.stats.elapsed_ms;
    record.peak_rss_mb = monitor.peak_mb();
    record.allocated_memory_mb = static_cast<double>(engine.tt_bytes()) / kMebibyte;
    record.page_faults =
        after.page_faults >= before.page_faults ? after.page_faults - before.page_faults : 0;
    record.best_move = qas::move_string(result.best_move);
    record.root_score = result.score;
    record.pv_line = pv_string(result.pv_line);
    record.stats = result.stats;
    if (!legal_result(*benchmark_case.fixture, result, record.external_action)) {
        record.illegal_move_count = 1;
    }
    if (!qas::same_state(before_state, benchmark_case.fixture->state) ||
        benchmark_case.fixture->state.hash != qas::zobrist_hash(benchmark_case.fixture->state)) {
        throw std::runtime_error("root state/hash changed: " + benchmark_case.fixture->name);
    }
    if (record.illegal_move_count != 0) {
        std::ostringstream error;
        error << "illegal result fixture=" << benchmark_case.fixture->name
              << " mode=" << benchmark_case.mode.name << " depth=" << benchmark_case.depth
              << " tt_mb=" << benchmark_case.tt_size_mb << " hash_before=" << before_state.hash
              << " hash_after=" << benchmark_case.fixture->state.hash;
        throw std::runtime_error(error.str());
    }
    record.per_depth_time_ms =
        depth_values(result.stats.completed_depths, [](const qas::DepthReport& depth) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3) << depth.elapsed_ms;
            return value.str();
        });
    record.per_depth_nodes =
        depth_values(result.stats.completed_depths,
                     [](const qas::DepthReport& depth) { return std::to_string(depth.nodes); });
    record.per_depth_score =
        depth_values(result.stats.completed_depths,
                     [](const qas::DepthReport& depth) { return std::to_string(depth.score); });
    record.per_depth_best_move = depth_values(
        result.stats.completed_depths,
        [](const qas::DepthReport& depth) { return qas::move_string(depth.best_move); });
    if (result.stats.completed_depths.size() >= 2) {
        const auto& current = result.stats.completed_depths.back();
        const auto& previous =
            result.stats.completed_depths[result.stats.completed_depths.size() - 2];
        record.score_stability = current.score == previous.score;
        record.best_move_stability = current.best_move == previous.best_move;
    }
    return record;
}

std::string group_id(const BenchmarkCase& benchmark_case) {
    std::ostringstream output;
    output << benchmark_case.suite << '|' << benchmark_case.mode.name << '|'
           << (benchmark_case.optimized_eval ? "optimized" : "baseline") << '|'
           << benchmark_case.fixture->name << '|' << benchmark_case.depth << '|'
           << benchmark_case.time_limit_ms << '|' << benchmark_case.tt_size_mb << '|'
           << benchmark_case.mode.l_eq_threshold << '|' << benchmark_case.mode.l_eq_min_depth << '|'
           << benchmark_case.mode.l_eq_min_duplicate_ratio;
    return output.str();
}

std::vector<Record> run_repeated(const BenchmarkCase& benchmark_case, const Arguments& arguments) {
    std::vector<Record> records;
    records.reserve(static_cast<std::size_t>(arguments.repeats) + 1U);
    const std::string id = group_id(benchmark_case);
    for (int run = 1; run <= arguments.repeats; ++run) {
        std::cerr << "[stage35] " << id << " run " << run << '/' << arguments.repeats << '\n';
        Record record = run_once(benchmark_case, run, arguments.hard_timeout_ms);
        record.median_group_id = id;
        records.push_back(std::move(record));
    }
    std::vector<std::size_t> order(records.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return records[left].elapsed_ms < records[right].elapsed_ms;
    });
    const double median = records[order[order.size() / 2]].elapsed_ms;
    const double minimum = records[order.front()].elapsed_ms;
    const double maximum = records[order.back()].elapsed_ms;
    bool unstable = false;
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index].completed != records[0].completed ||
            records[index].best_move != records[0].best_move ||
            records[index].root_score != records[0].root_score) {
            unstable = true;
        }
    }
    const bool noisy = median > 0.0 && (maximum - minimum) / median > 0.20;
    for (Record& record : records) {
        record.group_median_ms = median;
        record.group_min_ms = minimum;
        record.group_max_ms = maximum;
        record.unstable = unstable;
        record.noisy = noisy;
    }
    Record median_record = records[order[order.size() / 2]];
    median_record.run_id = 0;
    median_record.is_median = true;
    records.push_back(std::move(median_record));
    return records;
}

double ratio(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) / denominator;
}

std::string csv_text(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char character : value) {
        if (character == '"')
            escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

void write_header(std::ostream& output) {
    output
        << "suite,run_id,is_median,median_group_id,mode,eval_mode,l_eq_threshold,l_eq_min_depth,"
           "l_eq_min_duplicate_ratio,position,depth,time_limit_ms,"
           "tt_size_requested_mb,tt_size_mb,tt_entries,entry_size_bytes,completed,elapsed_ms,"
           "median_ms,min_ms,max_ms,unstable,noisy,nodes,expanded_nodes,nps,best_move_internal,"
           "best_move_external_action,root_score,pv_line,legal_move_avg,max_legal_moves,tt_probes,"
           "tt_hits,tt_hit_rate,tt_exact_hits,tt_lower_hits,tt_upper_hits,tt_best_move_used,"
           "tt_best_move_cutoffs,tt_replacements,tt_collisions_detected,tt_age_replacements,"
           "tt_depth_replacements,cutoffs,first_move_cutoffs,first_move_cutoff_rate,avg_cutoff_"
           "rank,"
           "killer_cutoffs,history_cutoffs,l_eq_calls,l_eq_enabled_nodes,l_eq_raw_L_sum,"
           "l_eq_group_L_sum,l_eq_duplicate_ratio,l_eq_skipped_moves,l_eq_group_ms,"
           "canonicalize_calls,canonicalize_ms,propagation_calls,propagation_ms,movegen_calls,"
           "movegen_ms,eval_calls,eval_ms,eval_terminal_calls,eval_terminal_ms,"
           "eval_material_calls,eval_material_ms,eval_mobility_calls,eval_mobility_ms,"
           "eval_lion_calls,eval_lion_ms,eval_immediate_setup_calls,eval_immediate_setup_ms,"
           "eval_immediate_movegen_calls,eval_immediate_movegen_ms,"
           "eval_immediate_filter_calls,eval_immediate_filter_ms,"
           "eval_immediate_transition_calls,eval_immediate_transition_ms,"
           "last_completed_depth,started_depth,timeout_depth,"
           "timeout_hit,per_depth_time_ms,per_depth_nodes,per_depth_score,per_depth_best_move,"
           "score_stability,best_move_stability,peak_rss_mb,allocated_memory_mb,page_faults,"
           "illegal_move_count\n";
}

void write_record(std::ostream& output, const Record& record) {
    const auto& stats = record.stats;
    const double nps = record.elapsed_ms > 0.0
                           ? static_cast<double>(stats.searched_nodes) * 1000.0 / record.elapsed_ms
                           : 0.0;
    const double leq_duplicate =
        stats.leq_raw_moves == 0 ? 0.0 : 1.0 - ratio(stats.leq_group_moves, stats.leq_raw_moves);
    output << record.suite << ',' << record.run_id << ',' << record.is_median << ','
           << csv_text(record.median_group_id) << ',' << csv_text(record.mode) << ','
           << record.eval_mode << ',' << record.l_eq_threshold << ',' << record.l_eq_min_depth
           << ',' << record.l_eq_min_duplicate_ratio << ',' << record.position << ','
           << record.depth << ',' << record.time_limit_ms << ',' << record.tt_size_requested_mb
           << ',' << record.tt_size_mb << ',' << record.tt_entries << ',' << sizeof(qas::TTEntry)
           << ',' << record.completed << ',' << std::fixed << std::setprecision(3)
           << record.elapsed_ms << ',' << record.group_median_ms << ',' << record.group_min_ms
           << ',' << record.group_max_ms << ',' << record.unstable << ',' << record.noisy << ','
           << stats.searched_nodes << ',' << stats.expanded_nodes << ',' << nps << ','
           << csv_text(record.best_move) << ',' << record.external_action << ','
           << record.root_score << ',' << csv_text(record.pv_line) << ','
           << stats.average_legal_moves() << ',' << stats.max_legal_moves << ',' << stats.tt_probes
           << ',' << stats.tt_hits << ',' << ratio(stats.tt_hits, stats.tt_probes) << ','
           << stats.tt_exact_hits << ',' << stats.tt_lower_hits << ',' << stats.tt_upper_hits << ','
           << stats.tt_move_used << ',' << stats.tt_move_cutoffs << ',' << stats.tt_replacements
           << ',' << stats.tt_collisions_detected << ',' << stats.tt_age_replacements << ','
           << stats.tt_depth_replacements << ',' << stats.cutoffs << ',' << stats.first_move_cutoffs
           << ',' << ratio(stats.first_move_cutoffs, stats.cutoffs) << ','
           << stats.average_cutoff_rank() << ',' << stats.killer_cutoffs << ','
           << stats.history_cutoffs << ',' << stats.leq_grouped_nodes << ','
           << stats.leq_grouped_nodes << ',' << stats.leq_raw_moves << ',' << stats.leq_group_moves
           << ',' << leq_duplicate << ',' << stats.leq_skipped_moves << ',' << stats.leq_grouping_ms
           << ',' << stats.canonicalize_calls << ',' << stats.canonicalize_ms << ','
           << stats.propagation_calls << ',' << stats.propagation_ms << ',' << stats.movegen_calls
           << ',' << stats.movegen_ms << ',' << stats.eval_calls << ',' << stats.eval_ms << ','
           << stats.eval_components.terminal.calls << ','
           << stats.eval_components.terminal.elapsed_ms << ','
           << stats.eval_components.material_mask.calls << ','
           << stats.eval_components.material_mask.elapsed_ms << ','
           << stats.eval_components.mobility.calls << ','
           << stats.eval_components.mobility.elapsed_ms << ','
           << stats.eval_components.lion_safety.calls << ','
           << stats.eval_components.lion_safety.elapsed_ms << ','
           << stats.eval_components.immediate_setup.calls << ','
           << stats.eval_components.immediate_setup.elapsed_ms << ','
           << stats.eval_components.immediate_movegen.calls << ','
           << stats.eval_components.immediate_movegen.elapsed_ms << ','
           << stats.eval_components.immediate_filter.calls << ','
           << stats.eval_components.immediate_filter.elapsed_ms << ','
           << stats.eval_components.immediate_transition.calls << ','
           << stats.eval_components.immediate_transition.elapsed_ms << ',' << stats.depth_reached
           << ',' << stats.started_depth << ',' << stats.timeout_depth << ',' << stats.timeout_hit
           << ',' << csv_text(record.per_depth_time_ms) << ',' << csv_text(record.per_depth_nodes)
           << ',' << csv_text(record.per_depth_score) << ',' << csv_text(record.per_depth_best_move)
           << ',' << record.score_stability << ',' << record.best_move_stability << ','
           << record.peak_rss_mb << ',' << record.allocated_memory_mb << ',' << record.page_faults
           << ',' << record.illegal_move_count << '\n';
}

bool selected_position(const qas::benchmark::Fixture& fixture,
                       const Arguments& arguments,
                       const std::string& suite) {
    if (!arguments.position_filter.empty() && fixture.name != arguments.position_filter) {
        return false;
    }
    if (suite == "tt") {
        static const std::vector<std::string> matrix_positions{"initial",
                                                               "duplicate_hands",
                                                               "high_uncertainty_midgame",
                                                               "low_uncertainty_midgame",
                                                               "near_try",
                                                               "near_catch",
                                                               "many_hands"};
        return std::find(matrix_positions.begin(), matrix_positions.end(), fixture.name) !=
               matrix_positions.end();
    }
    return arguments.position_set == "all" || fixture.category == "required";
}

std::vector<BenchmarkCase> build_cases(const std::vector<qas::benchmark::Fixture>& fixtures,
                                       const Arguments& arguments) {
    std::vector<BenchmarkCase> cases;
    auto all_modes = modes();
    for (Mode& mode : all_modes) {
        if (!mode.l_eq)
            continue;
        mode.l_eq_threshold = arguments.l_eq_threshold;
        mode.l_eq_min_depth = arguments.l_eq_min_depth;
        mode.l_eq_min_duplicate_ratio = arguments.l_eq_min_duplicate_ratio;
    }
    auto mode_selected = [&](const Mode& mode) {
        return arguments.mode_filter.empty() || arguments.mode_filter == mode.name;
    };
    if (arguments.suite == "smoke") {
        for (const auto& fixture : fixtures) {
            if (fixture.name != "initial" && fixture.name != "duplicate_hands")
                continue;
            cases.push_back(BenchmarkCase{"smoke", &fixture, all_modes.back(), 3, 0, 6, false});
        }
        for (BenchmarkCase& benchmark_case : cases)
            benchmark_case.optimized_eval = arguments.eval_mode == "optimized";
        return cases;
    }
    if (arguments.suite == "fixed" || arguments.suite == "all") {
        for (const Mode& mode : all_modes) {
            if (!mode_selected(mode))
                continue;
            for (const auto& fixture : fixtures) {
                if (!selected_position(fixture, arguments, "fixed"))
                    continue;
                for (int depth : arguments.depths) {
                    cases.push_back(BenchmarkCase{
                        "fixed", &fixture, mode, depth, 0, arguments.default_tt_size_mb, false});
                }
            }
        }
    }
    if (arguments.suite == "iterative" || arguments.suite == "all") {
        for (const auto& fixture : fixtures) {
            if (!selected_position(fixture, arguments, "iterative"))
                continue;
            for (int limit : arguments.time_limits_ms) {
                cases.push_back(BenchmarkCase{"iterative",
                                              &fixture,
                                              all_modes.back(),
                                              64,
                                              limit,
                                              arguments.default_tt_size_mb,
                                              true});
            }
        }
    }
    if (arguments.suite == "tt" || arguments.suite == "all") {
        for (const auto& fixture : fixtures) {
            if (!selected_position(fixture, arguments, "tt"))
                continue;
            for (std::size_t size_mb : arguments.tt_sizes_mb) {
                cases.push_back(
                    BenchmarkCase{"tt_fixed", &fixture, all_modes.back(), 10, 0, size_mb, false});
                cases.push_back(
                    BenchmarkCase{"tt_fixed", &fixture, all_modes.back(), 11, 0, size_mb, false});
                cases.push_back(BenchmarkCase{
                    "tt_iterative", &fixture, all_modes.back(), 64, 30000, size_mb, true});
            }
        }
    }
    if (cases.empty())
        throw std::invalid_argument("suite/filter selected no benchmark cases");
    for (BenchmarkCase& benchmark_case : cases)
        benchmark_case.optimized_eval = arguments.eval_mode == "optimized";
    return cases;
}

void run_correctness_guards(const std::vector<qas::benchmark::Fixture>& fixtures) {
    const auto required = std::find_if(fixtures.begin(), fixtures.end(), [](const auto& fixture) {
        return fixture.name == "initial";
    });
    const auto duplicate = std::find_if(fixtures.begin(), fixtures.end(), [](const auto& fixture) {
        return fixture.name == "duplicate_hands";
    });
    if (required == fixtures.end() || duplicate == fixtures.end()) {
        throw std::runtime_error("correctness guard fixtures are missing");
    }
    for (int depth = 1; depth <= 3; ++depth) {
        qas::AlphaBetaEngine ab(1U << 16U);
        qas::AlphaBetaEngine pvs(1U << 16U);
        BenchmarkCase ab_case{"guard", &*required, modes()[1], depth, 0, 1, false};
        BenchmarkCase pvs_case{"guard", &*required, modes()[2], depth, 0, 1, false};
        const auto ab_result = ab.find_best_move(required->state, search_options(ab_case, 60000));
        const auto pvs_result =
            pvs.find_best_move(required->state, search_options(pvs_case, 60000));
        if (ab_result.score != pvs_result.score) {
            throw std::runtime_error("AB/PVS guard mismatch at depth " + std::to_string(depth));
        }
    }
    qas::AlphaBetaEngine baseline(1U << 16U);
    qas::AlphaBetaEngine reduced(1U << 16U);
    BenchmarkCase baseline_case{"guard", &*duplicate, modes()[3], 2, 0, 1, false};
    BenchmarkCase reduced_case{"guard", &*duplicate, modes()[4], 2, 0, 1, false};
    const auto baseline_result =
        baseline.find_best_move(duplicate->state, search_options(baseline_case, 60000));
    const auto reduced_result =
        reduced.find_best_move(duplicate->state, search_options(reduced_case, 60000));
    if (baseline_result.score != reduced_result.score) {
        throw std::runtime_error("L_eq guard mismatch at depth 2");
    }
}

bool memory_safe(const BenchmarkCase& benchmark_case) {
    if (!benchmark_case.mode.tt)
        return true;
    const std::size_t physical_mb = qas::detect_physical_ram_mb();
    if (physical_mb == 0)
        return benchmark_case.tt_size_mb <= 1024;
    const std::size_t allocated_mb =
        tt_entries(benchmark_case.tt_size_mb) * sizeof(qas::TTEntry) / kMebibyte;
    return allocated_mb + 1024 <= physical_mb * 3 / 4;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        if (arguments.generate_fixtures) {
            const auto fixtures = qas::benchmark::generate_stage35_fixtures();
            qas::benchmark::save_stage35_fixtures(fixtures, arguments.fixture_directory);
            std::cerr << "[stage35] wrote " << fixtures.size() << " deterministic fixtures to "
                      << arguments.fixture_directory << '\n';
            if (arguments.suite == "generate")
                return EXIT_SUCCESS;
        }
        const auto fixtures = qas::benchmark::load_stage35_fixtures(arguments.fixture_directory);
        if (!arguments.skip_guards)
            run_correctness_guards(fixtures);
        const auto cases = build_cases(fixtures, arguments);
        std::ofstream output(arguments.output_path);
        if (!output)
            throw std::runtime_error("cannot write CSV: " + arguments.output_path);
        write_header(output);
        for (const BenchmarkCase& benchmark_case : cases) {
            if (!memory_safe(benchmark_case)) {
                std::cerr << "[stage35] skip unsafe TT allocation: " << group_id(benchmark_case)
                          << '\n';
                continue;
            }
            const auto records = run_repeated(benchmark_case, arguments);
            for (const Record& record : records)
                write_record(output, record);
            output.flush();
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "stage35 benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "exact/equivalence.hpp"
#include "io/protocol.hpp"
#include "search/alpha_beta.hpp"
#include "stage35_fixture.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct TestPosition {
    std::string name;
    qas::State state;
};

struct MovegenProfile {
    std::string name;
    std::size_t pseudo_count{0};
    std::size_t legal_count{0};
    double pseudo_us{0.0};
    double legal_filter_us{0.0};
    double apply_us{0.0};
    double propagation_us{0.0};
    double hash_us{0.0};
    std::uint64_t rejected{0};
    std::uint64_t apply_calls{0};
    std::uint64_t propagation_iterations{0};
};

struct PropagationProfile {
    std::string name;
    double reference_us{0.0};
    double lut_us{0.0};
    bool same_result{false};
};

struct SearchProfile {
    std::string name;
    int depth{0};
    int score{0};
    qas::Move best_move{};
    bool has_move{false};
    bool legal_best_move{false};
    std::uint64_t nodes{0};
    double total_ms{0.0};
    double movegen_ms{0.0};
    double legal_filter_ms{0.0};
    double pseudo_ms{0.0};
    double apply_ms{0.0};
    double propagation_ms{0.0};
    double hash_ms{0.0};
    double order_ms{0.0};
    double eval_ms{0.0};
    double leq_ms{0.0};
    std::uint64_t expanded_nodes{0};
    std::uint64_t generated_legal{0};
    std::uint64_t generated_pseudo{0};
    std::uint64_t rejected_pseudo{0};
    std::uint64_t apply_calls{0};
    std::uint64_t propagation_calls{0};
    std::uint64_t propagation_iterations{0};
    std::uint64_t move_order_calls{0};
    std::uint64_t leq_grouped_nodes{0};
    std::uint64_t leq_raw_moves{0};
    std::uint64_t leq_group_moves{0};
    std::uint64_t leq_skipped_moves{0};
    std::uint64_t cutoffs{0};
    std::uint64_t first_move_cutoffs{0};
};

struct ValidationProfile {
    std::string name;
    bool scores_match{true};
    bool nodes_match{true};
    bool best_moves_legal{true};
    bool best_moves_match{true};
};

const std::vector<std::string>& focused_names() {
    static const std::vector<std::string> names{"initial",
                                                "duplicate_hands",
                                                "high_uncertainty_midgame",
                                                "many_hands",
                                                "low_uncertainty_midgame"};
    return names;
}

std::vector<TestPosition> build_positions() {
    const auto fixtures = qas::benchmark::generate_stage35_fixtures();
    std::vector<TestPosition> positions;
    for (const std::string& name : focused_names()) {
        const auto found = std::find_if(
            fixtures.begin(), fixtures.end(), [&](const auto& item) { return item.name == name; });
        if (found == fixtures.end())
            throw std::runtime_error("missing focused fixture: " + name);
        positions.push_back(TestPosition{found->name, found->state});
    }
    return positions;
}

bool is_legal_best_move(const qas::State& state, const qas::Move& move) {
    const auto legal = qas::generate_legal_moves(state);
    if (std::find(legal.begin(), legal.end(), move) == legal.end())
        return false;
    const int action = qas::encode_action_move(move);
    return action >= 0 && action < qas::external_action_count &&
           qas::decode_action_move(state, action) == move;
}

qas::SearchOptions search_options(int depth, bool with_leq, qas::PropagationMode propagation_mode) {
    qas::SearchOptions options;
    options.max_depth = depth;
    options.time_limit_ms = 60'000;
    options.soft_time_limit_ms = 60'000;
    options.hard_time_limit_ms = 60'000;
    options.iterative_deepening_enabled = false;
    options.pvs_enabled = true;
    options.aspiration_enabled = false;
    options.use_tt = true;
    options.strong_ordering_enabled = true;
    options.benchmark_instrumentation_enabled = true;
    options.optimized_eval_enabled = true;
    options.propagation_mode = propagation_mode;
    if (with_leq) {
        qas::enable_successor_equivalence(options, 24, true);
        options.reducer_min_depth = 4;
        options.reducer_min_duplicate_ratio = 0.25;
    }
    return options;
}

MovegenProfile profile_movegen(const TestPosition& position, int iterations) {
    MovegenProfile result;
    result.name = position.name;
    std::vector<qas::Move> legal;
    std::vector<qas::Move> scratch;
    legal.reserve(128);
    scratch.reserve(128);

    qas::RuleMetrics metrics;
    for (int index = 0; index < iterations; ++index) {
        qas::generate_legal_moves_profiled(position.state, legal, scratch, metrics);
    }

    result.pseudo_count = scratch.size();
    result.legal_count = legal.size();
    result.rejected = metrics.pseudo_moves_rejected;
    result.apply_calls = metrics.apply_move_internal_calls;
    result.propagation_iterations = metrics.propagation_iterations;
    result.pseudo_us =
        metrics.pseudo_move_generation_calls == 0
            ? 0.0
            : metrics.pseudo_move_generation_ms * 1000.0 / metrics.pseudo_move_generation_calls;
    result.legal_filter_us = metrics.legal_filter_calls == 0
                                 ? 0.0
                                 : metrics.legal_filter_ms * 1000.0 / metrics.legal_filter_calls;
    result.apply_us =
        metrics.apply_move_internal_calls == 0
            ? 0.0
            : metrics.apply_move_internal_ms * 1000.0 / metrics.apply_move_internal_calls;
    result.propagation_us = metrics.propagation_calls == 0
                                ? 0.0
                                : metrics.propagation_ms * 1000.0 / metrics.propagation_calls;
    result.hash_us = metrics.hash_recompute_calls == 0
                         ? 0.0
                         : metrics.hash_recompute_ms * 1000.0 / metrics.hash_recompute_calls;
    return result;
}

PropagationProfile profile_propagation(const TestPosition& position, int iterations) {
    PropagationProfile result;
    result.name = position.name;

    auto measure = [&](qas::PropagationMode mode) {
        const auto begin = Clock::now();
        for (int index = 0; index < iterations; ++index) {
            qas::State copy = position.state;
            qas::propagate(copy, mode);
        }
        const auto end = Clock::now();
        return std::chrono::duration<double, std::micro>(end - begin).count() / iterations;
    };

    qas::State reference = position.state;
    qas::State optimized = position.state;
    const bool reference_ok = qas::propagate(reference, qas::PropagationMode::PermutationReference);
    const bool optimized_ok = qas::propagate(optimized, qas::PropagationMode::LineageLut);
    result.same_result = reference_ok == optimized_ok && reference.mask == optimized.mask;
    result.reference_us = measure(qas::PropagationMode::PermutationReference);
    result.lut_us = measure(qas::PropagationMode::LineageLut);
    return result;
}

SearchProfile profile_search(const TestPosition& position,
                             int depth,
                             bool with_leq,
                             qas::PropagationMode propagation_mode) {
    qas::AlphaBetaEngine engine(1ULL << 19U);
    const auto result =
        engine.find_best_move(position.state, search_options(depth, with_leq, propagation_mode));
    const auto& stats = result.stats;
    const auto& rules = stats.rule_metrics;

    SearchProfile profile;
    profile.name = position.name + (with_leq ? " (L_eq, LUT)" : " (no_L_eq, ");
    if (!with_leq) {
        profile.name +=
            propagation_mode == qas::PropagationMode::PermutationReference ? "ref)" : "LUT)";
    }
    profile.depth = depth;
    profile.score = result.score;
    profile.best_move = result.best_move;
    profile.has_move = result.has_move;
    profile.legal_best_move =
        result.has_move && is_legal_best_move(position.state, result.best_move);
    profile.nodes = stats.searched_nodes;
    profile.total_ms = stats.elapsed_ms;
    profile.movegen_ms = stats.movegen_ms;
    profile.legal_filter_ms = rules.legal_filter_ms;
    profile.pseudo_ms = rules.pseudo_move_generation_ms;
    profile.apply_ms = rules.apply_move_internal_ms;
    profile.propagation_ms = stats.propagation_ms;
    profile.hash_ms = rules.hash_recompute_ms;
    profile.order_ms = stats.move_order_ms;
    profile.eval_ms = stats.eval_ms;
    profile.leq_ms = stats.leq_grouping_ms;
    profile.expanded_nodes = stats.expanded_nodes;
    profile.generated_legal = stats.generated_legal_moves;
    profile.generated_pseudo = rules.pseudo_moves_generated;
    profile.rejected_pseudo = rules.pseudo_moves_rejected;
    profile.apply_calls = rules.apply_move_internal_calls;
    profile.propagation_calls = stats.propagation_calls;
    profile.propagation_iterations = rules.propagation_iterations;
    profile.move_order_calls = stats.move_order_calls;
    profile.leq_grouped_nodes = stats.leq_grouped_nodes;
    profile.leq_raw_moves = stats.leq_raw_moves;
    profile.leq_group_moves = stats.leq_group_moves;
    profile.leq_skipped_moves = stats.leq_skipped_moves;
    profile.cutoffs = stats.cutoffs;
    profile.first_move_cutoffs = stats.first_move_cutoffs;
    return profile;
}

ValidationProfile validate_position(const TestPosition& position) {
    ValidationProfile profile;
    profile.name = position.name;
    for (int depth = 1; depth <= 3; ++depth) {
        qas::AlphaBetaEngine reference(1ULL << 16U);
        qas::AlphaBetaEngine optimized(1ULL << 16U);
        auto reference_options =
            search_options(depth, false, qas::PropagationMode::PermutationReference);
        auto optimized_options = search_options(depth, false, qas::PropagationMode::LineageLut);
        reference_options.benchmark_instrumentation_enabled = false;
        optimized_options.benchmark_instrumentation_enabled = false;
        const auto reference_result = reference.find_best_move(position.state, reference_options);
        const auto optimized_result = optimized.find_best_move(position.state, optimized_options);
        profile.scores_match &= reference_result.score == optimized_result.score;
        profile.nodes_match &=
            reference_result.stats.searched_nodes == optimized_result.stats.searched_nodes;
        profile.best_moves_match &= reference_result.best_move == optimized_result.best_move;
        profile.best_moves_legal &=
            reference_result.has_move && optimized_result.has_move &&
            is_legal_best_move(position.state, reference_result.best_move) &&
            is_legal_best_move(position.state, optimized_result.best_move);
    }
    return profile;
}

double percent(double part, double total) {
    return total <= 0.0 ? 0.0 : 100.0 * part / total;
}

void print_movegen(const MovegenProfile& profile) {
    std::cout << std::left << std::setw(26) << profile.name << std::right << std::setw(7)
              << profile.pseudo_count << std::setw(7) << profile.legal_count << std::setw(9)
              << profile.rejected << std::setw(11) << std::fixed << std::setprecision(2)
              << profile.pseudo_us << std::setw(12) << profile.legal_filter_us << std::setw(11)
              << profile.apply_us << std::setw(10) << profile.propagation_us << std::setw(9)
              << profile.hash_us << '\n';
}

void print_propagation(const PropagationProfile& profile) {
    const double speedup = profile.lut_us > 0.0 ? profile.reference_us / profile.lut_us : 0.0;
    std::cout << std::left << std::setw(26) << profile.name << std::right << std::setw(13)
              << std::fixed << std::setprecision(3) << profile.reference_us << std::setw(11)
              << profile.lut_us << std::setw(10) << speedup << "x" << std::setw(9)
              << (profile.same_result ? "yes" : "no") << '\n';
}

void print_search(const SearchProfile& profile) {
    const double other_ms =
        profile.total_ms - profile.movegen_ms - profile.order_ms - profile.eval_ms - profile.leq_ms;
    std::cout << "\n[" << profile.name << "] depth=" << profile.depth << " score=" << profile.score
              << " best=" << qas::move_string(profile.best_move)
              << " legal_best=" << (profile.legal_best_move ? "yes" : "no")
              << " nodes=" << profile.nodes << " time=" << std::fixed << std::setprecision(1)
              << profile.total_ms << "ms\n";
    std::cout << "  expanded=" << profile.expanded_nodes
              << " legal_moves=" << profile.generated_legal
              << " pseudo_moves=" << profile.generated_pseudo
              << " rejected=" << profile.rejected_pseudo << " apply_calls=" << profile.apply_calls
              << '\n';
    std::cout << "  time: movegen=" << profile.movegen_ms << "ms ("
              << percent(profile.movegen_ms, profile.total_ms) << "%)"
              << " order=" << profile.order_ms << "ms ("
              << percent(profile.order_ms, profile.total_ms) << "%)"
              << " eval=" << profile.eval_ms << "ms (" << percent(profile.eval_ms, profile.total_ms)
              << "%)"
              << " leq=" << profile.leq_ms << "ms (" << percent(profile.leq_ms, profile.total_ms)
              << "%)"
              << " other=" << other_ms << "ms (" << percent(other_ms, profile.total_ms) << "%)\n";
    std::cout << "  rules: pseudo=" << profile.pseudo_ms
              << "ms legal_filter=" << profile.legal_filter_ms
              << "ms apply_inclusive=" << profile.apply_ms
              << "ms propagation=" << profile.propagation_ms << "ms hash=" << profile.hash_ms
              << "ms prop_calls=" << profile.propagation_calls
              << " prop_iterations=" << profile.propagation_iterations << '\n';
    if (profile.leq_raw_moves > 0) {
        const double duplicate_ratio =
            1.0 - static_cast<double>(profile.leq_group_moves) / profile.leq_raw_moves;
        std::cout << "  L_eq: grouped_nodes=" << profile.leq_grouped_nodes
                  << " raw=" << profile.leq_raw_moves << " reps=" << profile.leq_group_moves
                  << " skipped=" << profile.leq_skipped_moves
                  << " duplicate_ratio=" << std::setprecision(1) << duplicate_ratio * 100.0
                  << "%\n";
    }
    std::cout << "  cutoffs=" << profile.cutoffs
              << " first_move_cutoffs=" << profile.first_move_cutoffs
              << " move_order_calls=" << profile.move_order_calls << '\n';
}

void print_validation(const ValidationProfile& profile) {
    std::cout << std::left << std::setw(26) << profile.name
              << " scores=" << (profile.scores_match ? "ok" : "FAIL")
              << " nodes=" << (profile.nodes_match ? "ok" : "FAIL")
              << " best=" << (profile.best_moves_match ? "ok" : "DIFF")
              << " legal=" << (profile.best_moves_legal ? "ok" : "FAIL") << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        int search_depth = 8;
        if (argc > 1)
            search_depth = std::max(1, std::atoi(argv[1]));

        const auto positions = build_positions();

        std::cout << "=== Stage 5 Classical Profile ===\n";
        std::cout << "Search depth: " << search_depth << "\n\n";

        std::cout << "== Reference/LUT Validation (depths 1..3, no L_eq) ==\n";
        bool validation_ok = true;
        for (const auto& position : positions) {
            const auto profile = validate_position(position);
            print_validation(profile);
            validation_ok &= profile.scores_match && profile.nodes_match &&
                             profile.best_moves_legal && profile.best_moves_match;
        }
        if (!validation_ok)
            return EXIT_FAILURE;

        std::cout << "\n== Movegen and Legal Filtering (10000 iterations) ==\n";
        std::cout << std::left << std::setw(26) << "position" << std::right << std::setw(7)
                  << "pseudo" << std::setw(7) << "legal" << std::setw(9) << "rejected"
                  << std::setw(11) << "pgen_us" << std::setw(12) << "filter_us" << std::setw(11)
                  << "apply_us" << std::setw(10) << "prop_us" << std::setw(9) << "hash_us" << '\n';
        for (const auto& position : positions)
            print_movegen(profile_movegen(position, 10000));

        std::cout << "\n== Propagation Reference vs LUT (50000 iterations) ==\n";
        std::cout << std::left << std::setw(26) << "position" << std::right << std::setw(13)
                  << "ref_us" << std::setw(11) << "lut_us" << std::setw(11) << "speedup"
                  << std::setw(9) << "same" << '\n';
        for (const auto& position : positions)
            print_propagation(profile_propagation(position, 50000));

        std::cout << "\n== Full Search With L_eq ==\n";
        for (const auto& position : positions)
            print_search(
                profile_search(position, search_depth, true, qas::PropagationMode::LineageLut));

        std::cout << "\n== Full Search Without L_eq, Reference Propagation ==\n";
        for (const auto& position : positions)
            print_search(profile_search(
                position, search_depth, false, qas::PropagationMode::PermutationReference));

        std::cout << "\n== Full Search Without L_eq, LUT Propagation ==\n";
        for (const auto& position : positions)
            print_search(
                profile_search(position, search_depth, false, qas::PropagationMode::LineageLut));

        std::cout << "\n=== Stage 5 Profile Complete ===\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "stage5 profile failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

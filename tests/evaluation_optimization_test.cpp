#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "exact/equivalence.hpp"
#include "search/alpha_beta.hpp"
#include "stage35_fixture.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool is_legal(const qas::State& state, const qas::Move& move) {
    const auto legal = qas::generate_legal_moves(state);
    return std::find(legal.begin(), legal.end(), move) != legal.end();
}

qas::SearchOptions options(int depth, bool optimized) {
    qas::SearchOptions result;
    result.max_depth = depth;
    result.time_limit_ms = 60'000;
    result.soft_time_limit_ms = 60'000;
    result.hard_time_limit_ms = 60'000;
    result.iterative_deepening_enabled = false;
    result.aspiration_enabled = false;
    result.optimized_eval_enabled = optimized;
    qas::enable_successor_equivalence(result, 24, true);
    result.reducer_min_depth = 4;
    result.reducer_min_duplicate_ratio = 0.25;
    return result;
}

}  // namespace

int main() {
    const auto fixtures = qas::benchmark::load_stage35_fixtures("benchmarks/fixtures/stage35");
    const std::vector<std::string> focused{"initial",
                                           "duplicate_hands",
                                           "high_uncertainty_midgame",
                                           "many_hands",
                                           "low_uncertainty_midgame"};
    for (const std::string& name : focused) {
        const auto found = std::find_if(fixtures.begin(), fixtures.end(), [&](const auto& fixture) {
            return fixture.name == name;
        });
        check(found != fixtures.end(), "focused fixture exists: " + name);
        if (found == fixtures.end())
            continue;
        for (int depth = 1; depth <= 3; ++depth) {
            qas::AlphaBetaEngine baseline(1U << 16U);
            qas::AlphaBetaEngine optimized(1U << 16U);
            const auto before = baseline.find_best_move(found->state, options(depth, false));
            const auto after = optimized.find_best_move(found->state, options(depth, true));
            const std::string context = name + " depth " + std::to_string(depth);
            check(before.score == after.score, context + " preserves root score");
            check(
                is_legal(found->state, before.best_move) && is_legal(found->state, after.best_move),
                context + " returns legal moves in both eval modes");
            check(before.stats.searched_nodes == after.stats.searched_nodes,
                  context + " preserves search tree and ordering");
            if (before.best_move != after.best_move) {
                std::cerr << "NOTE: equal-score legal tie at " << context << '\n';
            }
        }
    }
    if (failures != 0) {
        std::cerr << failures << " evaluation optimization test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All evaluation optimization tests passed\n";
    return EXIT_SUCCESS;
}

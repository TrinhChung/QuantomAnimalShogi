#include "search/dfpn.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "search/guarded_search.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

qas::State parse(const std::string& text) {
    std::istringstream input(text);
    return qas::parse_state(input);
}

// North to move. Moving the Lion 6->9 captures on North's Try rank; South cannot recapture, so the
// pending Try survives every reply (reached in a tournament game, re-proven by two solvers).
const char* kTryWin =
    "N 39\n0 N S H2 E\n1 S S 11 L\n2 S S 9 G\n3 N S 7 C\n4 N N 6 L\n5 N N 0 E\n6 S N 8 C\n7 N N H0 "
    "G\n";

void test_proves_forced_try_with_verified_proof() {
    const qas::State state = parse(kTryWin);
    qas::DfpnSolver solver(std::size_t{1} << 16U);
    solver.reset(state.side_to_move, 13);
    const auto result =
        solver.prove(state, std::chrono::steady_clock::now() + std::chrono::seconds(10));
    check(result.status == qas::ProofStatus::Proven, "forced Try is proven for the side to move");
    check(qas::move_string(result.winning_move) == "p4:6->9", "proof reports the Try-entry move");
    check(solver.verify_proof(state) > 0, "proof re-checks with rule functions only");
}

void test_no_false_proof_in_opening() {
    qas::State state = qas::initial_state();
    for (const qas::Move& move : qas::generate_legal_moves(state)) {
        if (qas::move_string(move) == "p3:7->4") {
            qas::Undo undo;
            qas::apply_move(state, move, undo);
            break;
        }
    }
    qas::DfpnSolver solver(std::size_t{1} << 18U);
    solver.reset(state.side_to_move, 23);
    const auto begin = std::chrono::steady_clock::now();
    const auto result = solver.prove(state, begin + std::chrono::milliseconds(300));
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - begin)
                           .count();
    check(result.status != qas::ProofStatus::Proven, "opening exchange has no forced win");
    check(spent < 1500, "df-pn honours its wall-clock deadline");
}

void test_stop_flag_prevents_false_proof() {
    const qas::State state = parse(kTryWin);
    std::atomic<bool> stop{true};
    qas::DfpnSolver solver(std::size_t{1} << 12U);
    solver.reset(qas::opposite(state.side_to_move), 13);
    const auto result = solver.prove(state,
                                     std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                     ~std::uint64_t{0},
                                     &stop);
    check(result.status != qas::ProofStatus::Proven, "a raised stop flag never yields a proof");
}

void test_guard_plays_own_forced_win_and_stays_in_budget() {
    const qas::State state = parse(kTryWin);
    qas::AlphaBetaEngine engine(std::size_t{1} << 16U);
    qas::SearchOptions options;
    options.max_depth = 64;
    options.time_limit_ms = options.hard_time_limit_ms = 2000;
    options.soft_time_limit_ms = 1700;
    qas::GuardOptions guard;
    guard.max_ply = 13;
    guard.own_table_entries = std::size_t{1} << 16U;
    guard.refute_table_entries = std::size_t{1} << 18U;
    qas::GuardReport report;
    const auto begin = std::chrono::steady_clock::now();
    const auto result = qas::guarded_find_best_move(engine, state, options, guard, &report);
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - begin)
                           .count();
    check(result.has_move && qas::move_string(result.best_move) == "p4:6->9",
          "guard plays the proven forced win");
    check(report.own_forced_win, "guard reports the own forced win");
    check(spent <= 2000 + 300, "guarded decision stays inside the time budget");
}

void test_guard_disabled_is_plain_search() {
    const qas::State state = qas::initial_state();
    qas::AlphaBetaEngine engine(std::size_t{1} << 16U);
    qas::SearchOptions options;
    options.max_depth = 3;
    options.time_limit_ms = options.hard_time_limit_ms = 5000;
    qas::GuardOptions guard;
    guard.enabled = false;
    const auto guarded = qas::guarded_find_best_move(engine, state, options, guard);
    qas::AlphaBetaEngine reference(std::size_t{1} << 16U);
    const auto plain = reference.find_best_move(state, options);
    check(guarded.best_move == plain.best_move && guarded.score == plain.score,
          "disabled guard returns the plain search result");
}

}  // namespace

int main() {
    test_proves_forced_try_with_verified_proof();
    test_no_false_proof_in_opening();
    test_stop_flag_prevents_false_proof();
    test_guard_plays_own_forced_win_and_stays_in_budget();
    test_guard_disabled_is_plain_search();
    if (failures != 0) {
        std::cerr << failures << " df-pn/guard test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All df-pn and guard tests passed\n";
    return EXIT_SUCCESS;
}

#include "search/alpha_beta.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void place(qas::State& state, int piece, int square) {
    if (state.pos[piece] < qas::board_size) state.board[state.pos[piece]] = -1;
    state.pos[piece] = static_cast<std::uint8_t>(square);
    if (square < qas::board_size) state.board[square] = static_cast<std::int8_t>(piece);
}

qas::State catch_position() {
    qas::State state = qas::initial_state();
    state.mask = {
        qas::bit(qas::Animal::Giraffe), qas::bit(qas::Animal::Chick),
        qas::bit(qas::Animal::Elephant), qas::bit(qas::Animal::Lion),
        qas::bit(qas::Animal::Lion), qas::bit(qas::Animal::Chick),
        qas::bit(qas::Animal::Giraffe), qas::bit(qas::Animal::Elephant)};
    state.board.fill(-1);
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        state.pos[piece] = static_cast<std::uint8_t>(qas::first_hand_slot + piece);
    }
    place(state, 0, 4);
    place(state, 1, 10);
    place(state, 3, 11);
    place(state, 4, 1);
    place(state, 5, 0);
    place(state, 6, 2);
    place(state, 7, 3);
    state.side_to_move = qas::Side::South;
    qas::recompute_hash(state);
    return state;
}

bool is_legal(const qas::State& state, const qas::Move& move) {
    const auto legal = qas::generate_legal_moves(state);
    return std::find(legal.begin(), legal.end(), move) != legal.end();
}

void test_depths_are_legal_and_non_mutating() {
    qas::AlphaBetaEngine engine(1U << 16U);
    const qas::State state = qas::initial_state();
    for (int depth = 1; depth <= 3; ++depth) {
        const auto result = engine.search_fixed_depth(state, depth, true);
        check(result.has_move, "depth search returns a move");
        check(is_legal(state, result.best_move), "depth search returns a legal move");
        check(result.stats.depth_reached == depth, "fixed depth fully completes");
        check(state.hash == qas::zobrist_hash(state), "search does not mutate root state");
    }
}

void test_immediate_catch() {
    qas::AlphaBetaEngine engine(1U << 14U);
    const qas::State state = catch_position();
    const auto result = engine.search_fixed_depth(state, 2, true);
    check(result.has_move, "Catch position returns a move");
    check(result.best_move == qas::Move{0, 4, 1},
          "Alpha-Beta chooses immediate winning Catch");
    check(result.score >= qas::mate_score - 4, "Catch receives mate score");
}

void test_tt_preserves_value_and_hits() {
    const qas::State state = qas::initial_state();
    qas::AlphaBetaEngine no_tt(1U << 16U);
    qas::AlphaBetaEngine with_tt(1U << 16U);
    const auto baseline = no_tt.search_fixed_depth(state, 3, false);
    const auto cached = with_tt.search_fixed_depth(state, 3, true);
    check(baseline.score == cached.score, "TT preserves fixed-depth root value");
    check(cached.stats.tt_probes > 0, "TT is probed");
    check(cached.stats.tt_hits > 0, "iterative deepening produces TT hits");
}

qas::SearchResult configured_search(const qas::State& state, int depth, bool pvs,
                                    bool aspiration) {
    qas::AlphaBetaEngine engine(1U << 16U);
    qas::SearchOptions options;
    options.max_depth = depth;
    options.time_limit_ms = 60'000;
    options.soft_time_limit_ms = 60'000;
    options.hard_time_limit_ms = 60'000;
    options.pvs_enabled = pvs;
    options.aspiration_enabled = aspiration;
    options.use_tt = true;
    return engine.find_best_move(state, options);
}

void test_pvs_and_aspiration_equivalence() {
    const qas::State state = qas::initial_state();
    for (int depth = 1; depth <= 3; ++depth) {
        const auto alpha_beta = configured_search(state, depth, false, false);
        const auto pvs = configured_search(state, depth, true, false);
        const auto aspiration = configured_search(state, depth, true, true);
        check(alpha_beta.score == pvs.score, "PVS matches Alpha-Beta fixed-depth score");
        check(pvs.score == aspiration.score, "aspiration retry preserves full-window score");
        check(is_legal(state, pvs.best_move) && is_legal(state, aspiration.best_move),
              "PVS and aspiration return legal root moves");
    }
}

void test_timeout_fallback() {
    const qas::State state = qas::initial_state();
    qas::AlphaBetaEngine engine(1U << 14U);
    qas::SearchOptions options;
    options.max_depth = 20;
    options.time_limit_ms = 0;
    const auto result = engine.find_best_move(state, options);
    check(result.has_move && is_legal(state, result.best_move),
          "zero-time search still returns safe legal fallback");
    check(result.stats.depth_reached == 0, "incomplete depth is not reported as completed");
}

bool stop_after_depth_one(std::uint64_t searched_nodes, void*) {
    return searched_nodes >= 10;
}

void test_injected_stop_keeps_completed_depth() {
    const qas::State state = qas::initial_state();
    qas::AlphaBetaEngine reference(1U << 14U);
    const auto depth_one = reference.search_fixed_depth(state, 1, false);
    qas::AlphaBetaEngine interrupted(1U << 14U);
    qas::SearchOptions options;
    options.max_depth = 8;
    options.time_limit_ms = 60'000;
    options.use_tt = false;
    options.stop_policy = stop_after_depth_one;
    const auto result = interrupted.find_best_move(state, options);
    check(result.stats.depth_reached == 1, "injected stop rejects the incomplete next depth");
    check(result.best_move == depth_one.best_move && result.score == depth_one.score,
          "injected stop retains the last fully completed depth result");
}

void test_turn_changes_search_key() {
    qas::State early = qas::initial_state();
    qas::State late = early;
    late.turn = 255;
    qas::recompute_hash(late);
    check(early.hash != late.hash, "search keys distinguish remaining draw horizon");
    qas::AlphaBetaEngine engine(1U << 14U);
    const auto result = engine.search_fixed_depth(late, 1, true);
    check(result.score == 0, "all ordinary turn-256 successors evaluate as draw");
}

}  // namespace

int main() {
    test_depths_are_legal_and_non_mutating();
    test_immediate_catch();
    test_tt_preserves_value_and_hits();
    test_pvs_and_aspiration_equivalence();
    test_timeout_fallback();
    test_injected_stop_keeps_completed_depth();
    test_turn_changes_search_key();
    if (failures != 0) {
        std::cerr << failures << " search test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Alpha-Beta tests passed\n";
    return EXIT_SUCCESS;
}

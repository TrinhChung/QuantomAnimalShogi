#include "exact/equivalence.hpp"
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

qas::State two_equivalent_hand_pieces() {
    qas::State state = qas::initial_state();
    for (int piece : {0, 1}) {
        state.board[state.pos[piece]] = -1;
        state.pos[piece] = static_cast<std::uint8_t>(qas::first_hand_slot + piece);
    }
    qas::recompute_hash(state);
    return state;
}

bool legal_move(const qas::State& state, const qas::Move& move) {
    const auto legal = qas::generate_legal_moves(state);
    return std::find(legal.begin(), legal.end(), move) != legal.end();
}

void swap_piece_labels(qas::State& state, int left, int right) {
    const int left_pos = state.pos[left];
    const int right_pos = state.pos[right];
    std::swap(state.pos[left], state.pos[right]);
    std::swap(state.mask[left], state.mask[right]);
    const bool left_owner_north = (state.owner_bits & (1U << left)) != 0;
    const bool right_owner_north = (state.owner_bits & (1U << right)) != 0;
    state.owner_bits &= static_cast<std::uint8_t>(~((1U << left) | (1U << right)));
    if (left_owner_north) state.owner_bits |= static_cast<std::uint8_t>(1U << right);
    if (right_owner_north) state.owner_bits |= static_cast<std::uint8_t>(1U << left);
    if (left_pos < qas::board_size) state.board[left_pos] = static_cast<std::int8_t>(right);
    if (right_pos < qas::board_size) state.board[right_pos] = static_cast<std::int8_t>(left);
    qas::recompute_hash(state);
}

void test_canonical_piece_renaming_and_turn() {
    qas::State first = qas::initial_state();
    qas::State renamed = first;
    swap_piece_labels(renamed, 0, 1);
    check(first.hash != renamed.hash, "physical Zobrist key retains persistent piece labels");
    check(qas::canonical_key(first) == qas::canonical_key(renamed),
          "canonical key removes irrelevant labels within one origin group");

    qas::State later = first;
    ++later.turn;
    qas::recompute_hash(later);
    check(qas::canonical_key(first) != qas::canonical_key(later),
          "canonical key includes turn count");
}

void test_every_skipped_move_matches_representative() {
    const qas::State state = two_equivalent_hand_pieces();
    const auto legal = qas::generate_legal_moves(state);
    const auto classes = qas::generate_equivalent_successor_classes(state, legal);
    check(classes.size() < legal.size(), "equivalent hand drops reduce branching");

    std::size_t multiplicity = 0;
    for (const auto& item : classes) {
        multiplicity += item.multiplicity;
        check(legal_move(state, item.representative), "class keeps legal external representative");
        qas::State successor = state;
        qas::Undo undo;
        check(qas::apply_move(successor, item.representative, undo),
              "representative transition applies");
        check(qas::canonical_key(successor) == item.key,
              "representative produces stored canonical successor");
    }
    check(multiplicity == legal.size(), "every raw legal move belongs to one class");

    for (const qas::Move& move : legal) {
        qas::State successor = state;
        qas::Undo undo;
        check(qas::apply_move(successor, move, undo), "raw legal move applies before canonicalizing");
        const auto key = qas::canonical_key(successor);
        const auto found = std::find_if(classes.begin(), classes.end(), [&key](const auto& item) {
            return item.key == key;
        });
        check(found != classes.end(), "skipped move key matches a representative key");
    }
}

void test_search_equivalence() {
    const qas::State state = two_equivalent_hand_pieces();
    qas::AlphaBetaEngine baseline(1U << 16U);
    qas::AlphaBetaEngine equivalent(1U << 16U);
    qas::SearchOptions baseline_options;
    baseline_options.max_depth = 1;
    baseline_options.time_limit_ms = 60'000;
    qas::SearchOptions equivalent_options = baseline_options;
    qas::enable_successor_equivalence(equivalent_options, 0);
    const auto ab1 = baseline.find_best_move(state, baseline_options);
    const auto leq1 = equivalent.find_best_move(state, equivalent_options);
    check(ab1.score == leq1.score, "depth-1 AB and L_eq root values match");
    check(legal_move(state, leq1.best_move), "L_eq returns an actual legal external move");
    check(leq1.stats.equivalent_successor_moves < leq1.stats.generated_legal_moves,
          "L_eq instrumentation records duplicate reduction");

    baseline.clear_tt();
    equivalent.clear_tt();
    baseline_options.max_depth = 2;
    equivalent_options.max_depth = 2;
    const auto ab2 = baseline.find_best_move(state, baseline_options);
    const auto leq2 = equivalent.find_best_move(state, equivalent_options);
    check(ab2.score == leq2.score, "depth-2 AB and L_eq values match");
}

}  // namespace

int main() {
    test_canonical_piece_renaming_and_turn();
    test_every_skipped_move_matches_representative();
    test_search_equivalence();
    if (failures != 0) {
        std::cerr << failures << " equivalence test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All successor-equivalence tests passed\n";
    return EXIT_SUCCESS;
}

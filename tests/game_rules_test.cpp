#include "rules/game.hpp"
#include "io/protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <random>

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

void move_all_to_hands(qas::State& state) {
    state.board.fill(-1);
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        state.pos[piece] = static_cast<std::uint8_t>(qas::first_hand_slot + piece);
    }
}

qas::State exact_state() {
    qas::State state = qas::initial_state();
    state.mask = {
        qas::bit(qas::Animal::Giraffe), qas::bit(qas::Animal::Chick),
        qas::bit(qas::Animal::Elephant), qas::bit(qas::Animal::Lion),
        qas::bit(qas::Animal::Lion), qas::bit(qas::Animal::Chick),
        qas::bit(qas::Animal::Giraffe), qas::bit(qas::Animal::Elephant)};
    qas::recompute_hash(state);
    return state;
}

void test_initial_and_tables() {
    qas::State state = qas::initial_state();
    std::string error;
    check(qas::validate_state(state, &error), "initial state validates: " + error);
    check(state.hash == qas::zobrist_hash(state), "initial hash is current");
    const auto moves = qas::generate_legal_moves(state);
    check(!moves.empty(), "initial state has direct legal moves");
    check(moves.size() < qas::external_action_count, "generator does not return raw action space");

    const auto& tables = qas::move_tables();
    const auto south_forward = tables.move_possible_mask[0][7][4];
    check(qas::contains(south_forward, qas::Animal::Chick),
          "south forward table contains chick");
    check(qas::contains(south_forward, qas::Animal::Giraffe),
          "orthogonal table contains giraffe");
    check(qas::contains(south_forward, qas::Animal::Lion),
          "adjacent table contains lion");
}

void test_apply_undo_and_external_mapping() {
    qas::State state = qas::initial_state();
    const qas::State original = state;
    const auto moves = qas::generate_legal_moves(state);
    for (const qas::Move& move : moves) {
        const int action = qas::encode_action_move(move);
        check(action >= 0 && action < qas::external_action_count,
              "legal move maps inside 240 actions");
        check(qas::decode_action_move(state, action) == move,
              "external adapter round-trips a legal move");
        qas::Undo undo;
        check(qas::apply_move(state, move, undo), "generated move applies");
        check(state.hash == qas::zobrist_hash(state), "hash is current after apply");
        qas::undo_move(state, undo);
        check(qas::same_state(state, original), "undo restores byte-semantic state and hash");
    }
}

void test_global_propagation() {
    qas::State state = qas::initial_state();
    state.mask[0] = qas::bit(qas::Animal::Lion);
    check(qas::propagate(state), "single fixed lineage propagates");
    for (int piece = 1; piece < 4; ++piece) {
        check(!qas::contains(state.mask[piece], qas::Animal::Lion),
              "quota removes fixed Lion from same origin group");
    }

    qas::State impossible = state;
    impossible.mask[1] = qas::bit(qas::Animal::Lion);
    check(!qas::propagate(impossible), "two fixed Lions in one origin group contradict");

    qas::State hen = qas::initial_state();
    hen.mask[0] = qas::bit(qas::Animal::Hen);
    check(qas::propagate(hen), "Hen is accepted as Chick lineage");
    for (int piece = 1; piece < 4; ++piece) {
        check((hen.mask[piece] & (qas::bit(qas::Animal::Chick) |
                                  qas::bit(qas::Animal::Hen))) == 0,
              "Hen consumes the single CH lineage quota");
    }
}

void test_catch() {
    qas::State state = exact_state();
    move_all_to_hands(state);
    place(state, 0, 4);  // South giraffe
    place(state, 4, 1);  // North confirmed lion
    state.side_to_move = qas::Side::South;
    qas::recompute_hash(state);

    const qas::Move capture{0, 4, 1};
    qas::Undo undo;
    check(qas::apply_move(state, capture, undo), "Catch move applies");
    check(state.terminal == qas::Terminal::Catch && state.winner == 0,
          "capturing confirmed Lion is Catch for mover");
    qas::undo_move(state, undo);
    check(state.terminal == qas::Terminal::None, "Catch is undoable");
}

void test_capture_collapse_and_demotion() {
    qas::State state = exact_state();
    move_all_to_hands(state);
    place(state, 0, 4);
    place(state, 4, 1);
    state.mask[4] = qas::bit(qas::Animal::Chick) | qas::bit(qas::Animal::Lion);
    state.mask[5] = qas::bit(qas::Animal::Chick) | qas::bit(qas::Animal::Lion);
    qas::recompute_hash(state);
    qas::Undo undo;
    check(qas::apply_move(state, qas::Move{0, 4, 1}, undo),
          "capture of unresolved target applies");
    check(state.mask[4] == qas::bit(qas::Animal::Chick),
          "capture removes Lion possibility from unresolved target");
    check(state.mask[5] == qas::bit(qas::Animal::Lion),
          "capture collapse propagates to same-origin peer");
    check(qas::is_hand_position(state.pos[4]) && qas::owned_by(state, 4, qas::Side::South),
          "captured physical piece changes owner and moves to hand");

    qas::State demote = exact_state();
    move_all_to_hands(demote);
    demote.mask[4] = qas::bit(qas::Animal::Hen);
    demote.mask[5] = qas::bit(qas::Animal::Lion);
    place(demote, 0, 4);
    place(demote, 4, 1);
    qas::recompute_hash(demote);
    check(qas::apply_move(demote, qas::Move{0, 4, 1}, undo),
          "capturing Hen applies");
    check(demote.mask[4] == qas::bit(qas::Animal::Chick),
          "captured Hen demotes to Chick");
}

void test_promotion_try_and_draw() {
    qas::State promotion = exact_state();
    move_all_to_hands(promotion);
    place(promotion, 1, 3);
    qas::recompute_hash(promotion);
    qas::Undo undo;
    check(qas::apply_move(promotion, qas::Move{1, 3, 0}, undo),
          "Chick promotes on opponent back rank");
    check(promotion.mask[1] == qas::bit(qas::Animal::Hen), "promotion creates Hen form");

    qas::State try_state = exact_state();
    move_all_to_hands(try_state);
    try_state.mask[0] = qas::bit(qas::Animal::Giraffe) | qas::bit(qas::Animal::Lion);
    try_state.mask[3] = qas::bit(qas::Animal::Giraffe) | qas::bit(qas::Animal::Lion);
    place(try_state, 3, 3);
    qas::recompute_hash(try_state);
    check(qas::apply_move(try_state, qas::Move{3, 3, 0}, undo),
          "Lion candidate can enter back rank");
    check(try_state.terminal == qas::Terminal::Try && try_state.winner == 0,
          "safe piece that can be Lion wins by Try");

    qas::State defended = undo.previous;
    place(defended, 6, 1);  // North giraffe can capture square 0 horizontally.
    qas::recompute_hash(defended);
    check(qas::apply_move(defended, qas::Move{3, 3, 0}, undo),
          "defended Lion-candidate move is legal");
    check(defended.terminal == qas::Terminal::None,
          "Try does not succeed when the candidate can be captured immediately");

    qas::State draw = exact_state();
    draw.turn = 255;
    qas::recompute_hash(draw);
    const auto legal = qas::generate_legal_moves(draw);
    check(!legal.empty(), "turn-255 state has a move");
    check(qas::apply_move(draw, legal.front(), undo), "turn-256 move applies");
    check(draw.terminal == qas::Terminal::Draw, "turn 256 is a draw");
}

void test_turn_is_hashed_and_parse_roundtrip() {
    qas::State first = qas::initial_state();
    qas::State second = first;
    second.turn = 1;
    qas::recompute_hash(second);
    check(first.hash != second.hash, "turn count participates in Zobrist key");

    std::ostringstream text;
    qas::write_state(first, text);
    std::istringstream input(text.str());
    const qas::State parsed = qas::parse_state(input);
    check(qas::same_state(first, parsed), "text state parser round-trips initial state");
}

std::uint64_t perft(qas::State& state, int depth) {
    if (depth == 0 || state.terminal != qas::Terminal::None) return 1;
    std::uint64_t nodes = 0;
    for (const qas::Move& move : qas::generate_legal_moves(state)) {
        qas::Undo undo;
        if (!qas::apply_move(state, move, undo)) continue;
        nodes += perft(state, depth - 1);
        qas::undo_move(state, undo);
    }
    return nodes;
}

void test_seeded_playout_full_unwind() {
    qas::State state = qas::initial_state();
    const qas::State initial = state;
    std::vector<qas::Undo> undo_stack;
    std::mt19937 random(0x514153U);
    for (int ply = 0; ply < 64 && state.terminal == qas::Terminal::None; ++ply) {
        const auto moves = qas::generate_legal_moves(state);
        if (moves.empty()) break;
        std::uniform_int_distribution<std::size_t> select(0, moves.size() - 1);
        qas::Undo undo;
        check(qas::apply_move(state, moves[select(random)], undo),
              "seeded legal playout move applies");
        undo_stack.push_back(undo);
        check(state.hash == qas::zobrist_hash(state),
              "incremental/stored hash matches full recomputation during playout");
        qas::State fixed_point = state;
        check(qas::propagate(fixed_point) && fixed_point.mask == state.mask,
              "propagation is already at a fixed point after each move");
    }
    while (!undo_stack.empty()) {
        qas::undo_move(state, undo_stack.back());
        undo_stack.pop_back();
    }
    check(qas::same_state(state, initial), "full seeded playout unwind restores initial state");
}

void test_initial_perft() {
    qas::State state = qas::initial_state();
    const auto depth_one = perft(state, 1);
    const auto depth_two = perft(state, 2);
    check(depth_one == 9, "initial perft depth 1 is stable: " + std::to_string(depth_one));
    check(depth_two == 79, "initial perft depth 2 is stable: " + std::to_string(depth_two));
}

}  // namespace

int main() {
    test_initial_and_tables();
    test_apply_undo_and_external_mapping();
    test_global_propagation();
    test_catch();
    test_capture_collapse_and_demotion();
    test_promotion_try_and_draw();
    test_turn_is_hashed_and_parse_roundtrip();
    test_seeded_playout_full_unwind();
    test_initial_perft();
    if (failures != 0) {
        std::cerr << failures << " game test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All game-rule tests passed\n";
    return EXIT_SUCCESS;
}

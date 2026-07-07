#include "exact/equivalence.hpp"

#include <algorithm>
#include <tuple>

namespace qas {
namespace {

std::uint16_t piece_tuple(const State& state, int piece) {
    const unsigned canonical_position =
        is_hand_position(state.pos[piece]) ? first_hand_slot : state.pos[piece];
    return static_cast<std::uint16_t>(canonical_position |
                                      (owned_by(state, piece, Side::North) ? 1U << 5U : 0U) |
                                      (static_cast<unsigned>(state.mask[piece]) << 6U));
}

int representative_score(const State& state, const Move& move, const Move& preferred) {
    if (move == preferred) return 2'000'000;
    int score = 0;
    const int captured = state.board[move.to];
    if (captured >= 0) {
        score += 200'000;
        if (state.mask[captured] == bit(Animal::Lion)) score += 1'000'000;
    }
    return score - (static_cast<int>(move.from) * board_size + move.to);
}

void hash_combine(std::size_t& seed, std::uint64_t value) {
    seed ^= static_cast<std::size_t>(value + 0x9e3779b97f4a7c15ULL + (seed << 6U) +
                                    (seed >> 2U));
}

bool has_duplicate_drop_hint(const State& state) {
    for (int left = 0; left < physical_piece_count; ++left) {
        if (!is_hand_position(state.pos[left]) ||
            !owned_by(state, left, state.side_to_move)) continue;
        for (int right = left + 1; right < physical_piece_count; ++right) {
            if (is_hand_position(state.pos[right]) &&
                owned_by(state, right, state.side_to_move) &&
                state.mask[left] == state.mask[right] &&
                originated_from(state, left, Side::North) ==
                    originated_from(state, right, Side::North)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<Move> reduce_successors(const State& state, const std::vector<Move>& legal_moves,
                                    const Move& preferred, std::size_t threshold,
                                    bool& applied) {
    const bool trigger = threshold == 0 || has_duplicate_drop_hint(state);
    applied = legal_moves.size() >= threshold && trigger;
    if (!applied) return legal_moves;
    const auto classes = generate_equivalent_successor_classes(state, legal_moves, preferred);
    std::vector<Move> representatives;
    representatives.reserve(classes.size());
    for (const SuccessorClass& item : classes) representatives.push_back(item.representative);
    return representatives;
}

}  // namespace

bool CanonKey::operator==(const CanonKey& other) const {
    return board == other.board && pieces == other.pieces &&
           side_to_move == other.side_to_move && turn == other.turn &&
           terminal == other.terminal && winner == other.winner;
}

std::size_t CanonKeyHash::operator()(const CanonKey& key) const {
    std::size_t hash = 0x514153U;
    for (std::uint8_t value : key.board) hash_combine(hash, value);
    for (std::uint16_t value : key.pieces) hash_combine(hash, value);
    hash_combine(hash, static_cast<unsigned>(key.side_to_move));
    hash_combine(hash, key.turn);
    hash_combine(hash, static_cast<unsigned>(key.terminal));
    hash_combine(hash, static_cast<unsigned>(key.winner + 1));
    return hash;
}

CanonKey canonical_key(const State& state) {
    CanonKey key;
    for (int square = 0; square < board_size; ++square) {
        const int piece = state.board[square];
        if (piece < 0) continue;
        const unsigned origin = originated_from(state, piece, Side::North) ? 1U : 0U;
        const unsigned owner = owned_by(state, piece, Side::North) ? 1U : 0U;
        key.board[square] = static_cast<std::uint8_t>(1U | (origin << 1U) | (owner << 2U) |
                                                      (state.mask[piece] << 3U));
    }

    for (int origin = 0; origin < 2; ++origin) {
        std::array<std::uint16_t, 4> tuples{};
        int count = 0;
        const Side side = origin == 0 ? Side::South : Side::North;
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            if (originated_from(state, piece, side)) tuples[count++] = piece_tuple(state, piece);
        }
        std::sort(tuples.begin(), tuples.end());
        std::copy(tuples.begin(), tuples.end(), key.pieces.begin() + origin * 4);
    }
    key.side_to_move = state.side_to_move;
    key.turn = state.turn;
    key.terminal = state.terminal;
    key.winner = state.winner;
    return key;
}

std::vector<SuccessorClass> generate_equivalent_successor_classes(
    const State& state, const std::vector<Move>& legal_moves, const Move& preferred) {
    std::vector<SuccessorClass> classes;
    classes.reserve(legal_moves.size());
    for (const Move& move : legal_moves) {
        State successor = state;
        Undo undo;
        if (!apply_move(successor, move, undo)) continue;
        const CanonKey key = canonical_key(successor);
        auto found = std::find_if(classes.begin(), classes.end(), [&key](const SuccessorClass& item) {
            return item.key == key;
        });
        if (found == classes.end()) {
            classes.push_back(SuccessorClass{key, move, 1});
        } else {
            ++found->multiplicity;
            if (representative_score(state, move, preferred) >
                representative_score(state, found->representative, preferred)) {
                found->representative = move;
            }
        }
    }
    return classes;
}

void enable_successor_equivalence(SearchOptions& options, std::size_t threshold) {
    options.successor_reducer = reduce_successors;
    options.reducer_threshold = threshold;
}

}  // namespace qas

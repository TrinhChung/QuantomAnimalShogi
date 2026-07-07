#include "rules/game.hpp"

#include <algorithm>
#include <array>

namespace qas {
namespace {

constexpr int lineage_count = 4;

int row_of(int square) { return square / board_width; }
int col_of(int square) { return square % board_width; }
bool inside(int row, int col) {
    return row >= 0 && row < board_height && col >= 0 && col < board_width;
}

Mask lineage_bit(int lineage) {
    return static_cast<Mask>(1U << static_cast<unsigned>(lineage));
}

Mask lineage_mask(Mask forms) {
    Mask result = 0;
    if ((forms & (bit(Animal::Chick) | bit(Animal::Hen))) != 0) {
        result |= lineage_bit(0);
    }
    if (contains(forms, Animal::Giraffe)) {
        result |= lineage_bit(1);
    }
    if (contains(forms, Animal::Elephant)) {
        result |= lineage_bit(2);
    }
    if (contains(forms, Animal::Lion)) {
        result |= lineage_bit(3);
    }
    return result;
}

Mask forms_for_lineages(Mask forms, Mask lineages) {
    Mask result = 0;
    if ((lineages & lineage_bit(0)) != 0) {
        result |= forms & (bit(Animal::Chick) | bit(Animal::Hen));
    }
    if ((lineages & lineage_bit(1)) != 0) {
        result |= forms & bit(Animal::Giraffe);
    }
    if ((lineages & lineage_bit(2)) != 0) {
        result |= forms & bit(Animal::Elephant);
    }
    if ((lineages & lineage_bit(3)) != 0) {
        result |= forms & bit(Animal::Lion);
    }
    return result;
}

std::uint64_t splitmix64(std::uint64_t& seed) {
    std::uint64_t value = (seed += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

struct ZobristTables {
    std::array<std::array<std::uint64_t, external_source_count>, physical_piece_count> pos{};
    std::array<std::array<std::uint64_t, 32>, physical_piece_count> mask{};
    std::array<std::uint64_t, physical_piece_count> north_owner{};
    std::array<std::uint64_t, physical_piece_count> north_origin{};
    std::array<std::uint64_t, 2> side{};
    std::array<std::uint64_t, 257> turn{};
    std::array<std::uint64_t, 5> terminal{};
    std::array<std::uint64_t, 3> winner{};

    ZobristTables() {
        std::uint64_t seed = 0x5141535f5a4f4252ULL;
        for (auto& piece : pos) {
            for (auto& value : piece) value = splitmix64(seed);
        }
        for (auto& piece : mask) {
            for (auto& value : piece) value = splitmix64(seed);
        }
        for (auto& value : north_owner) value = splitmix64(seed);
        for (auto& value : north_origin) value = splitmix64(seed);
        for (auto& value : side) value = splitmix64(seed);
        for (auto& value : turn) value = splitmix64(seed);
        for (auto& value : terminal) value = splitmix64(seed);
        for (auto& value : winner) value = splitmix64(seed);
    }
};

const ZobristTables& zobrist_tables() {
    static const ZobristTables tables;
    return tables;
}

MoveTables build_move_tables() {
    MoveTables tables;
    const std::array<Animal, 5> forms = all_forms;
    for (int side = 0; side < 2; ++side) {
        const int forward = side == 0 ? -1 : 1;
        for (int square = 0; square < board_size; ++square) {
            const int row = row_of(square);
            const int col = col_of(square);
            for (Animal form : forms) {
                const int type = static_cast<int>(form);
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        bool allowed = false;
                        switch (form) {
                            case Animal::Chick:
                                allowed = dr == forward && dc == 0;
                                break;
                            case Animal::Giraffe:
                                allowed = (dr == 0) != (dc == 0);
                                break;
                            case Animal::Elephant:
                                allowed = dr != 0 && dc != 0;
                                break;
                            case Animal::Lion:
                                allowed = true;
                                break;
                            case Animal::Hen:
                                allowed = dr == forward || (dr == 0 && dc != 0) ||
                                          (dr == -forward && dc == 0);
                                break;
                        }
                        const int to_row = row + dr;
                        const int to_col = col + dc;
                        if (allowed && inside(to_row, to_col)) {
                            const int to = to_row * board_width + to_col;
                            tables.move_table[type][side][square] |=
                                static_cast<std::uint16_t>(1U << to);
                            tables.move_possible_mask[side][square][to] |= bit(form);
                        }
                    }
                }
            }
        }
    }
    for (int mask = 1; mask < 32; ++mask) {
        for (int side = 0; side < 2; ++side) {
            for (int square = 0; square < board_size; ++square) {
                for (Animal form : forms) {
                    if ((mask & bit(form)) != 0) {
                        tables.quantum_move_table[mask][side][square] |=
                            tables.move_table[static_cast<int>(form)][side][square];
                    }
                }
            }
        }
    }
    return tables;
}

bool is_back_rank(int square, Side side) {
    return row_of(square) == (side == Side::South ? 0 : board_height - 1);
}

bool propagate_origin(State& state, Side origin) {
    std::array<int, 4> ids{};
    int count = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (originated_from(state, piece, origin)) {
            if (count >= 4) return false;
            ids[count++] = piece;
        }
    }
    if (count != 4) return false;

    std::array<int, 4> permutation{0, 1, 2, 3};
    std::array<Mask, 4> supported{};
    bool found = false;
    do {
        bool valid = true;
        for (int index = 0; index < 4; ++index) {
            if ((lineage_mask(state.mask[ids[index]]) & lineage_bit(permutation[index])) == 0) {
                valid = false;
                break;
            }
        }
        if (valid) {
            found = true;
            for (int index = 0; index < 4; ++index) {
                supported[index] |= lineage_bit(permutation[index]);
            }
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    if (!found) return false;

    for (int index = 0; index < 4; ++index) {
        const int piece = ids[index];
        state.mask[piece] = forms_for_lineages(state.mask[piece], supported[index]);
        if (state.mask[piece] == 0) return false;
    }
    return true;
}

bool board_consistent(const State& state) {
    std::array<bool, physical_piece_count> seen{};
    std::array<bool, external_source_count - board_size> hand_seen{};
    for (int square = 0; square < board_size; ++square) {
        const int piece = state.board[square];
        if (piece < -1 || piece >= physical_piece_count) return false;
        if (piece >= 0) {
            if (seen[piece] || state.pos[piece] != square) return false;
            seen[piece] = true;
        }
    }
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (state.pos[piece] < board_size) {
            if (!seen[piece] || state.board[state.pos[piece]] != piece) return false;
        } else {
            if (!is_hand_position(state.pos[piece]) || seen[piece]) return false;
            const int hand_index = state.pos[piece] - first_hand_slot;
            if (hand_seen[hand_index]) return false;
            hand_seen[hand_index] = true;
        }
    }
    return true;
}

}  // namespace

Side opposite(Side side) { return side == Side::South ? Side::North : Side::South; }
int side_index(Side side) { return side == Side::South ? 0 : 1; }

bool owned_by(const State& state, int piece, Side side) {
    return (((state.owner_bits >> piece) & 1U) != 0) == (side == Side::North);
}

bool originated_from(const State& state, int piece, Side side) {
    return (((state.origin_bits >> piece) & 1U) != 0) == (side == Side::North);
}

bool is_hand_position(std::uint8_t position) {
    return position >= first_hand_slot && position < external_source_count;
}

const MoveTables& move_tables() {
    static const MoveTables tables = build_move_tables();
    return tables;
}

State initial_state() {
    State state;
    state.board.fill(-1);
    state.pos = {9, 10, 11, 7, 0, 1, 2, 4};
    state.mask.fill(all_mask);
    state.owner_bits = 0xF0U;
    state.origin_bits = 0xF0U;
    state.side_to_move = Side::South;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        state.board[state.pos[piece]] = static_cast<std::int8_t>(piece);
    }
    recompute_hash(state);
    return state;
}

bool validate_state(const State& state, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!board_consistent(state)) return fail("board and piece positions disagree");
    for (Mask mask : state.mask) {
        if (mask == 0 || (mask & ~all_form_mask) != 0) return fail("invalid piece mask");
    }
    int south_origins = 0;
    int north_origins = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (originated_from(state, piece, Side::South)) ++south_origins;
        else ++north_origins;
    }
    if (south_origins != 4 || north_origins != 4) {
        return fail("each origin side must have four persistent pieces");
    }
    State copy = state;
    if (!propagate(copy)) return fail("identity constraints are contradictory");
    if (zobrist_hash(state) != state.hash) return fail("stored Zobrist hash is stale");
    return true;
}

bool propagate(State& state) {
    bool changed = true;
    while (changed) {
        const auto before = state.mask;
        for (Mask mask : state.mask) {
            if (mask == 0 || (mask & ~all_form_mask) != 0) return false;
        }
        if (!propagate_origin(state, Side::South) || !propagate_origin(state, Side::North)) {
            return false;
        }
        changed = state.mask != before;
    }
    return true;
}

std::uint64_t zobrist_hash(const State& state) {
    const auto& tables = zobrist_tables();
    std::uint64_t hash = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        hash ^= tables.pos[piece][state.pos[piece]];
        hash ^= tables.mask[piece][state.mask[piece] & 31U];
        if (((state.owner_bits >> piece) & 1U) != 0) hash ^= tables.north_owner[piece];
        if (((state.origin_bits >> piece) & 1U) != 0) hash ^= tables.north_origin[piece];
    }
    hash ^= tables.side[side_index(state.side_to_move)];
    hash ^= tables.turn[std::min<std::uint16_t>(state.turn, 256)];
    hash ^= tables.terminal[static_cast<std::size_t>(state.terminal)];
    hash ^= tables.winner[static_cast<std::size_t>(state.winner + 1)];
    return hash;
}

void recompute_hash(State& state) { state.hash = zobrist_hash(state); }

bool is_square_attacked(const State& state, int square, Side by_side) {
    if (square < 0 || square >= board_size) return false;
    const auto& tables = move_tables();
    const int side = side_index(by_side);
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!owned_by(state, piece, by_side) || state.pos[piece] >= board_size) continue;
        if ((tables.move_possible_mask[side][state.pos[piece]][square] & state.mask[piece]) != 0) {
            return true;
        }
    }
    return false;
}

namespace {

int unused_hand_slot(const State& state, int excluded_piece) {
    std::array<bool, external_source_count - board_size> used{};
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (piece == excluded_piece || !is_hand_position(state.pos[piece])) continue;
        used[state.pos[piece] - first_hand_slot] = true;
    }
    for (int slot = 0; slot < static_cast<int>(used.size()); ++slot) {
        if (!used[slot]) return first_hand_slot + slot;
    }
    return -1;
}

int lion_candidates_for_origin(const State& state, int target_piece) {
    const Side origin = originated_from(state, target_piece, Side::South)
                            ? Side::South
                            : Side::North;
    int count = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (originated_from(state, piece, origin) &&
            contains(state.mask[piece], Animal::Lion)) {
            ++count;
        }
    }
    return count;
}

bool apply_move_internal(State& state, const Move& move, Undo& undo, bool detect_try);

bool can_capture_piece_immediately(const State& state, int target_piece) {
    if (state.pos[target_piece] >= board_size) return false;
    for (const Move& reply : generate_pseudo_legal_moves(state)) {
        if (reply.to != state.pos[target_piece]) continue;
        State copy = state;
        Undo undo;
        if (apply_move_internal(copy, reply, undo, false)) return true;
    }
    return false;
}

bool apply_move_internal(State& state, const Move& move, Undo& undo, bool detect_try) {
    undo.previous = state;
    auto reject = [&state, &undo]() {
        state = undo.previous;
        return false;
    };
    if (state.terminal != Terminal::None || !move.valid()) return reject();
    const int piece = move.piece;
    const Side mover = state.side_to_move;
    if (!owned_by(state, piece, mover) || state.pos[piece] != move.from) return reject();
    if (state.board[move.to] >= 0 && owned_by(state, state.board[move.to], mover)) return reject();

    bool catch_win = false;
    if (is_hand_position(move.from)) {
        if (state.board[move.to] != -1) return reject();
    } else {
        if (move.from >= board_size || state.board[move.from] != piece) return reject();
        const Mask movers = move_tables().move_possible_mask[side_index(mover)][move.from][move.to];
        state.mask[piece] &= movers;
        if (state.mask[piece] == 0) return reject();

        if (is_back_rank(move.to, mover) && contains(state.mask[piece], Animal::Chick)) {
            state.mask[piece] &= static_cast<Mask>(~bit(Animal::Chick));
            state.mask[piece] |= bit(Animal::Hen);
        }
        state.board[move.from] = -1;
    }

    const int captured = state.board[move.to];
    if (captured >= 0) {
        catch_win = contains(state.mask[captured], Animal::Lion) &&
                    lion_candidates_for_origin(state, captured) == 1;
        const int hand_slot = unused_hand_slot(state, captured);
        if (hand_slot < 0) return reject();
        state.pos[captured] = static_cast<std::uint8_t>(hand_slot);
        if (mover == Side::North) state.owner_bits |= static_cast<std::uint8_t>(1U << captured);
        else state.owner_bits &= static_cast<std::uint8_t>(~(1U << captured));
        Mask captured_mask = state.mask[captured];
        if (contains(captured_mask, Animal::Hen)) {
            captured_mask &= static_cast<Mask>(~bit(Animal::Hen));
            captured_mask |= bit(Animal::Chick);
        }
        if (!catch_win) captured_mask &= static_cast<Mask>(~bit(Animal::Lion));
        state.mask[captured] = captured_mask;
    }

    state.board[move.to] = static_cast<std::int8_t>(piece);
    state.pos[piece] = move.to;
    ++state.turn;
    state.side_to_move = opposite(mover);

    if (!propagate(state)) return reject();

    if (catch_win) {
        state.terminal = Terminal::Catch;
        state.winner = static_cast<std::int8_t>(side_index(mover));
    } else if (detect_try && contains(state.mask[piece], Animal::Lion) &&
               is_back_rank(move.to, mover) &&
               !can_capture_piece_immediately(state, piece)) {
        state.terminal = Terminal::Try;
        state.winner = static_cast<std::int8_t>(side_index(mover));
    } else if (state.turn >= 256) {
        state.terminal = Terminal::Draw;
        state.winner = -1;
    }
    recompute_hash(state);
    return true;
}

}  // namespace

bool apply_move(State& state, const Move& move, Undo& undo) {
    return apply_move_internal(state, move, undo, true);
}

void undo_move(State& state, const Undo& undo) { state = undo.previous; }

void generate_pseudo_legal_moves(const State& state, std::vector<Move>& moves) {
    moves.clear();
    if (state.terminal != Terminal::None) return;
    const Side side = state.side_to_move;
    const auto& tables = move_tables();
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!owned_by(state, piece, side)) continue;
        const int from = state.pos[piece];
        if (is_hand_position(static_cast<std::uint8_t>(from))) {
            for (int to = 0; to < board_size; ++to) {
                if (state.board[to] == -1) {
                    moves.push_back(Move{static_cast<std::uint8_t>(piece),
                                         static_cast<std::uint8_t>(from),
                                         static_cast<std::uint8_t>(to)});
                }
            }
            continue;
        }
        std::uint16_t destinations = tables.quantum_move_table[state.mask[piece]][side_index(side)][from];
        for (int to = 0; to < board_size; ++to) {
            if ((destinations & (1U << to)) == 0) continue;
            const int occupant = state.board[to];
            if (occupant >= 0 && owned_by(state, occupant, side)) continue;
            moves.push_back(Move{static_cast<std::uint8_t>(piece), static_cast<std::uint8_t>(from),
                                 static_cast<std::uint8_t>(to)});
        }
    }
}

std::vector<Move> generate_pseudo_legal_moves(const State& state) {
    std::vector<Move> moves;
    moves.reserve(128);
    generate_pseudo_legal_moves(state, moves);
    return moves;
}

void generate_legal_moves(const State& state, std::vector<Move>& legal,
                          std::vector<Move>& candidates) {
    generate_pseudo_legal_moves(state, candidates);
    legal.clear();
    legal.reserve(candidates.size());
    State copy = state;
    for (const Move& move : candidates) {
        Undo undo;
        if (apply_move(copy, move, undo)) legal.push_back(move);
        copy = state;
    }
}

std::vector<Move> generate_legal_moves(const State& state) {
    std::vector<Move> candidates;
    std::vector<Move> legal;
    candidates.reserve(128);
    legal.reserve(128);
    generate_legal_moves(state, legal, candidates);
    return legal;
}

bool is_immediate_winning_move(const State& state, const Move& move) {
    State copy = state;
    Undo undo;
    if (!apply_move(copy, move, undo)) return false;
    return copy.terminal == Terminal::Catch || copy.terminal == Terminal::Try;
}

}  // namespace qas

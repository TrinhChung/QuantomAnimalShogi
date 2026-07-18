#include "rules/game.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include "core/timing.hpp"

namespace qas {
namespace {

constexpr int kLineageCount = static_cast<int>(all_animals.size());
constexpr int kLineageKeyBitCount = kLineageCount * kLineageCount;
constexpr std::size_t kLineageTableSize = std::size_t{1} << kLineageKeyBitCount;
constexpr std::size_t kTurnHashCount = static_cast<std::size_t>(kTurnLimit) + 1;
constexpr std::size_t kTerminalHashCount = static_cast<std::size_t>(Terminal::Illegal) + 1;
constexpr std::size_t kWinnerHashCount = kSideCount + 1;
constexpr std::array<std::uint8_t, physical_piece_count> kInitialPiecePositions{
    9, 10, 11, 7, 0, 1, 2, 4};
constexpr std::uint8_t kInitialNorthPieceBits =
    static_cast<std::uint8_t>(((1U << kPiecesPerOrigin) - 1U) << kPiecesPerOrigin);

/// @brief Converts a board square index to its row.
/// @param square Zero-based board square.
/// @return Zero-based row index.
int row_of(int square) {
    return square / board_width;
}
/// @brief Converts a board square index to its column.
/// @param square Zero-based board square.
/// @return Zero-based column index.
int col_of(int square) {
    return square % board_width;
}
/// @brief Tests whether board coordinates lie inside the 3-by-4 board.
/// @param row Candidate row.
/// @param col Candidate column.
/// @return `true` for valid board coordinates.
bool inside(int row, int col) {
    return row >= 0 && row < board_height && col >= 0 && col < board_width;
}

/// @brief Returns the compact bit assigned to a CH/G/E/L lineage.
/// @param lineage Zero-based lineage index.
/// @return Single lineage bit.
Mask lineage_bit(int lineage) {
    return static_cast<Mask>(1U << static_cast<unsigned>(lineage));
}

/// @brief Maps animal-form possibilities to CH/G/E/L lineage possibilities.
/// @param forms Animal-form mask.
/// @return Four-bit lineage mask.
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

/// @brief Retains forms whose lineage remains supported.
/// @param forms Original animal-form mask.
/// @param lineages Supported CH/G/E/L lineage mask.
/// @return Intersection expressed as animal forms.
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

struct LineagePropagationEntry {
    std::array<Mask, kLineageCount> supported{};
    bool valid{false};
};

using OriginPieceIds = std::array<int, kLineageCount>;

/// @brief Collects the four physical pieces belonging to one immutable origin side.
/// @param state Source state.
/// @param origin Origin side to collect.
/// @param pieces Receives physical piece indices in ascending order.
/// @return `true` only when exactly four pieces belong to the origin.
bool collect_origin_pieces(const State& state, Side origin, OriginPieceIds& pieces) {
    int count = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!originated_from(state, piece, origin))
            continue;
        if (count >= kLineageCount)
            return false;
        pieces[count++] = piece;
    }
    return count == kLineageCount;
}

/// @brief Intersects origin-piece masks with lineage support.
/// @param state State whose masks are reduced.
/// @param pieces Four physical pieces of one origin.
/// @param supported Supported lineage bits for each piece.
/// @return `false` if any piece loses every possible form.
bool apply_supported_lineages(State& state,
                              const OriginPieceIds& pieces,
                              const std::array<Mask, kLineageCount>& supported) {
    for (int index = 0; index < kLineageCount; ++index) {
        const int piece = pieces[index];
        state.mask[piece] = forms_for_lineages(state.mask[piece], supported[index]);
        if (state.mask[piece] == 0)
            return false;
    }
    return true;
}

/// @brief Packs four lineage masks into a lookup-table index.
/// @param lineages Four four-bit lineage masks.
/// @return Sixteen-bit table index.
std::size_t lineage_key(const std::array<Mask, kLineageCount>& lineages) {
    std::size_t key = 0;
    for (int index = 0; index < kLineageCount; ++index) {
        key |= static_cast<std::size_t>(lineages[index] & all_mask)
               << static_cast<unsigned>(index * kLineageCount);
    }
    return key;
}

/// @brief Exhaustively computes valid assignment support for one lineage tuple.
/// @param lineages Candidate lineages for four pieces.
/// @return Validity and supported lineage bits per piece.
LineagePropagationEntry build_lineage_entry(const std::array<Mask, kLineageCount>& lineages) {
    LineagePropagationEntry entry;
    std::array<int, kLineageCount> permutation{0, 1, 2, 3};
    do {
        bool valid = true;
        for (int index = 0; index < kLineageCount; ++index) {
            if ((lineages[index] & lineage_bit(permutation[index])) == 0) {
                valid = false;
                break;
            }
        }
        if (valid) {
            entry.valid = true;
            for (int index = 0; index < kLineageCount; ++index) {
                entry.supported[index] |= lineage_bit(permutation[index]);
            }
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    return entry;
}

/// @brief Builds the complete lineage-propagation lookup table.
/// @return Entry for every packed four-piece lineage tuple.
std::array<LineagePropagationEntry, kLineageTableSize> build_lineage_table() {
    std::array<LineagePropagationEntry, kLineageTableSize> table{};
    for (std::size_t key = 0; key < table.size(); ++key) {
        std::array<Mask, kLineageCount> lineages{};
        for (int index = 0; index < kLineageCount; ++index) {
            lineages[index] =
                static_cast<Mask>((key >> static_cast<unsigned>(index * kLineageCount)) & all_mask);
        }
        table[key] = build_lineage_entry(lineages);
    }
    return table;
}

/// @brief Returns the immutable lazily initialized lineage lookup table.
/// @return Process-wide lineage table.
const std::array<LineagePropagationEntry, kLineageTableSize>& lineage_table() {
    static const auto table = build_lineage_table();
    return table;
}

/// @brief Advances a deterministic SplitMix64 stream for Zobrist material.
/// @param seed Mutable generator state.
/// @return Next pseudo-random 64-bit value.
std::uint64_t splitmix64(std::uint64_t& seed) {
    std::uint64_t value = (seed += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

struct ZobristTables {
    std::array<std::array<std::uint64_t, external_source_count>, physical_piece_count> pos{};
    std::array<std::array<std::uint64_t, kAnimalFormMaskCount>, physical_piece_count> mask{};
    std::array<std::uint64_t, physical_piece_count> north_owner{};
    std::array<std::uint64_t, physical_piece_count> north_origin{};
    std::array<std::uint64_t, kSideCount> side{};
    std::array<std::uint64_t, kTurnHashCount> turn{};
    std::array<std::uint64_t, kTerminalHashCount> terminal{};
    std::array<std::uint64_t, kWinnerHashCount> winner{};

    /// @brief Deterministically initializes every Zobrist field table.
    ZobristTables() {
        std::uint64_t seed = 0x5141535f5a4f4252ULL;
        for (auto& piece : pos) {
            for (auto& value : piece)
                value = splitmix64(seed);
        }
        for (auto& piece : mask) {
            for (auto& value : piece)
                value = splitmix64(seed);
        }
        for (auto& value : north_owner)
            value = splitmix64(seed);
        for (auto& value : north_origin)
            value = splitmix64(seed);
        for (auto& value : side)
            value = splitmix64(seed);
        for (auto& value : turn)
            value = splitmix64(seed);
        for (auto& value : terminal)
            value = splitmix64(seed);
        for (auto& value : winner)
            value = splitmix64(seed);
    }
};

/// @brief Returns immutable lazily initialized Zobrist tables.
/// @return Process-wide Zobrist material.
const ZobristTables& zobrist_tables() {
    static const ZobristTables tables;
    return tables;
}

/// @brief Precomputes per-form, quantum-union and move-capability tables.
/// @return Complete immutable movement table data.
MoveTables build_move_tables() {
    MoveTables tables;
    for (int side = 0; side < static_cast<int>(kSideCount); ++side) {
        const int forward = side == 0 ? -1 : 1;
        for (int square = 0; square < board_size; ++square) {
            const int row = row_of(square);
            const int col = col_of(square);
            for (Animal form : all_forms) {
                const int type = static_cast<int>(form);
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (dr == 0 && dc == 0)
                            continue;
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
    for (std::size_t mask = 1; mask < kAnimalFormMaskCount; ++mask) {
        for (int side = 0; side < static_cast<int>(kSideCount); ++side) {
            for (int square = 0; square < board_size; ++square) {
                for (Animal form : all_forms) {
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

/// @brief Tests whether a square is the promotion/Try rank for a side.
/// @param square Board square.
/// @param side Moving side.
/// @return `true` when the square lies on the opponent-facing back rank.
bool is_back_rank(int square, Side side) {
    return row_of(square) == (side == Side::South ? 0 : board_height - 1);
}

/// @brief Propagates one origin group by exhaustive lineage permutations.
/// @param state State whose masks are reduced.
/// @param origin Immutable origin group to process.
/// @return `false` when the origin constraints are contradictory.
bool propagate_origin_reference(State& state, Side origin) {
    OriginPieceIds pieces{};
    if (!collect_origin_pieces(state, origin, pieces))
        return false;

    std::array<int, kLineageCount> permutation{0, 1, 2, 3};
    std::array<Mask, kLineageCount> supported{};
    bool found = false;
    do {
        bool valid = true;
        for (int index = 0; index < kLineageCount; ++index) {
            if ((lineage_mask(state.mask[pieces[index]]) & lineage_bit(permutation[index])) == 0) {
                valid = false;
                break;
            }
        }
        if (valid) {
            found = true;
            for (int index = 0; index < kLineageCount; ++index) {
                supported[index] |= lineage_bit(permutation[index]);
            }
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    if (!found)
        return false;
    return apply_supported_lineages(state, pieces, supported);
}

/// @brief Propagates one origin group through the precomputed lookup table.
/// @param state State whose masks are reduced.
/// @param origin Immutable origin group to process.
/// @return `false` when the origin constraints are contradictory.
bool propagate_origin_lut(State& state, Side origin) {
    OriginPieceIds pieces{};
    if (!collect_origin_pieces(state, origin, pieces))
        return false;

    std::array<Mask, kLineageCount> lineages{};
    for (int index = 0; index < kLineageCount; ++index)
        lineages[index] = lineage_mask(state.mask[pieces[index]]);

    const auto& entry = lineage_table()[lineage_key(lineages)];
    if (!entry.valid)
        return false;
    return apply_supported_lineages(state, pieces, entry.supported);
}

/// @brief Verifies board, piece-position and unique hand-slot consistency.
/// @param state State to inspect.
/// @return `true` when board and position representations agree.
bool board_consistent(const State& state) {
    std::array<bool, physical_piece_count> seen{};
    std::array<bool, external_source_count - board_size> hand_seen{};
    for (int square = 0; square < board_size; ++square) {
        const int piece = state.board[square];
        if (piece < -1 || piece >= physical_piece_count)
            return false;
        if (piece >= 0) {
            if (seen[piece] || state.pos[piece] != square)
                return false;
            seen[piece] = true;
        }
    }
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (state.pos[piece] < board_size) {
            if (!seen[piece] || state.board[state.pos[piece]] != piece)
                return false;
        } else {
            if (!is_hand_position(state.pos[piece]) || seen[piece])
                return false;
            const int hand_index = state.pos[piece] - first_hand_slot;
            if (hand_seen[hand_index])
                return false;
            hand_seen[hand_index] = true;
        }
    }
    return true;
}

}  // namespace

Side opposite(Side side) {
    return side == Side::South ? Side::North : Side::South;
}
int side_index(Side side) {
    return side == Side::South ? 0 : 1;
}

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
    state.board.fill(kEmptyBoardSquare);
    state.pos = kInitialPiecePositions;
    state.mask.fill(all_mask);
    state.owner_bits = kInitialNorthPieceBits;
    state.origin_bits = kInitialNorthPieceBits;
    state.side_to_move = Side::South;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        state.board[state.pos[piece]] = static_cast<std::int8_t>(piece);
    }
    recompute_hash(state);
    return state;
}

bool validate_state(const State& state, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error != nullptr)
            *error = message;
        return false;
    };
    if (!board_consistent(state))
        return fail("board and piece positions disagree");
    for (Mask mask : state.mask) {
        if (mask == 0 || (mask & ~all_form_mask) != 0)
            return fail("invalid piece mask");
    }
    int south_origins = 0;
    int north_origins = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (originated_from(state, piece, Side::South))
            ++south_origins;
        else
            ++north_origins;
    }
    if (south_origins != kPiecesPerOrigin || north_origins != kPiecesPerOrigin) {
        return fail("each origin side must have four persistent pieces");
    }
    State copy = state;
    if (!propagate(copy))
        return fail("identity constraints are contradictory");
    if (zobrist_hash(state) != state.hash)
        return fail("stored Zobrist hash is stale");
    return true;
}

namespace {

/// @brief Runs whole-state propagation until no mask changes.
/// @param state State whose masks are reduced.
/// @param mode Origin-propagation implementation.
/// @param iteration_count Optional counter incremented for each fixed-point pass.
/// @return `false` when masks are invalid or contradictory.
bool propagate_impl(State& state, PropagationMode mode, std::uint64_t* iteration_count) {
    bool changed = true;
    while (changed) {
        if (iteration_count != nullptr)
            ++*iteration_count;
        const auto before = state.mask;
        for (Mask mask : state.mask) {
            if (mask == 0 || (mask & ~all_form_mask) != 0)
                return false;
        }
        const bool south_ok = mode == PropagationMode::PermutationReference
                                  ? propagate_origin_reference(state, Side::South)
                                  : propagate_origin_lut(state, Side::South);
        const bool north_ok = mode == PropagationMode::PermutationReference
                                  ? propagate_origin_reference(state, Side::North)
                                  : propagate_origin_lut(state, Side::North);
        if (!south_ok || !north_ok) {
            return false;
        }
        changed = state.mask != before;
    }
    return true;
}

}  // namespace

bool propagate(State& state, PropagationMode mode) {
    return propagate_impl(state, mode, nullptr);
}

std::uint64_t zobrist_hash(const State& state) {
    const auto& tables = zobrist_tables();
    std::uint64_t hash = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        hash ^= tables.pos[piece][state.pos[piece]];
        hash ^= tables.mask[piece][state.mask[piece] & all_form_mask];
        if (((state.owner_bits >> piece) & 1U) != 0)
            hash ^= tables.north_owner[piece];
        if (((state.origin_bits >> piece) & 1U) != 0)
            hash ^= tables.north_origin[piece];
    }
    hash ^= tables.side[side_index(state.side_to_move)];
    hash ^= tables.turn[std::min(state.turn, kTurnLimit)];
    hash ^= tables.terminal[static_cast<std::size_t>(state.terminal)];
    hash ^= tables.winner[static_cast<std::size_t>(state.winner + 1)];
    return hash;
}

void recompute_hash(State& state) {
    state.hash = zobrist_hash(state);
}

bool is_square_attacked(const State& state, int square, Side by_side) {
    if (square < 0 || square >= board_size)
        return false;
    const auto& tables = move_tables();
    const int side = side_index(by_side);
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!owned_by(state, piece, by_side) || state.pos[piece] >= board_size)
            continue;
        if ((tables.move_possible_mask[side][state.pos[piece]][square] & state.mask[piece]) != 0) {
            return true;
        }
    }
    return false;
}

namespace {

/// @brief Finds the lowest unused external hand slot.
/// @param state Source state.
/// @param excluded_piece Piece omitted while scanning, typically the captured piece.
/// @return Hand-slot position, or -1 when every slot is occupied.
int unused_hand_slot(const State& state, int excluded_piece) {
    std::array<bool, external_source_count - board_size> used{};
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (piece == excluded_piece || !is_hand_position(state.pos[piece]))
            continue;
        used[state.pos[piece] - first_hand_slot] = true;
    }
    for (int slot = 0; slot < static_cast<int>(used.size()); ++slot) {
        if (!used[slot])
            return first_hand_slot + slot;
    }
    return -1;
}

/// @brief Counts Lion-capable pieces sharing a target piece's immutable origin.
/// @param state Source state.
/// @param target_piece Piece whose origin selects the group.
/// @return Number of Lion candidates in that origin group.
int lion_candidates_for_origin(const State& state, int target_piece) {
    const Side origin =
        originated_from(state, target_piece, Side::South) ? Side::South : Side::North;
    int count = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (originated_from(state, piece, origin) && contains(state.mask[piece], Animal::Lion)) {
            ++count;
        }
    }
    return count;
}

class ApplyMoveMetricsRecorder {
   public:
    /// @brief Starts optional whole-transition timing and call accounting.
    /// @param metrics Optional metrics sink; null disables instrumentation.
    explicit ApplyMoveMetricsRecorder(RuleMetrics* metrics) : metrics_(metrics) {
        if (metrics_ != nullptr) {
            ++metrics_->apply_move_internal_calls;
            start_ = std::chrono::steady_clock::now();
        }
    }

    /// @brief Records transition completion and returns the supplied status.
    /// @param success Whether the transition succeeded.
    /// @return The unchanged success value.
    bool finish(bool success) {
        if (metrics_ != nullptr) {
            if (success)
                ++metrics_->apply_move_internal_successes;
            else
                ++metrics_->apply_move_internal_failures;
            metrics_->apply_move_internal_ms += elapsed_milliseconds(start_);
        }
        return success;
    }

   private:
    RuleMetrics* metrics_{nullptr};
    std::chrono::steady_clock::time_point start_{};
};

/// @brief Checks nonmutating terminal, range, ownership and destination preconditions.
/// @param state Source state.
/// @param move Candidate move.
/// @return `true` when mutation may begin.
bool move_preconditions_hold(const State& state, const Move& move) {
    if (state.terminal != Terminal::None || !move.valid())
        return false;
    const int piece = move.piece;
    const Side mover = state.side_to_move;
    if (!owned_by(state, piece, mover) || state.pos[piece] != move.from)
        return false;
    return state.board[move.to] < 0 || !owned_by(state, state.board[move.to], mover);
}

/// @brief Applies movement collapse, promotion and source-square removal.
/// @param state State being transitioned.
/// @param move Candidate move.
/// @param mover Side making the move.
/// @return `false` when the source or collapsed identity is invalid.
bool apply_move_origin_effects(State& state, const Move& move, Side mover) {
    if (is_hand_position(move.from))
        return state.board[move.to] == kEmptyBoardSquare;

    const int piece = move.piece;
    if (move.from >= board_size || state.board[move.from] != piece)
        return false;
    const Mask movers = move_tables().move_possible_mask[side_index(mover)][move.from][move.to];
    state.mask[piece] &= movers;
    if (state.mask[piece] == 0)
        return false;

    if (is_back_rank(move.to, mover) && contains(state.mask[piece], Animal::Chick)) {
        state.mask[piece] &= static_cast<Mask>(~bit(Animal::Chick));
        state.mask[piece] |= bit(Animal::Hen);
    }
    state.board[move.from] = -1;
    return true;
}

/// @brief Transfers and collapses a piece occupying the move destination.
/// @param state State being transitioned.
/// @param move Applied move.
/// @param mover Side taking ownership of a captured piece.
/// @param catch_win Receives whether the captured piece was the final Lion candidate.
/// @return `false` only when no hand slot is available for a capture.
bool capture_destination_piece(State& state, const Move& move, Side mover, bool& catch_win) {
    const int captured = state.board[move.to];
    if (captured < 0)
        return true;

    // Catch is determined from the identity mask before capture removes Lion from that mask.
    catch_win = contains(state.mask[captured], Animal::Lion) &&
                lion_candidates_for_origin(state, captured) == 1;
    const int hand_slot = unused_hand_slot(state, captured);
    if (hand_slot < 0)
        return false;
    state.pos[captured] = static_cast<std::uint8_t>(hand_slot);
    if (mover == Side::North)
        state.owner_bits |= static_cast<std::uint8_t>(1U << captured);
    else
        state.owner_bits &= static_cast<std::uint8_t>(~(1U << captured));

    Mask captured_mask = state.mask[captured];
    if (contains(captured_mask, Animal::Hen)) {
        captured_mask &= static_cast<Mask>(~bit(Animal::Hen));
        captured_mask |= bit(Animal::Chick);
    }
    if (!catch_win)
        captured_mask &= static_cast<Mask>(~bit(Animal::Lion));
    state.mask[captured] = captured_mask;
    return true;
}

/// @brief Places the mover, increments the turn and switches side to move.
/// @param state State being transitioned.
/// @param move Applied move.
/// @param mover Side that made the move.
void complete_board_transition(State& state, const Move& move, Side mover) {
    state.board[move.to] = static_cast<std::int8_t>(move.piece);
    state.pos[move.piece] = move.to;
    ++state.turn;
    state.side_to_move = opposite(mover);
}

/// @brief Propagates a transitioned state with optional timing metrics.
/// @param state State whose identity masks are reduced.
/// @param mode Propagation implementation.
/// @param metrics Optional metrics sink.
/// @return `false` when propagation finds a contradiction.
bool propagate_transition(State& state, PropagationMode mode, RuleMetrics* metrics) {
    if (metrics == nullptr)
        return propagate(state, mode);

    const auto propagation_start = std::chrono::steady_clock::now();
    std::uint64_t propagation_iterations = 0;
    const bool succeeded = propagate_impl(state, mode, &propagation_iterations);
    const auto propagation_end = std::chrono::steady_clock::now();
    ++metrics->propagation_calls;
    metrics->propagation_iterations += propagation_iterations;
    metrics->propagation_ms += elapsed_milliseconds(propagation_start, propagation_end);
    return succeeded;
}

/// @brief Executes the ordered move-transition pipeline.
/// @param state State to mutate and restore on failure.
/// @param move Candidate move.
/// @param undo Receives the complete input state.
/// @param mode Propagation implementation.
/// @param detect_try Whether Try terminal detection is enabled.
/// @param metrics Optional rule metrics sink.
/// @return `true` when the complete transition succeeds.
bool apply_move_internal(State& state,
                         const Move& move,
                         Undo& undo,
                         PropagationMode mode,
                         bool detect_try,
                         RuleMetrics* metrics);

/// @brief Tests whether the side to move has a legal immediate capture of a piece.
/// @param state Source state.
/// @param target_piece Physical piece that must be captured.
/// @param mode Propagation implementation used for replies.
/// @param metrics Optional metrics sink for reply transitions.
/// @return `true` when at least one legal reply captures the target.
bool can_capture_piece_immediately(const State& state,
                                   int target_piece,
                                   PropagationMode mode,
                                   RuleMetrics* metrics) {
    if (state.pos[target_piece] >= board_size)
        return false;
    for (const Move& reply : generate_pseudo_legal_moves(state)) {
        if (reply.to != state.pos[target_piece])
            continue;
        State copy = state;
        Undo undo;
        if (apply_move_internal(copy, reply, undo, mode, false, metrics))
            return true;
    }
    return false;
}

/// @brief Applies official Catch, Try and draw terminal priority.
/// @param state Transitioned and propagated state to update.
/// @param move Applied move.
/// @param mover Side that made the move.
/// @param catch_win Whether capture removed the final Lion candidate.
/// @param detect_try Whether Try detection is enabled.
/// @param mode Propagation implementation used for defensive replies.
/// @param metrics Optional metrics sink for defensive replies.
void update_terminal_status(State& state,
                            const Move& move,
                            Side mover,
                            bool catch_win,
                            bool detect_try,
                            PropagationMode mode,
                            RuleMetrics* metrics) {
    // Official terminal priority is Catch, then Try, then the turn-limit draw.
    if (catch_win) {
        state.terminal = Terminal::Catch;
        state.winner = static_cast<std::int8_t>(side_index(mover));
    } else if (detect_try && contains(state.mask[move.piece], Animal::Lion) &&
               is_back_rank(move.to, mover) &&
               !can_capture_piece_immediately(state, move.piece, mode, metrics)) {
        state.terminal = Terminal::Try;
        state.winner = static_cast<std::int8_t>(side_index(mover));
    } else if (state.turn >= kTurnLimit) {
        state.terminal = Terminal::Draw;
        state.winner = kNoWinner;
    }
}

/// @brief Recomputes the transitioned-state hash with optional timing metrics.
/// @param state State whose stored hash is updated.
/// @param metrics Optional metrics sink.
void recompute_transition_hash(State& state, RuleMetrics* metrics) {
    if (metrics == nullptr) {
        recompute_hash(state);
        return;
    }

    const auto hash_begin = std::chrono::steady_clock::now();
    recompute_hash(state);
    const auto hash_end = std::chrono::steady_clock::now();
    ++metrics->hash_recompute_calls;
    metrics->hash_recompute_ms += elapsed_milliseconds(hash_begin, hash_end);
}

bool apply_move_internal(State& state,
                         const Move& move,
                         Undo& undo,
                         PropagationMode mode,
                         bool detect_try,
                         RuleMetrics* metrics) {
    ApplyMoveMetricsRecorder metrics_recorder(metrics);
    undo.previous = state;
    auto reject = [&state, &undo, &metrics_recorder]() {
        state = undo.previous;
        return metrics_recorder.finish(false);
    };
    if (!move_preconditions_hold(state, move))
        return reject();

    const Side mover = state.side_to_move;
    // Keep these phases ordered: identity collapse/promotion, capture, placement, propagation,
    // terminal detection, and finally hashing.
    if (!apply_move_origin_effects(state, move, mover))
        return reject();
    bool catch_win = false;
    if (!capture_destination_piece(state, move, mover, catch_win))
        return reject();
    complete_board_transition(state, move, mover);
    if (!propagate_transition(state, mode, metrics))
        return reject();
    update_terminal_status(state, move, mover, catch_win, detect_try, mode, metrics);
    recompute_transition_hash(state, metrics);
    return metrics_recorder.finish(true);
}

struct LegalMoveGenerationContext {
    PropagationMode propagation_mode{PropagationMode::LineageLut};
    RuleMetrics* metrics{nullptr};
};

/// @brief Generates pseudo-legal candidates with optional profiling.
/// @param state Source state.
/// @param candidates Buffer cleared and filled with candidates.
/// @param metrics Optional metrics sink.
void generate_candidates(const State& state, std::vector<Move>& candidates, RuleMetrics* metrics) {
    if (metrics == nullptr) {
        generate_pseudo_legal_moves(state, candidates);
        return;
    }

    const auto begin = std::chrono::steady_clock::now();
    generate_pseudo_legal_moves(state, candidates);
    const auto end = std::chrono::steady_clock::now();
    ++metrics->pseudo_move_generation_calls;
    metrics->pseudo_moves_generated += candidates.size();
    metrics->pseudo_move_generation_ms += elapsed_milliseconds(begin, end);
}

/// @brief Filters pseudo-legal candidates through complete transitions.
/// @param state Source state.
/// @param legal Buffer cleared and filled with legal moves.
/// @param candidates Pseudo-legal candidates to test.
/// @param context Propagation and optional instrumentation context.
void filter_legal_candidates(const State& state,
                             std::vector<Move>& legal,
                             const std::vector<Move>& candidates,
                             const LegalMoveGenerationContext& context) {
    std::chrono::steady_clock::time_point begin{};
    if (context.metrics != nullptr)
        begin = std::chrono::steady_clock::now();

    legal.clear();
    legal.reserve(candidates.size());
    State copy = state;
    for (const Move& move : candidates) {
        Undo undo;
        if (apply_move_internal(
                copy, move, undo, context.propagation_mode, true, context.metrics)) {
            legal.push_back(move);
        } else if (context.metrics != nullptr) {
            ++context.metrics->pseudo_moves_rejected;
        }
        copy = state;
    }

    if (context.metrics != nullptr) {
        const auto end = std::chrono::steady_clock::now();
        ++context.metrics->legal_filter_calls;
        context.metrics->legal_moves_generated += legal.size();
        context.metrics->legal_filter_ms += elapsed_milliseconds(begin, end);
    }
}

/// @brief Runs the shared candidate-generation and legal-filter pipeline.
/// @param state Source state.
/// @param legal Buffer receiving legal moves.
/// @param candidates Reusable pseudo-legal candidate buffer.
/// @param context Propagation and optional instrumentation context.
void generate_legal_moves_impl(const State& state,
                               std::vector<Move>& legal,
                               std::vector<Move>& candidates,
                               const LegalMoveGenerationContext& context) {
    generate_candidates(state, candidates, context.metrics);
    filter_legal_candidates(state, legal, candidates, context);
}

}  // namespace

bool apply_move(State& state, const Move& move, Undo& undo, PropagationMode mode) {
    return apply_move_internal(state, move, undo, mode, true, nullptr);
}

bool apply_move_profiled(
    State& state, const Move& move, Undo& undo, RuleMetrics& metrics, PropagationMode mode) {
    return apply_move_internal(state, move, undo, mode, true, &metrics);
}

void undo_move(State& state, const Undo& undo) {
    state = undo.previous;
}

void generate_pseudo_legal_moves(const State& state, std::vector<Move>& moves) {
    moves.clear();
    if (state.terminal != Terminal::None)
        return;
    const Side side = state.side_to_move;
    const auto& tables = move_tables();
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!owned_by(state, piece, side))
            continue;
        const int from = state.pos[piece];
        if (is_hand_position(static_cast<std::uint8_t>(from))) {
            for (int to = 0; to < board_size; ++to) {
                if (state.board[to] == kEmptyBoardSquare) {
                    moves.push_back(Move{static_cast<std::uint8_t>(piece),
                                         static_cast<std::uint8_t>(from),
                                         static_cast<std::uint8_t>(to)});
                }
            }
            continue;
        }
        std::uint16_t destinations =
            tables.quantum_move_table[state.mask[piece]][side_index(side)][from];
        for (int to = 0; to < board_size; ++to) {
            if ((destinations & (1U << to)) == 0)
                continue;
            const int occupant = state.board[to];
            if (occupant >= 0 && owned_by(state, occupant, side))
                continue;
            moves.push_back(Move{static_cast<std::uint8_t>(piece),
                                 static_cast<std::uint8_t>(from),
                                 static_cast<std::uint8_t>(to)});
        }
    }
}

std::vector<Move> generate_pseudo_legal_moves(const State& state) {
    std::vector<Move> moves;
    moves.reserve(kMoveBufferCapacity);
    generate_pseudo_legal_moves(state, moves);
    return moves;
}

void generate_legal_moves(const State& state,
                          std::vector<Move>& legal,
                          std::vector<Move>& candidates,
                          PropagationMode mode) {
    generate_legal_moves_impl(state, legal, candidates, LegalMoveGenerationContext{mode, nullptr});
}

std::vector<Move> generate_legal_moves(const State& state, PropagationMode mode) {
    std::vector<Move> candidates;
    std::vector<Move> legal;
    candidates.reserve(kMoveBufferCapacity);
    legal.reserve(kMoveBufferCapacity);
    generate_legal_moves(state, legal, candidates, mode);
    return legal;
}

void generate_legal_moves_profiled(const State& state,
                                   std::vector<Move>& legal,
                                   std::vector<Move>& candidates,
                                   RuleMetrics& metrics,
                                   PropagationMode mode) {
    generate_legal_moves_impl(state, legal, candidates, LegalMoveGenerationContext{mode, &metrics});
}

bool is_immediate_winning_move(const State& state, const Move& move) {
    State copy = state;
    Undo undo;
    if (!apply_move(copy, move, undo))
        return false;
    return copy.terminal == Terminal::Catch || copy.terminal == Terminal::Try;
}

}  // namespace qas

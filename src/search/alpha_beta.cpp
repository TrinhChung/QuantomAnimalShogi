#include "search/alpha_beta.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "core/timing.hpp"

namespace qas {
namespace {

/// @brief Computes the bit width required to encode a zero-based value range.
/// @param value_count Number of distinct values in the range.
/// @return Minimum number of bits that can encode every value.
constexpr unsigned required_bit_count(std::size_t value_count) {
    unsigned bit_count = 0;
    for (std::size_t maximum = value_count - 1; maximum != 0; maximum >>= 1U)
        ++bit_count;
    return bit_count;
}

constexpr unsigned kDestinationBitCount = required_bit_count(board_size);
constexpr unsigned kSourceBitCount = required_bit_count(external_source_count);
constexpr unsigned kPieceBitCount = required_bit_count(physical_piece_count);
constexpr unsigned kSourceShift = kDestinationBitCount;
constexpr unsigned kPieceShift = kSourceShift + kSourceBitCount;
constexpr unsigned kPackedMoveBitCount = kPieceShift + kPieceBitCount;
constexpr std::uint16_t kDestinationMask =
    static_cast<std::uint16_t>((1U << kDestinationBitCount) - 1U);
constexpr std::uint16_t kSourceMask = static_cast<std::uint16_t>((1U << kSourceBitCount) - 1U);
constexpr std::uint16_t kPieceMask = static_cast<std::uint16_t>((1U << kPieceBitCount) - 1U);
constexpr int kMateScoreMargin = 512;
constexpr int kMateStopMargin = 256;
constexpr std::uint64_t kStopCheckNodeInterval = 64;
constexpr std::uint64_t kStopCheckNodeMask = kStopCheckNodeInterval - 1;

static_assert(kPackedMoveBitCount < std::numeric_limits<std::uint16_t>::digits,
              "packed move must leave a distinct invalid sentinel");
static_assert((kStopCheckNodeInterval & kStopCheckNodeMask) == 0,
              "stop-check interval must be a power of two");

class ComponentTimer {
   public:
    /// @brief Starts optional timing and increments a component call counter.
    /// @param counter Optional counter; null disables all timing work.
    explicit ComponentTimer(TimedCounter* counter) : counter_(counter) {
        if (counter_ != nullptr) {
            ++counter_->calls;
            start_ = std::chrono::steady_clock::now();
        }
    }

    /// @brief Adds the scoped elapsed time to the component counter.
    ~ComponentTimer() {
        if (counter_ != nullptr) {
            counter_->elapsed_ms += elapsed_milliseconds(start_);
        }
    }

   private:
    TimedCounter* counter_{nullptr};
    std::chrono::steady_clock::time_point start_{};
};

/// @brief Counts set squares in a board bitmask.
/// @param mask Board bitmask to inspect.
/// @return Number of set board squares.
int popcount_board(std::uint16_t mask) {
    int count = 0;
    for (; mask != 0; mask = static_cast<std::uint16_t>(mask & (mask - 1)))
        ++count;
    return count;
}

/// @brief Counts bits during compile-time lookup-table construction.
/// @param value Unsigned mask to inspect.
/// @return Number of set bits.
constexpr int constant_popcount(unsigned value) {
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

/// @brief Builds popcounts for every five-bit animal-form mask.
/// @return Compile-time 32-entry popcount table.
constexpr std::array<int, 32> build_mask_popcounts() {
    std::array<int, 32> result{};
    for (unsigned mask = 0; mask < result.size(); ++mask)
        result[mask] = constant_popcount(mask);
    return result;
}

/// @brief Builds expected material values for every animal-form mask.
/// @return Compile-time 32-entry material table.
constexpr std::array<int, 32> build_expected_piece_values() {
    constexpr std::array<int, 5> values{100, 320, 310, 1050, 420};
    std::array<int, 32> result{};
    for (unsigned mask = 1; mask < result.size(); ++mask) {
        int total = 0;
        int count = 0;
        for (unsigned form = 0; form < values.size(); ++form) {
            if ((mask & (1U << form)) != 0) {
                total += values[form];
                ++count;
            }
        }
        result[mask] = total / count;
    }
    return result;
}

inline constexpr auto kMaskPopcounts = build_mask_popcounts();
inline constexpr auto kExpectedPieceValues = build_expected_piece_values();
inline constexpr std::uint16_t kBoardMask = (1U << board_size) - 1U;
inline constexpr std::uint16_t kMidBoardMask = 0b000111'111000U;
inline constexpr std::uint16_t kCenterSpineMask = 0b010010'010010U;
inline constexpr std::uint16_t kSouthBackRankMask = 0b000000'000111U;
inline constexpr std::uint16_t kNorthBackRankMask = 0b111000'000000U;
inline constexpr int kOriginPieceCount = 4;

struct OmegaBitmaskTables {
    std::array<std::array<std::uint32_t, 16>, kOriginPieceCount> slot_valid_worlds{};
    std::array<std::uint32_t, kOriginPieceCount> slot_lion_worlds{};
};

/// @brief Converts a five-bit animal form mask into its four-bit CH/G/E/L lineage mask.
/// @param forms Five-bit animal form mask.
/// @return Four-bit lineage capability mask.
constexpr std::uint8_t to_lineage_bits(Mask forms) {
    std::uint8_t result = 0;
    if ((forms & (bit(Animal::Chick) | bit(Animal::Hen))) != 0)
        result |= 1U << 0U;
    if ((forms & bit(Animal::Giraffe)) != 0)
        result |= 1U << 1U;
    if ((forms & bit(Animal::Elephant)) != 0)
        result |= 1U << 2U;
    if ((forms & bit(Animal::Lion)) != 0)
        result |= 1U << 3U;
    return result;
}

/// @brief Builds the 320-byte compile-time 24-bit permutation bitmask tables for Omega worlds.
/// @return Compile-time Omega bitmask tables.
constexpr OmegaBitmaskTables build_omega_bitmask_tables() {
    constexpr int perms[24][4] = {
        {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {0, 3, 2, 1},
        {1, 0, 2, 3}, {1, 0, 3, 2}, {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0},
        {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0}, {2, 3, 0, 1}, {2, 3, 1, 0},
        {3, 0, 1, 2}, {3, 0, 2, 1}, {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0},
    };
    OmegaBitmaskTables tables{};
    for (int p = 0; p < 24; ++p) {
        const std::uint32_t p_bit = 1U << static_cast<unsigned>(p);
        for (int slot = 0; slot < kOriginPieceCount; ++slot) {
            const int animal = perms[p][slot];
            if (animal == 3)
                tables.slot_lion_worlds[slot] |= p_bit;
            for (unsigned mask = 0; mask < 16U; ++mask) {
                if ((mask & (1U << static_cast<unsigned>(animal))) != 0)
                    tables.slot_valid_worlds[slot][mask] |= p_bit;
            }
        }
    }
    return tables;
}

inline constexpr auto kOmegaBitmaskTables = build_omega_bitmask_tables();

/// @brief Counts Lion-capable pieces currently owned by a side.
/// @param state Source state.
/// @param owner Current owner to count.
/// @return Number of owned Lion candidates.
int lion_candidate_count(const State& state, Side owner) {
    int count = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (owned_by(state, piece, owner) && contains(state.mask[piece], Animal::Lion))
            ++count;
    }
    return count;
}

/// @brief Tests whether a terminal state was won by a selected side.
/// @param state Terminal candidate state.
/// @param side Side expected to be the winner.
/// @return `true` for a Catch or Try credited to the selected side.
bool is_terminal_win_for(const State& state, Side side) {
    return (state.terminal == Terminal::Catch || state.terminal == Terminal::Try) &&
           state.winner == side_index(side);
}

/// @brief Exhaustively tests pseudo-legal moves for an immediate Catch or Try.
/// @param state Source state.
/// @param side Side to install as the mover.
/// @param profile Optional per-component timing sink.
/// @return `true` when at least one transition immediately wins.
bool side_has_immediate_win(const State& state, Side side, EvalComponentProfile* profile) {
    State copy;
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_setup);
        copy = state;
        copy.side_to_move = side;
        copy.terminal = Terminal::None;
        copy.winner = kNoWinner;
        recompute_hash(copy);
    }
    std::vector<Move> moves;
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_movegen);
        moves = generate_pseudo_legal_moves(copy);
    }
    for (const Move& move : moves) {
        Undo undo;
        bool applied = false;
        {
            ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_transition);
            applied = apply_move(copy, move, undo);
        }
        if (applied && is_terminal_win_for(copy, side)) {
            return true;
        }
        {
            ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_setup);
            copy = state;
            copy.side_to_move = side;
            copy.terminal = Terminal::None;
            copy.winner = kNoWinner;
            recompute_hash(copy);
        }
    }
    return false;
}

/// @brief Measures forward progress of a square toward a side's Try rank.
/// @param square Board or hand position.
/// @param side Moving side.
/// @return Nonnegative row progress, or zero for hand positions.
int progress_to_try(int square, Side side) {
    if (square >= board_size)
        return 0;
    const int row = square / board_width;
    return side == Side::South ? board_height - 1 - row : row;
}

/// @brief Packs a valid move into the transposition-table representation.
/// @param move Move to pack.
/// @return Packed move or `kInvalidPackedMove` for an invalid move.
std::uint16_t pack_move(const Move& move) {
    if (!move.valid())
        return kInvalidPackedMove;
    return static_cast<std::uint16_t>(move.to | (move.from << kSourceShift) |
                                      (move.piece << kPieceShift));
}

/// @brief Unpacks a transposition-table move representation.
/// @param packed Packed move value.
/// @return Decoded move or an invalid move for `kInvalidPackedMove`.
Move unpack_move(std::uint16_t packed) {
    if (packed == kInvalidPackedMove)
        return {};
    return Move{static_cast<std::uint8_t>((packed >> kPieceShift) & kPieceMask),
                static_cast<std::uint8_t>((packed >> kSourceShift) & kSourceMask),
                static_cast<std::uint8_t>(packed & kDestinationMask)};
}

/// @brief Normalizes root-relative mate distance before table storage.
/// @param score Search score at the current ply.
/// @param ply Distance from root.
/// @return Transposition-table score.
int score_to_tt(int score, int ply) {
    if (score > mate_score - kMateScoreMargin)
        return score + ply;
    if (score < -mate_score + kMateScoreMargin)
        return score - ply;
    return score;
}

/// @brief Restores current-root mate distance after a table probe.
/// @param score Stored transposition-table score.
/// @param ply Distance from root.
/// @return Search score for the current ply.
int score_from_tt(int score, int ply) {
    if (score > mate_score - kMateScoreMargin)
        return score - ply;
    if (score < -mate_score + kMateScoreMargin)
        return score + ply;
    return score;
}

/// @brief Accumulates every rule metric field into a search-owned total.
/// @param target Metrics accumulator to mutate.
/// @param source Metrics sample to add.
void add_rule_metrics(RuleMetrics& target, const RuleMetrics& source) {
    target.pseudo_move_generation_calls += source.pseudo_move_generation_calls;
    target.pseudo_moves_generated += source.pseudo_moves_generated;
    target.legal_filter_calls += source.legal_filter_calls;
    target.legal_moves_generated += source.legal_moves_generated;
    target.pseudo_moves_rejected += source.pseudo_moves_rejected;
    target.apply_move_internal_calls += source.apply_move_internal_calls;
    target.apply_move_internal_successes += source.apply_move_internal_successes;
    target.apply_move_internal_failures += source.apply_move_internal_failures;
    target.propagation_calls += source.propagation_calls;
    target.propagation_iterations += source.propagation_iterations;
    target.hash_recompute_calls += source.hash_recompute_calls;
    target.pseudo_move_generation_ms += source.pseudo_move_generation_ms;
    target.legal_filter_ms += source.legal_filter_ms;
    target.apply_move_internal_ms += source.apply_move_internal_ms;
    target.propagation_ms += source.propagation_ms;
    target.hash_recompute_ms += source.hash_recompute_ms;
}

}  // namespace

AlphaBetaEngine::AlphaBetaEngine(std::size_t table_entries) {
    if (table_entries == 0 || (table_entries & (table_entries - 1)) != 0) {
        throw std::invalid_argument("transposition table size must be a power of two");
    }
    table_.resize(table_entries);
    table_mask_ = table_entries - 1;
    for (std::size_t ply = 0; ply < kMaxSearchPly; ++ply) {
        move_pool_[ply].reserve(kMoveBufferCapacity);
        candidate_pool_[ply].reserve(kMoveBufferCapacity);
        score_pool_[ply].reserve(kMoveBufferCapacity);
    }
}

void AlphaBetaEngine::clear_tt() {
    std::fill(table_.begin(), table_.end(), TTEntry{});
}

TTEntry* AlphaBetaEngine::probe(std::uint64_t key) {
    return &table_[key & table_mask_];
}
const TTEntry* AlphaBetaEngine::probe(std::uint64_t key) const {
    return &table_[key & table_mask_];
}

void AlphaBetaEngine::store(
    std::uint64_t key, int depth, int score, BoundType bound, const Move& move, int ply) {
    if (!options_.use_tt)
        return;
    TTEntry& entry = *probe(key);
    const BoundType old_bound = static_cast<BoundType>(entry.flag);
    if (entry.key != 0 && entry.key != key && entry.age == age_ && entry.depth > depth)
        return;
    if (entry.key == key && entry.depth > depth && old_bound == BoundType::Exact)
        return;
    if (entry.key != 0) {
        ++stats_.tt_replacements;
        if (entry.key != key)
            ++stats_.tt_collisions_detected;
        if (entry.age != age_)
            ++stats_.tt_age_replacements;
        else if (depth >= entry.depth)
            ++stats_.tt_depth_replacements;
    }
    const bool preserve_existing_move =
        entry.key == key && bound == BoundType::Upper && entry.move16 != kInvalidPackedMove;
    entry.key = key;
    entry.depth = static_cast<std::int16_t>(depth);
    entry.score = score_to_tt(score, ply);
    entry.flag = static_cast<std::uint8_t>(bound);
    if (!preserve_existing_move)
        entry.move16 = pack_move(move);
    entry.age = age_;
    entry.generation = age_;
}

bool AlphaBetaEngine::should_stop() {
    if (stopped_)
        return true;
    if (options_.stop_policy != nullptr &&
        options_.stop_policy(stats_.searched_nodes, options_.stop_context)) {
        stopped_ = true;
        return true;
    }
    if ((stats_.searched_nodes & kStopCheckNodeMask) == 0U &&
        std::chrono::steady_clock::now() >= hard_deadline_) {
        stopped_ = true;
    }
    return stopped_;
}

int AlphaBetaEngine::terminal_score(const State& state, int ply) const {
    if (state.terminal == Terminal::Draw)
        return 0;
    if (state.terminal == Terminal::Catch || state.terminal == Terminal::Try) {
        const bool current_won = state.winner == side_index(state.side_to_move);
        return current_won ? mate_score - ply : -mate_score + ply;
    }
    if (state.terminal == Terminal::Illegal)
        return -mate_score + ply;
    return 0;
}

struct TacticalSummary {
    std::array<std::uint16_t, 2> occupancy{};
    std::array<std::uint16_t, 2> attack_mask{};
    std::array<std::uint16_t, 2> double_attack_mask{};
    std::array<std::uint16_t, 2> orth_attack_mask{};
    std::array<std::uint16_t, 2> diag_attack_mask{};
    std::array<std::uint16_t, 2> lion_board_mask{};
    std::array<std::uint16_t, 2> checker_mask{};
    std::array<int, 2> mobility{};
    std::array<int, 2> safe_mobility{};
    std::array<int, 2> safe_drops{};
    std::array<int, 2> hand_pieces{};
    std::array<int, 2> lion_candidates{};
    std::array<int, 2> origin_worlds{};
    std::array<std::uint8_t, physical_piece_count> lion_prob_24{};
};

/// @brief Populates exact Omega-World marginal probabilities for one origin using 24-bit bitmasks.
/// @param state Source state.
/// @param origin Origin group to inspect.
/// @param summary Tactical summary receiving world count and Lion probabilities.
void populate_origin_omega(const State& state, Side origin, TacticalSummary& summary) {
    std::array<int, kOriginPieceCount> pieces{};
    int count = 0;
    std::uint32_t valid_worlds = 0x00FF'FFFFU;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!originated_from(state, piece, origin))
            continue;
        if (count < kOriginPieceCount) {
            pieces[count] = piece;
            valid_worlds &=
                kOmegaBitmaskTables.slot_valid_worlds[count][to_lineage_bits(state.mask[piece])];
            ++count;
        }
    }
    const int worlds = std::max(1, constant_popcount(valid_worlds));
    summary.origin_worlds[side_index(origin)] = worlds;
    for (int idx = 0; idx < count; ++idx) {
        const int piece = pieces[idx];
        if (!contains(state.mask[piece], Animal::Lion)) {
            summary.lion_prob_24[piece] = 0;
            continue;
        }
        const int lion_worlds =
            constant_popcount(valid_worlds & kOmegaBitmaskTables.slot_lion_worlds[idx]);
        summary.lion_prob_24[piece] =
            static_cast<std::uint8_t>((lion_worlds * 24 + worlds / 2) / worlds);
    }
}

/// @brief Populates multi-layer 12-bit attack masks, occupancy, and safe mobility metrics.
/// @param state Source state.
/// @param summary Tactical summary to populate.
void populate_tactical_bitboards(const State& state, TacticalSummary& summary) {
    const auto& tables = move_tables();
    int empty_count = 0;
    std::uint16_t empty_mask = 0;
    for (int square = 0; square < board_size; ++square) {
        const int piece = state.board[square];
        if (piece < 0) {
            ++empty_count;
            empty_mask |= static_cast<std::uint16_t>(1U << square);
        } else {
            const int owner = (state.owner_bits >> piece) & 1U;
            summary.occupancy[owner] |= static_cast<std::uint16_t>(1U << square);
        }
    }
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        const int owner = (state.owner_bits >> piece) & 1U;
        const Mask mask = state.mask[piece];
        if (contains(mask, Animal::Lion))
            ++summary.lion_candidates[owner];
        if (is_hand_position(state.pos[piece])) {
            ++summary.hand_pieces[owner];
            summary.mobility[owner] += empty_count;
            continue;
        }
        const int sq = state.pos[piece];
        const std::uint16_t attacks = tables.quantum_move_table[mask][owner][sq];
        summary.double_attack_mask[owner] |=
            static_cast<std::uint16_t>(summary.attack_mask[owner] & attacks);
        summary.attack_mask[owner] |= attacks;
        if ((mask & (bit(Animal::Giraffe) | bit(Animal::Hen) | bit(Animal::Lion) |
                     bit(Animal::Chick))) != 0) {
            summary.orth_attack_mask[owner] |=
                tables.move_table[static_cast<int>(Animal::Giraffe)][owner][sq] & attacks;
        }
        if ((mask & (bit(Animal::Elephant) | bit(Animal::Hen) | bit(Animal::Lion))) != 0) {
            summary.diag_attack_mask[owner] |=
                tables.move_table[static_cast<int>(Animal::Elephant)][owner][sq] & attacks;
        }
        if (contains(mask, Animal::Lion))
            summary.lion_board_mask[owner] |= static_cast<std::uint16_t>(1U << sq);
    }
    for (int owner = 0; owner < 2; ++owner) {
        const std::uint16_t enemy_att = summary.attack_mask[1 - owner];
        summary.safe_drops[owner] =
            popcount_board(static_cast<std::uint16_t>(empty_mask & ~enemy_att));
    }
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (is_hand_position(state.pos[piece]))
            continue;
        const int owner = (state.owner_bits >> piece) & 1U;
        const Mask mask = state.mask[piece];
        const std::uint16_t destinations = tables.quantum_move_table[mask][owner][state.pos[piece]];
        if ((destinations & summary.lion_board_mask[1 - owner]) != 0) {
            summary.checker_mask[owner] |= static_cast<std::uint16_t>(1U << state.pos[piece]);
        }
        const std::uint16_t available = static_cast<std::uint16_t>(
            destinations & static_cast<std::uint16_t>(~summary.occupancy[owner]) & kBoardMask);
        summary.mobility[owner] += popcount_board(available);
        const std::uint16_t safe_zone =
            mask == bit(Animal::Lion) ? static_cast<std::uint16_t>(~summary.attack_mask[1 - owner])
                                      : static_cast<std::uint16_t>(~summary.attack_mask[1 - owner] |
                                                                   summary.attack_mask[owner]);
        summary.safe_mobility[owner] +=
            popcount_board(static_cast<std::uint16_t>(available & safe_zone));
    }
}

/// @brief Scores piece coordination, battery focal fire, and Zugzwang squeeze compression.
/// @param summary Precomputed tactical bitboard summary.
/// @return South-perspective coordination and squeeze score.
int score_coordination_and_squeeze(const TacticalSummary& summary) {
    int south_score = 18 * (summary.mobility[0] - summary.mobility[1]) +
                      8 * (summary.safe_mobility[0] - summary.safe_mobility[1]);
    for (int owner = 0; owner < 2; ++owner) {
        const int sign = owner == 0 ? 1 : -1;
        const int opp = 1 - owner;
        const std::uint16_t defended =
            static_cast<std::uint16_t>(summary.occupancy[owner] & summary.attack_mask[owner]);
        south_score += sign * 20 * popcount_board(defended);
        south_score +=
            sign * 12 * popcount_board(static_cast<std::uint16_t>(defended & kMidBoardMask));
        const std::uint16_t battery_targets =
            static_cast<std::uint16_t>(summary.double_attack_mask[owner] & summary.occupancy[opp] &
                                       ~summary.double_attack_mask[opp]);
        south_score += sign * 42 * popcount_board(battery_targets);
        const std::uint16_t web_synergy = static_cast<std::uint16_t>(
            summary.orth_attack_mask[owner] & summary.diag_attack_mask[owner] & kMidBoardMask);
        south_score += sign * 10 * popcount_board(web_synergy);

        if (summary.safe_mobility[owner] == 0)
            south_score -= sign * 120;
        else if (summary.safe_mobility[owner] == 1)
            south_score -= sign * 45;
        if (summary.hand_pieces[owner] > 0 && summary.safe_drops[owner] == 0)
            south_score -= sign * 35 * summary.hand_pieces[owner];
    }
    return south_score;
}

/// @brief Scores King-ring encirclement, Try corridors, quantum exposure, and sound bait.
/// @param state Source state.
/// @param summary Precomputed tactical summary.
/// @return South-perspective siege, Try, quantum info, and bait score.
int score_siege_try_and_bait(const State& state, const TacticalSummary& summary) {
    const auto& tables = move_tables();
    const int mover_idx = side_index(state.side_to_move);
    int south_score = 85 * (summary.lion_candidates[0] - summary.lion_candidates[1]) +
                      4 * (summary.origin_worlds[0] - summary.origin_worlds[1]);
    for (int owner = 0; owner < 2; ++owner) {
        const int sign = owner == 0 ? 1 : -1;
        if (summary.lion_candidates[owner] == 1)
            south_score -= sign * 80;
    }

    for (int piece = 0; piece < physical_piece_count; ++piece) {
        const int owner = (state.owner_bits >> piece) & 1U;
        const int opp = 1 - owner;
        const int sign = owner == 0 ? 1 : -1;
        const Side owner_side = owner == 0 ? Side::South : Side::North;
        const Mask mask = state.mask[piece];
        const bool on_board = state.pos[piece] < board_size;
        const int lion_w = contains(mask, Animal::Lion)
                               ? std::max(1, static_cast<int>(summary.lion_prob_24[piece]))
                               : 0;

        if (lion_w > 0) {
            if (!on_board) {
                if (mask == bit(Animal::Lion))
                    south_score -= sign * 900;
                continue;
            }
            const int sq = state.pos[piece];
            const bool under_fire = (summary.attack_mask[opp] & (1U << sq)) != 0;
            const std::uint16_t ring = tables.move_table[static_cast<int>(Animal::Lion)][owner][sq];
            const int ring_attacks =
                popcount_board(static_cast<std::uint16_t>(ring & summary.attack_mask[opp]));
            south_score -= sign * 2 * ring_attacks * lion_w;
            if (under_fire) {
                const bool undefended_single_check =
                    owner == mover_idx && popcount_board(summary.checker_mask[opp]) == 1 &&
                    (summary.checker_mask[opp] & summary.attack_mask[opp]) == 0;
                if (undefended_single_check) {
                    south_score -= sign * 90;
                } else {
                    south_score -= sign * (mask == bit(Animal::Lion) ? 720 : (140 + 6 * lion_w));
                    if ((summary.double_attack_mask[opp] & (1U << sq)) != 0)
                        south_score -= sign * 120;
                    const std::uint16_t safe_ring = static_cast<std::uint16_t>(
                        ring & ~summary.occupancy[owner] & ~summary.attack_mask[opp] & kBoardMask);
                    if (safe_ring == 0)
                        south_score -= sign * 16 * lion_w;
                }
            }

            const int prog = progress_to_try(sq, owner_side);
            south_score += sign * prog * (mask == bit(Animal::Lion) ? 95 : 28);
            const std::uint16_t back_rank = owner == 0 ? kSouthBackRankMask : kNorthBackRankMask;
            if (prog == 3) {
                if (!under_fire) {
                    south_score += sign * (28'000 + 80 * lion_w);
                } else if (summary.lion_candidates[owner] >= 2) {
                    south_score += sign * 180;
                }
            } else if (prog == 2) {
                const std::uint16_t try_steps =
                    static_cast<std::uint16_t>(tables.quantum_move_table[mask][owner][sq] &
                                               back_rank & ~summary.occupancy[owner]);
                const std::uint16_t safe_try =
                    static_cast<std::uint16_t>(try_steps & ~summary.attack_mask[opp]);
                if (safe_try != 0) {
                    const int safe_count = popcount_board(safe_try);
                    if (mover_idx == owner) {
                        south_score += sign * (1800 + 30 * lion_w + (safe_count >= 2 ? 700 : 0));
                    } else {
                        south_score += sign * (360 + 12 * lion_w + (safe_count >= 2 ? 600 : 0));
                    }
                }
            }
        }

        if (on_board && mask != bit(Animal::Lion)) {
            const int sq = state.pos[piece];
            const bool under_fire = (summary.attack_mask[opp] & (1U << sq)) != 0;
            if (!under_fire)
                continue;
            const bool defended = (summary.attack_mask[owner] & (1U << sq)) != 0;
            const std::uint16_t piece_att = tables.quantum_move_table[mask][owner][sq];
            const std::uint16_t back_rank = owner == 0 ? kSouthBackRankMask : kNorthBackRankMask;
            const bool tactical_threat =
                (piece_att & (summary.lion_board_mask[opp] | back_rank)) != 0;
            const bool quantum_bait =
                defended && lion_w > 0 && lion_w < 24 && summary.lion_candidates[owner] >= 2;
            if ((defended && tactical_threat) || quantum_bait) {
                south_score += sign * 35;
            } else if (!defended) {
                south_score -= sign * 38;
            }
        }
    }
    return south_score;
}

/// @brief Tests whether a side has an immediate legal Catch transition using bitboard prefiltering.
/// @param state Source state.
/// @param summary Precomputed tactical bitboard summary.
/// @param side Side to test as the mover.
/// @param candidates Reusable candidate move buffer.
/// @param profile Optional per-component timing sink.
/// @return `true` when at least one capture immediately wins for `side`.
bool has_targeted_immediate_catch(const State& state,
                                  const TacticalSummary& summary,
                                  Side side,
                                  std::vector<Move>& candidates,
                                  EvalComponentProfile* profile) {
    const int s_idx = side_index(side);
    const std::uint16_t target_lions = summary.lion_board_mask[1 - s_idx];
    if ((summary.attack_mask[s_idx] & target_lions) == 0)
        return false;

    const auto& tables = move_tables();
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_filter);
        candidates.clear();
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            if (((state.owner_bits >> piece) & 1U) != static_cast<unsigned>(s_idx) ||
                state.pos[piece] >= board_size) {
                continue;
            }
            std::uint16_t hits = static_cast<std::uint16_t>(
                tables.quantum_move_table[state.mask[piece]][s_idx][state.pos[piece]] &
                target_lions);
            while (hits != 0) {
                int to_sq = 0;
                while (((hits >> to_sq) & 1U) == 0)
                    ++to_sq;
                candidates.push_back(Move{static_cast<std::uint8_t>(piece),
                                          state.pos[piece],
                                          static_cast<std::uint8_t>(to_sq)});
                hits &= static_cast<std::uint16_t>(hits - 1U);
            }
        }
    }
    if (candidates.empty())
        return false;

    State copy;
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_setup);
        copy = state;
        copy.side_to_move = side;
        copy.terminal = Terminal::None;
        copy.winner = kNoWinner;
    }
    for (const Move& move : candidates) {
        Undo undo;
        bool applied = false;
        {
            ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_transition);
            applied = apply_move(copy, move, undo);
        }
        if (applied && is_terminal_win_for(copy, side))
            return true;
        if (applied)
            undo_move(copy, undo);
    }
    return false;
}

AlphaBetaEngine::EvalScratch::EvalScratch() {
    pseudo_moves.reserve(128);
    immediate_candidates.reserve(32);
}

int AlphaBetaEngine::evaluate_baseline(const State& state, EvalComponentProfile* profile) const {
    EvalScratch scratch;
    return evaluate_optimized(state, scratch, profile);
}

int AlphaBetaEngine::evaluate_optimized(const State& state,
                                        EvalScratch& scratch,
                                        EvalComponentProfile* profile) const {
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->terminal);
        if (state.terminal != Terminal::None)
            return terminal_score(state, 0);
    }

    TacticalSummary summary{};
    int south_score = 0;
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->material_mask);
        populate_origin_omega(state, Side::South, summary);
        populate_origin_omega(state, Side::North, summary);
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const int owner = (state.owner_bits >> piece) & 1U;
            const int sign = owner == 0 ? 1 : -1;
            const Mask mask = state.mask[piece];
            south_score += sign * kExpectedPieceValues[mask];
            south_score += sign * (kMaskPopcounts[mask] - 1) * 12;
        }
    }
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->mobility);
        populate_tactical_bitboards(state, summary);
        south_score += score_coordination_and_squeeze(summary);
    }
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->lion_safety);
        south_score += score_siege_try_and_bait(state, summary);
    }

    const Side mover = state.side_to_move;
    const Side opp = opposite(mover);
    const int mover_idx = side_index(mover);
    const int opp_idx = 1 - mover_idx;
    const int mover_sign = mover_idx == 0 ? 1 : -1;
    if (has_targeted_immediate_catch(
            state, summary, mover, scratch.immediate_candidates, profile)) {
        south_score += mover_sign * 35'000;
    } else {
        const bool supported_or_double_check =
            popcount_board(summary.checker_mask[opp_idx]) >= 2 ||
            (summary.checker_mask[opp_idx] & summary.attack_mask[opp_idx]) != 0;
        if (supported_or_double_check &&
            has_targeted_immediate_catch(
                state, summary, opp, scratch.immediate_candidates, profile)) {
            south_score -= mover_sign * 2600;
        }
    }
    return mover_idx == 0 ? south_score : -south_score;
}

int AlphaBetaEngine::evaluate(const State& state) const {
    EvalScratch scratch;
    return evaluate_optimized(state, scratch, nullptr);
}

int AlphaBetaEngine::evaluate_search(const State& state) {
    EvalComponentProfile* profile =
        options_.benchmark_instrumentation_enabled ? &stats_.eval_components : nullptr;
    if (!options_.benchmark_instrumentation_enabled) {
        return options_.optimized_eval_enabled ? evaluate_optimized(state, eval_scratch_, nullptr)
                                               : evaluate_baseline(state, nullptr);
    }
    const auto begin = std::chrono::steady_clock::now();
    const int score = options_.optimized_eval_enabled
                          ? evaluate_optimized(state, eval_scratch_, profile)
                          : evaluate_baseline(state, profile);
    const auto end = std::chrono::steady_clock::now();
    ++stats_.eval_calls;
    stats_.eval_ms += elapsed_milliseconds(begin, end);
    return score;
}

bool AlphaBetaEngine::apply_search_move(State& state, const Move& move, Undo& undo) {
    if (!options_.benchmark_instrumentation_enabled)
        return apply_move(state, move, undo, options_.propagation_mode);
    RuleMetrics metrics;
    const bool applied = apply_move_profiled(state, move, undo, metrics, options_.propagation_mode);
    add_rule_metrics(stats_.rule_metrics, metrics);
    stats_.propagation_calls += metrics.propagation_calls;
    stats_.propagation_ms += metrics.propagation_ms;
    return applied;
}

void AlphaBetaEngine::update_pv(int ply, const Move& move) {
    if (ply < 0 || ply >= static_cast<int>(kMaxSearchPly))
        return;
    auto& line = pv_pool_[static_cast<std::size_t>(ply)];
    line.clear();
    line.push_back(move);
    if (ply + 1 < static_cast<int>(kMaxSearchPly)) {
        const auto& child = pv_pool_[static_cast<std::size_t>(ply + 1)];
        line.insert(line.end(), child.begin(), child.end());
    }
}

int AlphaBetaEngine::move_order_score(const State& state,
                                      const Move& move,
                                      const Move& tt_move,
                                      int ply) {
    if (options_.tt_move_ordering_enabled && move == tt_move)
        return 2'000'000;
    const Side mover = state.side_to_move;
    const Side enemy = opposite(mover);
    const int mover_idx = side_index(mover);
    int score = 0;
    if (options_.killer_enabled && ply < static_cast<int>(killers_.size())) {
        if (move == killers_[ply][0])
            score += 180'000;
        else if (move == killers_[ply][1])
            score += 160'000;
    }
    if (ply > 0 && ply < static_cast<int>(kMaxSearchPly)) {
        const Move& prev = ply_move_[static_cast<std::size_t>(ply - 1)];
        if (prev.valid() && move == countermoves_[mover_idx][prev.from][prev.to])
            score += 145'000;
    }
    if (options_.history_enabled) {
        score += history_[mover_idx][move.from][move.to];
    }

    const int target = state.board[move.to];
    const Mask piece_mask = state.mask[move.piece];
    if (target >= 0 && options_.capture_ordering_enabled) {
        score += 250'000 + kExpectedPieceValues[state.mask[target]] * 20;
        if (state.mask[target] == bit(Animal::Lion))
            score += 1'000'000;
        else if (contains(state.mask[target], Animal::Lion))
            score += 320'000;
    }

    const Mask move_mask = move.from < board_size
                               ? move_tables().move_possible_mask[mover_idx][move.from][move.to]
                               : piece_mask;
    if (options_.mask_collapse_ordering_enabled) {
        score += (kMaskPopcounts[piece_mask] -
                  kMaskPopcounts[static_cast<Mask>(piece_mask & move_mask)]) *
                 12'000;
    }
    if (options_.try_threat_ordering_enabled && contains(piece_mask, Animal::Lion) &&
        move.to / board_width == (mover == Side::South ? 0 : board_height - 1)) {
        score += 30'000;
    }
    if (!options_.strong_ordering_enabled || ply > 0)
        return score;

    State after = state;
    Undo undo;
    if (!apply_search_move(after, move, undo))
        return -2'000'000;
    if (options_.immediate_win_ordering_enabled && is_terminal_win_for(after, mover)) {
        return 1'500'000 + score;
    }
    const int before_lions = lion_candidate_count(state, enemy);
    const int after_lions = lion_candidate_count(after, enemy);
    if (options_.lion_reduction_ordering_enabled)
        score += (before_lions - after_lions) * 70'000;
    if (options_.prevent_loss_ordering_enabled && side_has_immediate_win(state, enemy, nullptr) &&
        !side_has_immediate_win(after, enemy, nullptr)) {
        score += 400'000;
    }
    if (after.pos[move.piece] < board_size && contains(after.mask[move.piece], Animal::Lion)) {
        score += progress_to_try(after.pos[move.piece], mover) * 4000;
        if (after.mask[move.piece] == bit(Animal::Lion) &&
            is_square_attacked(after, after.pos[move.piece], enemy)) {
            score -= 180'000;
        }
    }
    score -= evaluate_search(after) / 4;
    return score;
}

void AlphaBetaEngine::order_moves(const State& state,
                                  std::vector<Move>& moves,
                                  const Move& tt_move,
                                  int ply) {
    std::chrono::steady_clock::time_point begin;
    if (options_.benchmark_instrumentation_enabled) {
        begin = std::chrono::steady_clock::now();
    }
    const auto& tables = move_tables();
    const int mover_idx = side_index(state.side_to_move);
    std::uint16_t mover_att = 0;
    std::uint16_t enemy_att = 0;
    std::uint16_t enemy_lions = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (state.pos[piece] >= board_size)
            continue;
        const int owner = (state.owner_bits >> piece) & 1U;
        const std::uint16_t att =
            tables.quantum_move_table[state.mask[piece]][owner][state.pos[piece]];
        if (owner == mover_idx) {
            mover_att |= att;
        } else {
            enemy_att |= att;
            if (contains(state.mask[piece], Animal::Lion))
                enemy_lions |= static_cast<std::uint16_t>(1U << state.pos[piece]);
        }
    }

    auto& scored = score_pool_[std::min<std::size_t>(ply, kMaxSearchPly - 1)];
    scored.clear();
    const int back_rank = mover_idx == 0 ? 0 : board_height - 1;
    const int own_home_rank = mover_idx == 0 ? board_height - 1 : 0;
    const std::uint16_t back_rank_mask = mover_idx == 0 ? kSouthBackRankMask : kNorthBackRankMask;
    for (const Move& move : moves) {
        int score = move_order_score(state, move, tt_move, ply);
        const Mask piece_mask = state.mask[move.piece];
        const Mask move_mask = move.from < board_size
                                   ? tables.move_possible_mask[mover_idx][move.from][move.to]
                                   : piece_mask;
        const Mask collapsed = static_cast<Mask>(piece_mask & move_mask);
        const bool dest_attacked = (enemy_att & (1U << move.to)) != 0;
        const std::uint16_t next_att = tables.quantum_move_table[collapsed][mover_idx][move.to];
        if (move.to / board_width == own_home_rank && (enemy_lions & (1U << move.to)) != 0)
            score += 650'000;
        if ((next_att & enemy_lions) != 0 && !dest_attacked)
            score += 45'000;
        if (contains(collapsed, Animal::Lion)) {
            if (move.to / board_width == back_rank && !dest_attacked) {
                score += 420'000;
            } else if (!dest_attacked && (next_att & back_rank_mask & ~enemy_att) != 0) {
                score += 85'000;
            }
        }
        if (piece_mask == bit(Animal::Lion) && dest_attacked)
            score -= 550'000;
        else if (state.board[move.to] < 0 && dest_attacked && (mover_att & (1U << move.to)) != 0 &&
                 (next_att & enemy_lions) != 0)
            score += 28'000;
        scored.emplace_back(score, move);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    for (std::size_t index = 0; index < moves.size(); ++index)
        moves[index] = scored[index].second;
    if (options_.benchmark_instrumentation_enabled) {
        ++stats_.move_order_calls;
        stats_.move_order_ms += elapsed_milliseconds(begin);
    }
}

void AlphaBetaEngine::generate_search_moves(
    const State& state, const Move& tt_move, int depth, int ply, std::vector<Move>& moves) {
    auto& scratch = candidate_pool_[std::min<std::size_t>(ply, kMaxSearchPly - 1)];
    if (options_.benchmark_instrumentation_enabled) {
        const auto begin = std::chrono::steady_clock::now();
        RuleMetrics metrics;
        generate_legal_moves_profiled(state, moves, scratch, metrics, options_.propagation_mode);
        const auto end = std::chrono::steady_clock::now();
        ++stats_.movegen_calls;
        stats_.movegen_ms += elapsed_milliseconds(begin, end);
        add_rule_metrics(stats_.rule_metrics, metrics);
        stats_.propagation_calls += metrics.propagation_calls;
        stats_.propagation_ms += metrics.propagation_ms;
    } else {
        generate_legal_moves(state, moves, scratch, options_.propagation_mode);
    }
    const std::size_t raw_move_count = moves.size();
    ++stats_.expanded_nodes;
    stats_.generated_legal_moves += moves.size();
    stats_.max_legal_moves = std::max<std::uint64_t>(stats_.max_legal_moves, moves.size());
    if (options_.successor_reducer != nullptr) {
        const auto begin = std::chrono::steady_clock::now();
        bool applied = false;
        SuccessorReductionStats reducer_stats;
        const bool low_time =
            options_.reducer_disable_low_time && std::chrono::steady_clock::now() >= soft_deadline_;
        moves = options_.successor_reducer(
            state,
            moves,
            tt_move,
            options_.reducer_threshold,
            options_.reducer_min_depth,
            depth,
            options_.reducer_min_duplicate_ratio,
            options_.reducer_require_duplicate_hint,
            low_time,
            applied,
            options_.benchmark_instrumentation_enabled ? &reducer_stats : nullptr);
        const auto end = std::chrono::steady_clock::now();
        if (applied) {
            stats_.leq_grouping_ms += elapsed_milliseconds(begin, end);
            ++stats_.leq_grouped_nodes;
            stats_.leq_skipped_moves += raw_move_count - moves.size();
            stats_.leq_raw_moves += raw_move_count;
            stats_.leq_group_moves += moves.size();
            stats_.canonicalize_calls += reducer_stats.canonicalize_calls;
            stats_.canonicalize_ms += reducer_stats.canonicalize_ms;
            stats_.propagation_calls += reducer_stats.propagation_calls;
            stats_.propagation_ms += reducer_stats.propagation_ms;
        }
    }
    stats_.equivalent_successor_moves += moves.size();
}

int AlphaBetaEngine::negamax(State& state, int depth, int alpha, int beta, int ply) {
    ++stats_.searched_nodes;
    if (options_.benchmark_instrumentation_enabled && ply < static_cast<int>(kMaxSearchPly)) {
        pv_pool_[static_cast<std::size_t>(ply)].clear();
    }
    if (should_stop())
        return 0;
    if (state.terminal != Terminal::None)
        return terminal_score(state, ply);
    if (depth <= 0)
        return evaluate_search(state);

    const int original_alpha = alpha;
    Move tt_move;
    if (options_.use_tt) {
        ++stats_.tt_probes;
        const TTEntry* entry = probe(state.hash);
        if (entry->key == state.hash) {
            ++stats_.tt_hits;
            tt_move = unpack_move(entry->move16);
            if (entry->depth >= depth) {
                const int tt_score = score_from_tt(entry->score, ply);
                const BoundType bound = static_cast<BoundType>(entry->flag);
                if (bound == BoundType::Exact) {
                    ++stats_.tt_exact_hits;
                    return tt_score;
                }
                if (bound == BoundType::Lower) {
                    ++stats_.tt_lower_hits;
                    alpha = std::max(alpha, tt_score);
                }
                if (bound == BoundType::Upper) {
                    ++stats_.tt_upper_hits;
                    beta = std::min(beta, tt_score);
                }
                if (alpha >= beta)
                    return tt_score;
            }
        }
    }

    auto& moves = move_pool_[std::min<std::size_t>(ply, kMaxSearchPly - 1)];
    generate_search_moves(state, tt_move, depth, ply, moves);
    if (moves.empty())
        return -mate_score + ply;
    order_moves(state, moves, tt_move, ply);
    if (tt_move.valid() && std::find(moves.begin(), moves.end(), tt_move) != moves.end()) {
        ++stats_.tt_move_used;
    }

    const Side mover = state.side_to_move;
    const bool in_squeeze = moves.size() <= 2;
    int best = -search_infinity;
    Move best_move;
    bool first = true;
    int move_index = 0;
    for (const Move& move : moves) {
        const bool is_capture = state.board[move.to] != kEmptyBoardSquare;
        const bool is_lion_move = contains(state.mask[move.piece], Animal::Lion);
        const bool is_killer = ply < static_cast<int>(killers_.size()) &&
                               (move == killers_[ply][0] || move == killers_[ply][1]);
        if (ply < static_cast<int>(kMaxSearchPly))
            ply_move_[static_cast<std::size_t>(ply)] = move;

        Undo undo;
        if (!apply_search_move(state, move, undo))
            continue;
        int score = 0;
        if (first || !options_.pvs_enabled) {
            score = -negamax(state, depth - 1, -beta, -alpha, ply + 1);
        } else {
            int reduction = 0;
            if (depth >= 5 && move_index >= 6 && !in_squeeze && !is_capture && !is_lion_move &&
                !is_killer && state.terminal == Terminal::None) {
                reduction = 1;
            }
            if (reduction > 0) {
                score = -negamax(state, depth - 1 - reduction, -alpha - 1, -alpha, ply + 1);
            }
            if (reduction == 0 || (!stopped_ && score > alpha)) {
                score = -negamax(state, depth - 1, -alpha - 1, -alpha, ply + 1);
            }
            if (!stopped_ && score > alpha && score < beta) {
                ++stats_.pvs_researches;
                score = -negamax(state, depth - 1, -beta, -alpha, ply + 1);
            }
        }
        undo_move(state, undo);
        if (stopped_)
            return 0;
        if (score > best) {
            best = score;
            if (first || score > original_alpha) {
                best_move = move;
                if (options_.benchmark_instrumentation_enabled)
                    update_pv(ply, move);
            }
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            ++stats_.cutoffs;
            stats_.cutoff_rank_sum += static_cast<std::uint64_t>(move_index + 1);
            if (move_index == 0)
                ++stats_.first_move_cutoffs;
            if (move == tt_move)
                ++stats_.tt_move_cutoffs;
            const bool quiet = !is_capture;
            if (quiet && options_.killer_enabled && ply < static_cast<int>(killers_.size())) {
                if (killers_[ply][0] != move) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = move;
                }
                ++stats_.killer_cutoffs;
            }
            if (quiet && ply > 0 && ply < static_cast<int>(kMaxSearchPly)) {
                const Move& prev = ply_move_[static_cast<std::size_t>(ply - 1)];
                if (prev.valid()) {
                    countermoves_[side_index(mover)][prev.from][prev.to] = move;
                }
            }
            if (quiet && options_.history_enabled) {
                int& history = history_[side_index(mover)][move.from][move.to];
                history = std::min(500'000, history + depth * depth * 64);
                ++history_updates_;
                ++stats_.history_cutoffs;
                if (options_.history_decay_interval > 0 &&
                    history_updates_ % options_.history_decay_interval == 0) {
                    for (auto& side : history_)
                        for (auto& from : side)
                            for (int& value : from)
                                value /= 2;
                }
            }
            break;
        }
        first = false;
        ++move_index;
    }

    BoundType bound = BoundType::Exact;
    if (best <= original_alpha)
        bound = BoundType::Upper;
    else if (best >= beta)
        bound = BoundType::Lower;
    store(state.hash, depth, best, bound, best_move, ply);
    return best;
}

bool AlphaBetaEngine::search_root(
    State& state, int depth, int alpha, int beta, Move& best_move, int& best_score) {
    if (options_.benchmark_instrumentation_enabled)
        pv_pool_[0].clear();
    const int alpha_original = alpha;
    const int beta_original = beta;
    Move tt_move;
    if (options_.use_tt) {
        ++stats_.tt_probes;
        const TTEntry* entry = probe(state.hash);
        if (entry->key == state.hash) {
            ++stats_.tt_hits;
            tt_move = unpack_move(entry->move16);
        }
    }
    auto& moves = move_pool_[0];
    generate_search_moves(state, tt_move, depth, 0, moves);
    if (moves.empty())
        return true;
    order_moves(state, moves, tt_move, 0);
    int best = -search_infinity;
    Move local_best = moves.front();
    bool first = true;
    for (const Move& move : moves) {
        if (should_stop())
            return false;
        ply_move_[0] = move;
        Undo undo;
        if (!apply_search_move(state, move, undo))
            continue;
        int score = 0;
        if (first || !options_.pvs_enabled) {
            score = -negamax(state, depth - 1, -beta, -alpha, 1);
        } else {
            score = -negamax(state, depth - 1, -alpha - 1, -alpha, 1);
            if (!stopped_ && score > alpha && score < beta) {
                ++stats_.pvs_researches;
                score = -negamax(state, depth - 1, -beta, -alpha, 1);
            }
        }
        undo_move(state, undo);
        if (stopped_)
            return false;
        if (score > best) {
            best = score;
            if (first || score > alpha_original) {
                local_best = move;
                if (options_.benchmark_instrumentation_enabled)
                    update_pv(0, move);
            }
        }
        alpha = std::max(alpha, score);
        first = false;
        if (alpha >= beta)
            break;
    }
    best_move = local_best;
    best_score = best;
    const BoundType bound = best <= alpha_original  ? BoundType::Upper
                            : best >= beta_original ? BoundType::Lower
                                                    : BoundType::Exact;
    store(state.hash, depth, best, bound, best_move, 0);
    return true;
}

SearchResult AlphaBetaEngine::find_best_move(const State& root, const SearchOptions& options) {
    options_ = options;
    stats_ = {};
    stopped_ = false;
    if (options_.tt_clear_each_move)
        clear_tt();
    if (options_.tt_age_enabled)
        ++age_;
    killers_ = {};
    countermoves_ = {};
    ply_move_ = {};
    for (auto& side : history_)
        for (auto& from : side)
            for (int& value : from)
                value /= 2;
    if (options_.benchmark_instrumentation_enabled) {
        const std::size_t maximum_pv_length = std::min<std::size_t>(
            kMaxSearchPly, static_cast<std::size_t>(std::max(1, options_.max_depth)));
        for (std::size_t ply = 0; ply < maximum_pv_length; ++ply) {
            pv_pool_[ply].reserve(maximum_pv_length - ply);
        }
    }
    start_ = std::chrono::steady_clock::now();
    const int hard_ms =
        options.hard_time_limit_ms > 0 ? options.hard_time_limit_ms : options.time_limit_ms;
    const int soft_ms =
        options.soft_time_limit_ms > 0 ? options.soft_time_limit_ms : std::max(0, hard_ms * 9 / 10);
    soft_deadline_ = start_ + std::chrono::milliseconds(std::max(0, soft_ms));
    hard_deadline_ = start_ + std::chrono::milliseconds(std::max(0, hard_ms));

    SearchResult result;
    const auto fallback = generate_legal_moves(root, options_.propagation_mode);
    if (fallback.empty()) {
        result.score = root.terminal == Terminal::None ? -mate_score : terminal_score(root, 0);
        stats_.elapsed_ms = elapsed_milliseconds(start_);
        result.stats = stats_;
        return result;
    }
    result.best_move = fallback.front();
    result.has_move = true;
    result.pv_line = {result.best_move};
    result.score = evaluate_search(root);

    State state = root;
    const int first_depth =
        options.iterative_deepening_enabled ? 1 : std::max(1, options.max_depth);
    for (int depth = first_depth; depth <= std::max(1, options.max_depth); ++depth) {
        if (depth > first_depth && std::chrono::steady_clock::now() >= soft_deadline_)
            break;
        stats_.started_depth = depth;
        Move depth_move;
        int depth_score = 0;
        const std::uint64_t nodes_before = stats_.searched_nodes;
        const auto depth_start = std::chrono::steady_clock::now();
        int alpha = -search_infinity;
        int beta = search_infinity;
        int window = std::max(1, options.aspiration_initial_window);
        if (options.aspiration_enabled && depth > 1 && stats_.depth_reached > 0 &&
            std::abs(result.score) < mate_score - kMateScoreMargin) {
            alpha = std::max(-search_infinity, result.score - window);
            beta = std::min(search_infinity, result.score + window);
        }
        int retries = 0;
        bool completed = false;
        while (!stopped_) {
            if (!search_root(state, depth, alpha, beta, depth_move, depth_score))
                break;
            if (depth_score > alpha && depth_score < beta) {
                completed = true;
                break;
            }
            ++stats_.aspiration_retries;
            ++retries;
            if (retries > options.aspiration_max_retries ||
                std::chrono::steady_clock::now() >= soft_deadline_) {
                alpha = -search_infinity;
                beta = search_infinity;
            } else {
                window *= 2;
                if (depth_score <= alpha)
                    alpha = std::max(-search_infinity, depth_score - window);
                if (depth_score >= beta)
                    beta = std::min(search_infinity, depth_score + window);
            }
            if (alpha == -search_infinity && beta == search_infinity &&
                retries > options.aspiration_max_retries + 1)
                break;
        }
        if (!completed || stopped_)
            break;
        const auto depth_end = std::chrono::steady_clock::now();
        result.best_move = depth_move;
        result.score = depth_score;
        result.pv_line = options_.benchmark_instrumentation_enabled ? pv_pool_[0]
                                                                    : std::vector<Move>{depth_move};
        stats_.depth_reached = depth;
        stats_.completed_depths.push_back(DepthReport{depth,
                                                      depth_score,
                                                      depth_move,
                                                      stats_.searched_nodes - nodes_before,
                                                      elapsed_milliseconds(depth_start, depth_end),
                                                      result.pv_line});
        if (std::abs(depth_score) >= mate_score - kMateStopMargin)
            break;
        if (!options.iterative_deepening_enabled)
            break;
    }
    const auto end = std::chrono::steady_clock::now();
    stats_.elapsed_ms = elapsed_milliseconds(start_, end);
    stats_.timeout_hit = stopped_;
    stats_.timeout_depth = stopped_ ? stats_.started_depth : 0;
    result.stats = stats_;
    return result;
}

SearchResult AlphaBetaEngine::search_fixed_depth(const State& root, int depth, bool use_tt) {
    SearchOptions options;
    options.max_depth = std::max(1, depth);
    options.time_limit_ms = 60'000;
    options.soft_time_limit_ms = 60'000;
    options.hard_time_limit_ms = 60'000;
    options.use_tt = use_tt;
    return find_best_move(root, options);
}

}  // namespace qas

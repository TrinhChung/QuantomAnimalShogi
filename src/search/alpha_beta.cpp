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

/// @brief Counts set bits in an animal-form mask.
/// @param mask Mask to inspect.
/// @return Number of candidate forms.
int popcount(Mask mask) {
    int count = 0;
    for (; mask != 0; mask = static_cast<Mask>(mask & (mask - 1)))
        ++count;
    return count;
}

/// @brief Counts set squares in a board bitmask.
/// @param mask Board bitmask to inspect.
/// @return Number of set board squares.
int popcount_board(std::uint16_t mask) {
    int count = 0;
    for (; mask != 0; mask = static_cast<std::uint16_t>(mask & (mask - 1)))
        ++count;
    return count;
}

/// @brief Computes mean material value across candidate forms.
/// @param mask Candidate-form mask.
/// @return Integer mean value, or zero for an empty mask.
int expected_piece_value(Mask mask) {
    constexpr std::array<int, 5> values{100, 320, 310, 1050, 420};
    int total = 0;
    int count = 0;
    for (Animal form : all_forms) {
        if (contains(mask, form)) {
            total += values[static_cast<std::size_t>(form)];
            ++count;
        }
    }
    return count == 0 ? 0 : total / count;
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

/// @brief Counts locally reachable destinations without full transition validation.
/// @param state Source state.
/// @param side Side whose mobility is counted.
/// @return Geometric mobility count.
int geometric_mobility(const State& state, Side side) {
    int result = 0;
    const auto& tables = move_tables();
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!owned_by(state, piece, side))
            continue;
        if (is_hand_position(state.pos[piece])) {
            for (int square = 0; square < board_size; ++square) {
                if (state.board[square] == kEmptyBoardSquare)
                    ++result;
            }
            continue;
        }
        const auto destinations =
            tables.quantum_move_table[state.mask[piece]][side_index(side)][state.pos[piece]];
        for (int square = 0; square < board_size; ++square) {
            if ((destinations & (1U << square)) == 0)
                continue;
            const int occupant = state.board[square];
            if (occupant < 0 || !owned_by(state, occupant, side))
                ++result;
        }
    }
    return result;
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
        if (applied && (copy.terminal == Terminal::Catch || copy.terminal == Terminal::Try)) {
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
    entry.key = key;
    entry.depth = static_cast<std::int16_t>(depth);
    entry.score = score_to_tt(score, ply);
    entry.flag = static_cast<std::uint8_t>(bound);
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

AlphaBetaEngine::EvalScratch::EvalScratch() {
    pseudo_moves.reserve(128);
    immediate_candidates.reserve(32);
}

int AlphaBetaEngine::evaluate_baseline(const State& state, EvalComponentProfile* profile) const {
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->terminal);
        if (state.terminal != Terminal::None)
            return terminal_score(state, 0);
    }

    int south_score = 0;
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->material_mask);
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const Side owner = owned_by(state, piece, Side::South) ? Side::South : Side::North;
            const int sign = owner == Side::South ? 1 : -1;
            const Mask mask = state.mask[piece];
            south_score += sign * expected_piece_value(mask);
            south_score += sign * (popcount(mask) - 1) * 12;
        }
    }
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->lion_safety);
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const Side owner = owned_by(state, piece, Side::South) ? Side::South : Side::North;
            const int sign = owner == Side::South ? 1 : -1;
            const Mask mask = state.mask[piece];
            if (!contains(mask, Animal::Lion))
                continue;
            const bool on_board = state.pos[piece] < board_size;
            if (on_board && is_square_attacked(state, state.pos[piece], opposite(owner)))
                south_score -= sign * (mask == bit(Animal::Lion) ? 700 : 180);
            if (on_board) {
                south_score += sign * progress_to_try(state.pos[piece], owner) *
                               (mask == bit(Animal::Lion) ? 90 : 25);
            }
            if (mask == bit(Animal::Lion) && !on_board)
                south_score -= sign * 900;
        }
        south_score += 80 * (lion_candidate_count(state, Side::South) -
                             lion_candidate_count(state, Side::North));
    }
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->mobility);
        south_score +=
            18 * (geometric_mobility(state, Side::South) - geometric_mobility(state, Side::North));
    }

    if (side_has_immediate_win(state, Side::South, profile))
        south_score += 3500;
    if (side_has_immediate_win(state, Side::North, profile))
        south_score -= 3500;
    return state.side_to_move == Side::South ? south_score : -south_score;
}

int AlphaBetaEngine::evaluate_optimized(const State& state,
                                        EvalScratch& scratch,
                                        EvalComponentProfile* profile) const {
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->terminal);
        if (state.terminal != Terminal::None)
            return terminal_score(state, 0);
    }

    int south_score = 0;
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->material_mask);
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const int owner = (state.owner_bits >> piece) & 1U;
            const int sign = owner == 0 ? 1 : -1;
            const Mask mask = state.mask[piece];
            south_score += sign * kExpectedPieceValues[mask];
            south_score += sign * (kMaskPopcounts[mask] - 1) * 12;
        }
    }

    const auto& tables = move_tables();
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->mobility);
        scratch.occupancy = {};
        scratch.mobility = {};
        int empty_count = 0;
        for (int square = 0; square < board_size; ++square) {
            const int piece = state.board[square];
            if (piece < 0) {
                ++empty_count;
            } else {
                const int owner = (state.owner_bits >> piece) & 1U;
                scratch.occupancy[owner] |= static_cast<std::uint16_t>(1U << square);
            }
        }
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const int owner = (state.owner_bits >> piece) & 1U;
            if (is_hand_position(state.pos[piece])) {
                scratch.mobility[owner] += empty_count;
                continue;
            }
            const std::uint16_t destinations =
                tables.quantum_move_table[state.mask[piece]][owner][state.pos[piece]];
            const std::uint16_t available = static_cast<std::uint16_t>(
                destinations & static_cast<std::uint16_t>(~scratch.occupancy[owner]) & kBoardMask);
            scratch.mobility[owner] += popcount_board(available);
        }
        south_score += 18 * (scratch.mobility[0] - scratch.mobility[1]);
    }
    {
        ComponentTimer timer(profile == nullptr ? nullptr : &profile->lion_safety);
        scratch.attack_mask = {};
        scratch.lion_candidates = {};
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const int owner = (state.owner_bits >> piece) & 1U;
            if (state.pos[piece] < board_size) {
                scratch.attack_mask[owner] |=
                    tables.quantum_move_table[state.mask[piece]][owner][state.pos[piece]];
            }
            if (contains(state.mask[piece], Animal::Lion))
                ++scratch.lion_candidates[owner];
        }
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            const Mask mask = state.mask[piece];
            if (!contains(mask, Animal::Lion))
                continue;
            const int owner = (state.owner_bits >> piece) & 1U;
            const int sign = owner == 0 ? 1 : -1;
            const bool on_board = state.pos[piece] < board_size;
            if (on_board && (scratch.attack_mask[1 - owner] & (1U << state.pos[piece])) != 0) {
                south_score -= sign * (mask == bit(Animal::Lion) ? 700 : 180);
            }
            if (on_board) {
                const Side owner_side = owner == 0 ? Side::South : Side::North;
                south_score += sign * progress_to_try(state.pos[piece], owner_side) *
                               (mask == bit(Animal::Lion) ? 90 : 25);
            }
            if (mask == bit(Animal::Lion) && !on_board)
                south_score -= sign * 900;
        }
        south_score += 80 * (scratch.lion_candidates[0] - scratch.lion_candidates[1]);
    }

    auto has_immediate_win = [&](Side side) {
        State copy;
        {
            ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_setup);
            copy = state;
            copy.side_to_move = side;
            copy.terminal = Terminal::None;
            copy.winner = kNoWinner;
            recompute_hash(copy);
        }
        {
            ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_movegen);
            generate_pseudo_legal_moves(copy, scratch.pseudo_moves);
        }
        {
            ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_filter);
            scratch.immediate_candidates.clear();
            const int back_rank = side == Side::South ? 0 : board_height - 1;
            for (const Move& move : scratch.pseudo_moves) {
                const int target = state.board[move.to];
                const bool can_catch = target >= 0 && contains(state.mask[target], Animal::Lion);
                const bool can_try = contains(state.mask[move.piece], Animal::Lion) &&
                                     move.to / board_width == back_rank;
                if (can_catch || can_try)
                    scratch.immediate_candidates.push_back(move);
            }
        }
        for (const Move& move : scratch.immediate_candidates) {
            Undo undo;
            bool applied = false;
            {
                ComponentTimer timer(profile == nullptr ? nullptr : &profile->immediate_transition);
                applied = apply_move(copy, move, undo);
            }
            if (applied && (copy.terminal == Terminal::Catch || copy.terminal == Terminal::Try))
                return true;
            if (applied)
                undo_move(copy, undo);
        }
        return false;
    };

    if (has_immediate_win(Side::South))
        south_score += 3500;
    if (has_immediate_win(Side::North))
        south_score -= 3500;
    return state.side_to_move == Side::South ? south_score : -south_score;
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
    int score = 0;
    if (options_.killer_enabled && ply < static_cast<int>(killers_.size())) {
        if (move == killers_[ply][0])
            score += 180'000;
        else if (move == killers_[ply][1])
            score += 160'000;
    }
    if (options_.history_enabled) {
        score += history_[side_index(state.side_to_move)][move.from][move.to];
    }
    const int target = state.board[move.to];
    if (target >= 0 && options_.capture_ordering_enabled) {
        score += 250'000 + expected_piece_value(state.mask[target]) * 20;
        if (state.mask[target] == bit(Animal::Lion))
            score += 1'000'000;
    }

    const Mask move_mask =
        move.from < board_size
            ? move_tables().move_possible_mask[side_index(state.side_to_move)][move.from][move.to]
            : state.mask[move.piece];
    if (options_.mask_collapse_ordering_enabled) {
        score += (popcount(state.mask[move.piece]) -
                  popcount(static_cast<Mask>(state.mask[move.piece] & move_mask))) *
                 12'000;
    }
    if (options_.try_threat_ordering_enabled && contains(state.mask[move.piece], Animal::Lion) &&
        move.to / board_width == (state.side_to_move == Side::South ? 0 : board_height - 1)) {
        score += 30'000;
    }
    if (!options_.strong_ordering_enabled || ply > 0)
        return score;

    State after = state;
    Undo undo;
    if (!apply_search_move(after, move, undo))
        return -2'000'000;
    if (options_.immediate_win_ordering_enabled &&
        (after.terminal == Terminal::Catch || after.terminal == Terminal::Try)) {
        return 1'500'000 + score;
    }
    const int before_lions = lion_candidate_count(state, after.side_to_move);
    const int after_lions = lion_candidate_count(after, after.side_to_move);
    if (options_.lion_reduction_ordering_enabled)
        score += (before_lions - after_lions) * 70'000;
    if (options_.prevent_loss_ordering_enabled &&
        side_has_immediate_win(state, opposite(state.side_to_move), nullptr) &&
        !side_has_immediate_win(after, after.side_to_move, nullptr)) {
        score += 400'000;
    }
    if (after.pos[move.piece] < board_size && contains(after.mask[move.piece], Animal::Lion)) {
        score += progress_to_try(after.pos[move.piece], state.side_to_move) * 4000;
        if (after.mask[move.piece] == bit(Animal::Lion) &&
            is_square_attacked(after, after.pos[move.piece], after.side_to_move)) {
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
    auto& scored = score_pool_[std::min<std::size_t>(ply, kMaxSearchPly - 1)];
    scored.clear();
    for (const Move& move : moves) {
        scored.emplace_back(move_order_score(state, move, tt_move, ply), move);
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
    if (depth == 0)
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

    int best = -search_infinity;
    Move best_move;
    bool first = true;
    int move_index = 0;
    for (const Move& move : moves) {
        Undo undo;
        if (!apply_search_move(state, move, undo))
            continue;
        int score = 0;
        if (first || !options_.pvs_enabled) {
            score = -negamax(state, depth - 1, -beta, -alpha, ply + 1);
        } else {
            score = -negamax(state, depth - 1, -alpha - 1, -alpha, ply + 1);
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
            best_move = move;
            if (options_.benchmark_instrumentation_enabled)
                update_pv(ply, move);
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            ++stats_.cutoffs;
            stats_.cutoff_rank_sum += static_cast<std::uint64_t>(move_index + 1);
            if (move_index == 0)
                ++stats_.first_move_cutoffs;
            if (move == tt_move)
                ++stats_.tt_move_cutoffs;
            const bool quiet = state.board[move.to] == kEmptyBoardSquare;
            if (quiet && options_.killer_enabled && ply < static_cast<int>(killers_.size())) {
                if (killers_[ply][0] != move) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = move;
                }
                ++stats_.killer_cutoffs;
            }
            if (quiet && options_.history_enabled) {
                int& history = history_[side_index(state.side_to_move)][move.from][move.to];
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
            local_best = move;
            if (options_.benchmark_instrumentation_enabled)
                update_pv(0, move);
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

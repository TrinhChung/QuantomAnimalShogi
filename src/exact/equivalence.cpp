#include "exact/equivalence.hpp"

#include <algorithm>
#include <chrono>
#include <tuple>

#include "core/timing.hpp"

namespace qas {
namespace {

constexpr unsigned kCanonicalPositionBitCount = 5;
constexpr unsigned kCanonicalOwnerShift = kCanonicalPositionBitCount;
constexpr unsigned kCanonicalMaskShift = kCanonicalOwnerShift + 1;
constexpr unsigned kBoardOriginShift = 1;
constexpr unsigned kBoardOwnerShift = kBoardOriginShift + 1;
constexpr unsigned kBoardMaskShift = kBoardOwnerShift + 1;
constexpr std::uint8_t kOccupiedSquareBit = 1U;

static_assert(external_source_count <= (1U << kCanonicalPositionBitCount),
              "canonical position field is too narrow");

/// @brief Encodes canonical position, owner and mask fields for one physical piece.
/// @param state Source state.
/// @param piece Physical piece index.
/// @return Compact sortable tuple with canonicalized hand position.
std::uint16_t piece_tuple(const State& state, int piece) {
    const unsigned canonical_position =
        is_hand_position(state.pos[piece]) ? first_hand_slot : state.pos[piece];
    return static_cast<std::uint16_t>(
        canonical_position |
        (owned_by(state, piece, Side::North) ? 1U << kCanonicalOwnerShift : 0U) |
        (static_cast<unsigned>(state.mask[piece]) << kCanonicalMaskShift));
}

/// @brief Ranks equivalent moves when selecting a class representative.
/// @param state Source state.
/// @param move Candidate representative.
/// @param preferred Caller-preferred move.
/// @return Higher-is-preferred deterministic score.
int representative_score(const State& state, const Move& move, const Move& preferred) {
    if (move == preferred)
        return 2'000'000;
    int score = 0;
    const int captured = state.board[move.to];
    if (captured >= 0) {
        score += 200'000;
        if (state.mask[captured] == bit(Animal::Lion))
            score += 1'000'000;
    }
    return score - (static_cast<int>(move.from) * board_size + move.to);
}

/// @brief Mixes one canonical field into an accumulated hash.
/// @param seed Hash accumulator to mutate.
/// @param value Field value to mix.
void hash_combine(std::size_t& seed, std::uint64_t value) {
    seed ^= static_cast<std::size_t>(value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

/// @brief Detects interchangeable hand pieces that may yield duplicate drops.
/// @param state Source state.
/// @return `true` when two movable hand pieces share owner, origin and mask.
bool has_duplicate_drop_hint(const State& state) {
    for (int left = 0; left < physical_piece_count; ++left) {
        if (!is_hand_position(state.pos[left]) || !owned_by(state, left, state.side_to_move))
            continue;
        for (int right = left + 1; right < physical_piece_count; ++right) {
            if (is_hand_position(state.pos[right]) && owned_by(state, right, state.side_to_move) &&
                state.mask[left] == state.mask[right] &&
                originated_from(state, left, Side::North) ==
                    originated_from(state, right, Side::North)) {
                return true;
            }
        }
    }
    return false;
}

/// @brief Applies configured gates and retains one move per equivalent successor class.
/// @param state Source state.
/// @param legal_moves Legal moves before reduction.
/// @param preferred Preferred representative move.
/// @param threshold Minimum raw branching threshold.
/// @param min_depth Minimum remaining depth for reduction.
/// @param depth_remaining Current remaining search depth.
/// @param minimum_duplicate_ratio Minimum observed reduction ratio.
/// @param require_hint Whether a duplicate-hand hint is mandatory.
/// @param low_time Whether the caller is in low-time mode.
/// @param applied Receives whether reduction passed all gates.
/// @param stats Optional reduction metrics sink.
/// @return Reduced representatives, or the original legal moves when gated off.
std::vector<Move> reduce_successors(const State& state,
                                    const std::vector<Move>& legal_moves,
                                    const Move& preferred,
                                    std::size_t threshold,
                                    int min_depth,
                                    int depth_remaining,
                                    double minimum_duplicate_ratio,
                                    bool require_hint,
                                    bool low_time,
                                    bool& applied,
                                    SuccessorReductionStats* stats) {
    const bool trigger = !require_hint || threshold == 0 || has_duplicate_drop_hint(state);
    applied =
        !low_time && depth_remaining >= min_depth && legal_moves.size() >= threshold && trigger;
    if (!applied)
        return legal_moves;
    const auto classes =
        generate_equivalent_successor_classes(state, legal_moves, preferred, stats);
    const double duplicate_ratio =
        legal_moves.empty()
            ? 0.0
            : 1.0 - static_cast<double>(classes.size()) / static_cast<double>(legal_moves.size());
    if (duplicate_ratio < minimum_duplicate_ratio) {
        applied = false;
        return legal_moves;
    }
    std::vector<Move> representatives;
    representatives.reserve(classes.size());
    for (const SuccessorClass& item : classes)
        representatives.push_back(item.representative);
    return representatives;
}

}  // namespace

bool CanonKey::operator==(const CanonKey& other) const {
    return board == other.board && pieces == other.pieces && side_to_move == other.side_to_move &&
           turn == other.turn && terminal == other.terminal && winner == other.winner;
}

std::size_t CanonKeyHash::operator()(const CanonKey& key) const {
    std::size_t hash = 0x514153U;
    for (std::uint8_t value : key.board)
        hash_combine(hash, value);
    for (std::uint16_t value : key.pieces)
        hash_combine(hash, value);
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
        if (piece < 0)
            continue;
        const unsigned origin = originated_from(state, piece, Side::North) ? 1U : 0U;
        const unsigned owner = owned_by(state, piece, Side::North) ? 1U : 0U;
        key.board[square] = static_cast<std::uint8_t>(
            kOccupiedSquareBit | (origin << kBoardOriginShift) | (owner << kBoardOwnerShift) |
            (state.mask[piece] << kBoardMaskShift));
    }

    for (int origin = 0; origin < static_cast<int>(kSideCount); ++origin) {
        std::array<std::uint16_t, kPiecesPerOrigin> tuples{};
        int count = 0;
        const Side side = origin == 0 ? Side::South : Side::North;
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            if (originated_from(state, piece, side))
                tuples[count++] = piece_tuple(state, piece);
        }
        std::sort(tuples.begin(), tuples.end());
        std::copy(tuples.begin(), tuples.end(), key.pieces.begin() + origin * kPiecesPerOrigin);
    }
    key.side_to_move = state.side_to_move;
    key.turn = state.turn;
    key.terminal = state.terminal;
    key.winner = state.winner;
    return key;
}

std::vector<SuccessorClass> generate_equivalent_successor_classes(
    const State& state,
    const std::vector<Move>& legal_moves,
    const Move& preferred,
    SuccessorReductionStats* stats) {
    std::vector<SuccessorClass> classes;
    classes.reserve(legal_moves.size());
    for (const Move& move : legal_moves) {
        State successor = state;
        Undo undo;
        RuleMetrics rule_metrics;
        const bool applied = stats == nullptr
                                 ? apply_move(successor, move, undo)
                                 : apply_move_profiled(successor, move, undo, rule_metrics);
        if (!applied)
            continue;
        if (stats != nullptr) {
            stats->propagation_calls += rule_metrics.propagation_calls;
            stats->propagation_ms += rule_metrics.propagation_ms;
        }
        CanonKey key;
        if (stats != nullptr) {
            const auto begin = std::chrono::steady_clock::now();
            key = canonical_key(successor);
            const auto end = std::chrono::steady_clock::now();
            ++stats->canonicalize_calls;
            stats->canonicalize_ms += elapsed_milliseconds(begin, end);
        } else {
            key = canonical_key(successor);
        }
        auto found = std::find_if(classes.begin(),
                                  classes.end(),
                                  [&key](const SuccessorClass& item) { return item.key == key; });
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

void enable_successor_equivalence(SearchOptions& options,
                                  std::size_t threshold,
                                  bool require_duplicate_hint) {
    options.successor_reducer = reduce_successors;
    options.reducer_threshold = threshold;
    options.reducer_require_duplicate_hint = require_duplicate_hint;
}

}  // namespace qas

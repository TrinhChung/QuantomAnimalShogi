#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rules/game.hpp"
#include "search/alpha_beta.hpp"

namespace qas {

struct CanonKey {
    // Each occupied square stores origin/owner/mask; zero means empty.
    std::array<std::uint8_t, board_size> board{};
    // Piece tuples sorted within each origin group: position/owner/mask.
    std::array<std::uint16_t, physical_piece_count> pieces{};
    Side side_to_move{Side::South};
    std::uint16_t turn{0};
    Terminal terminal{Terminal::None};
    std::int8_t winner{kNoWinner};

    /// @brief Compares every canonical successor field.
    /// @param other Key to compare.
    /// @return `true` when both keys describe the same successor semantics.
    bool operator==(const CanonKey& other) const;

    /// @brief Tests canonical successor keys for inequality.
    /// @param other Key to compare.
    /// @return `true` when at least one canonical field differs.
    bool operator!=(const CanonKey& other) const { return !(*this == other); }
};

struct CanonKeyHash {
    /// @brief Hashes every value-relevant field of a canonical successor key.
    /// @param key Canonical key to hash.
    /// @return Hash value suitable for standard unordered containers.
    std::size_t operator()(const CanonKey& key) const;
};

struct SuccessorClass {
    CanonKey key{};
    Move representative{};
    std::size_t multiplicity{0};
};

/// @brief Canonicalizes physical piece labels while preserving successor semantics.
/// @param state Fully propagated game state.
/// @return Canonical key including board, pieces, side, turn and terminal result.
CanonKey canonical_key(const State& state);

/// @brief Groups legal moves that produce identical canonical successors.
/// @param state Source game state.
/// @param legal_moves Moves already validated as legal in the source state.
/// @param preferred Move favored when selecting a class representative.
/// @param stats Optional sink for canonicalization and propagation metrics.
/// @return Successor classes in first-occurrence order.
/// @note Each move is applied and propagated before its canonical key is computed.
std::vector<SuccessorClass> generate_equivalent_successor_classes(
    const State& state,
    const std::vector<Move>& legal_moves,
    const Move& preferred = {},
    SuccessorReductionStats* stats = nullptr);

/// @brief Installs canonical successor reduction into search options.
/// @param options Search options to mutate.
/// @param threshold Minimum legal-move count that permits reduction.
/// @param require_duplicate_hint Whether reduction requires a duplicate-hand hint.
void enable_successor_equivalence(SearchOptions& options,
                                  std::size_t threshold = 12,
                                  bool require_duplicate_hint = true);

}  // namespace qas

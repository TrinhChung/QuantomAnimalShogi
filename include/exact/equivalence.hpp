#pragma once

#include "rules/game.hpp"
#include "search/alpha_beta.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qas {

struct CanonKey {
    // Each occupied square stores origin/owner/mask; zero means empty.
    std::array<std::uint8_t, board_size> board{};
    // Piece tuples sorted within each origin group: position/owner/mask.
    std::array<std::uint16_t, physical_piece_count> pieces{};
    Side side_to_move{Side::South};
    std::uint16_t turn{0};
    Terminal terminal{Terminal::None};
    std::int8_t winner{-1};

    bool operator==(const CanonKey& other) const;
    bool operator!=(const CanonKey& other) const { return !(*this == other); }
};

struct CanonKeyHash {
    std::size_t operator()(const CanonKey& key) const;
};

struct SuccessorClass {
    CanonKey key{};
    Move representative{};
    std::size_t multiplicity{0};
};

CanonKey canonical_key(const State& state);

// Every input must already be legal. apply_move performs propagation before keying.
std::vector<SuccessorClass> generate_equivalent_successor_classes(
    const State& state, const std::vector<Move>& legal_moves, const Move& preferred = {});

void enable_successor_equivalence(SearchOptions& options, std::size_t threshold = 12,
                                  bool require_duplicate_hint = true);

}  // namespace qas

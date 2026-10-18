#pragma once

#include <array>

#include "rules/game.hpp"

namespace qas {

/// Sparse NNUE input encoding, from the side to move's perspective (board rotated for North).
/// One active feature per physical piece:
///   index = ((location * 2 + is_theirs) * 2 + is_their_origin) * 32 + mask
/// where location is the board square in the mover's orientation (0..11) or 12 for a hand piece.
inline constexpr int kNnueLocations = board_size + 1;
inline constexpr int kNnueFeatureCount = kNnueLocations * 2 * 2 * 32;
using NnueActive = std::array<int, physical_piece_count>;

/// @brief Computes the eight active input features of a state.
/// @param state Position to encode; not mutated.
/// @return One feature index per physical piece, each in [0, kNnueFeatureCount).
NnueActive nnue_active_features(const State& state);

}  // namespace qas

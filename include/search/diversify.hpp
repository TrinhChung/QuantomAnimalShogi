#pragma once

#include <optional>
#include <random>

#include "rules/game.hpp"
#include "search/alpha_beta.hpp"

namespace qas {

/// @brief Test-only opening diversification: ranks every legal move by a fixed-depth search of
///        its child and picks uniformly among the moves scoring within `margin` of the best one.
/// @param engine Engine used for the child searches (its table is reused).
/// @param state Position to move from.
/// @param options Search options (evaluation settings); depth and time are overridden.
/// @param depth Fixed search depth for each child.
/// @param margin Accepted score loss in centipawns versus the best move (0 keeps only the best).
/// @param random Random source.
/// @return Chosen move, or nothing when the position has no legal move.
std::optional<Move> diversified_move(AlphaBetaEngine& engine,
                                     const State& state,
                                     const SearchOptions& options,
                                     int depth,
                                     int margin,
                                     std::mt19937& random);

}  // namespace qas

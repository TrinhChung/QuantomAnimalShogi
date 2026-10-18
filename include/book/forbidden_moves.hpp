#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rules/game.hpp"

namespace qas {

/// @brief Offline-proven losing moves, excluded from the root search (never forces a move).
/// @note Uses the same label-independent keys and move encoding as `PositionBook`, so keys must be
///       built from the protocol-normalized view (side to move is South).
class ForbiddenMoves {
   public:
    /// @brief Loads `<key> <move>` lines (several lines may share a key); `#` starts a comment.
    /// @param path File path.
    /// @param error Optional sink for the first parse problem.
    /// @return `true` when the file was read (possibly with zero entries).
    bool load(const std::string& path, std::string* error = nullptr);

    /// @brief Adds one forbidden move for a position.
    void add(const std::string& key, const std::string& move);

    /// @brief Returns the legal moves of a state that are listed as forbidden.
    std::vector<Move> lookup(const State& state) const;

    /// @brief Number of positions with at least one forbidden move.
    std::size_t size() const { return entries_.size(); }

   private:
    std::unordered_map<std::string, std::unordered_set<std::string>> entries_;
};

}  // namespace qas

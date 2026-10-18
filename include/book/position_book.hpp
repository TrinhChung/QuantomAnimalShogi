#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

#include "rules/game.hpp"

namespace qas {

/// @brief Precomputed position-to-move table (opening and known-loss positions).
/// @note Keys are label-independent canonical successor keys, so a position matches regardless
///       of how physical pieces were numbered when the state was built (for example from a
///       protocol observation). Moves are stored by squares, and drops by the dropped piece's
///       origin and identity mask, so they too are independent of piece numbering. A stored move
///       is returned only if it is legal in the queried state.
class PositionBook {
   public:
    /// @brief Loads entries from a text file of `<key> <move>` lines; `#` starts a comment.
    /// @param path File path.
    /// @param error Optional sink for the first parse problem.
    /// @return `true` when the file was read (possibly with zero entries).
    bool load(const std::string& path, std::string* error = nullptr);

    /// @brief Adds or replaces one entry.
    /// @param key Canonical key text from `key()`.
    /// @param move Move text from `encode_move()`.
    /// @return `false` if the move text is malformed.
    bool add(const std::string& key, const std::string& move);

    /// @brief Looks up a legal book move for a state.
    /// @param state Position to decide.
    /// @return Matching legal move, or nothing.
    std::optional<Move> lookup(const State& state) const;

    /// @brief Number of stored positions.
    std::size_t size() const { return entries_.size(); }

    /// @brief Builds the label-independent key text of a state.
    static std::string key(const State& state);

    /// @brief Encodes a legal move label-independently.
    /// @param state Position the move belongs to.
    /// @param move Move to encode.
    /// @return `b<from>-<to>` for board moves or `h<S|N><mask>-<to>` for drops.
    static std::string encode_move(const State& state, const Move& move);

   private:
    std::unordered_map<std::string, std::string> entries_;
};

}  // namespace qas

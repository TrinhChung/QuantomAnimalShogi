#pragma once

#include <cstddef>

#include "search/alpha_beta.hpp"

namespace qas {

/// @brief Configuration of the concurrent df-pn root guard.
struct GuardOptions {
    bool enabled{true};
    int max_ply{23};           ///< df-pn proof horizon from each root.
    int reserve_cap_ms{2500};  ///< Upper bound of the re-search reserve.
    std::size_t own_table_entries{std::size_t{1} << 20U};     ///< Own-win solver table entries.
    std::size_t refute_table_entries{std::size_t{1} << 22U};  ///< Refutation solver table entries.
};

/// @brief Telemetry of one guarded decision.
struct GuardReport {
    int excluded_moves{0};
    bool own_forced_win{false};
    bool re_searched{false};
};

/// @brief Chooses a root move with alpha-beta while a worker thread runs df-pn.
/// @param engine Alpha-beta engine used on the calling thread.
/// @param root Position to decide.
/// @param options Alpha-beta options; their hard limit is the total decision budget.
/// @param guard Guard configuration; when disabled this is a plain `find_best_move` call.
/// @param report Optional telemetry sink.
/// @return Search result whose best move is legal and, whenever possible, not proven losing.
/// @note The worker first tries to prove an own forced win (played immediately when found), then
///       repeatedly tries to prove an opponent forced win after root moves, current alpha-beta best
///       first. Proven-losing moves become live exclusions for alpha-beta's next iteration. A small
///       reserve lets alpha-beta re-search if its final move was refuted late. The worker is joined
///       before returning, so no CPU is used after the decision.
SearchResult guarded_find_best_move(AlphaBetaEngine& engine,
                                    const State& root,
                                    const SearchOptions& options,
                                    const GuardOptions& guard,
                                    GuardReport* report = nullptr);

}  // namespace qas

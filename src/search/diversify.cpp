#include "search/diversify.hpp"

#include <algorithm>
#include <vector>

namespace qas {

std::optional<Move> diversified_move(AlphaBetaEngine& engine,
                                     const State& state,
                                     const SearchOptions& options,
                                     int depth,
                                     int margin,
                                     std::mt19937& random) {
    SearchOptions fixed = options;
    fixed.max_depth = depth;
    fixed.time_limit_ms = fixed.soft_time_limit_ms = fixed.hard_time_limit_ms = 600'000;
    fixed.root_guard = nullptr;
    std::vector<std::pair<Move, int>> scored;
    for (const Move& move : generate_legal_moves(state)) {
        State child = state;
        Undo undo;
        if (!apply_move(child, move, undo))
            continue;
        int score;
        if (child.terminal != Terminal::None) {
            score = child.terminal == Terminal::Draw                 ? 0
                    : child.winner == side_index(state.side_to_move) ? mate_score
                                                                     : -mate_score;
        } else {
            score = -engine.find_best_move(child, fixed).score;
        }
        scored.emplace_back(move, score);
    }
    if (scored.empty())
        return std::nullopt;
    int best = scored.front().second;
    for (const auto& [move, score] : scored)
        best = std::max(best, score);
    std::vector<Move> pool;
    for (const auto& [move, score] : scored) {
        if (score >= best - std::max(0, margin))
            pool.push_back(move);
    }
    std::uniform_int_distribution<std::size_t> pick(0, pool.size() - 1);
    return pool[pick(random)];
}

}  // namespace qas

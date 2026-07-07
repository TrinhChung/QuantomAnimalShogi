#include "search/alpha_beta.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

qas::State deterministic_position(int plies, int salt) {
    qas::State state = qas::initial_state();
    for (int ply = 0; ply < plies && state.terminal == qas::Terminal::None; ++ply) {
        auto moves = qas::generate_legal_moves(state);
        if (moves.empty()) break;
        std::sort(moves.begin(), moves.end(), [](const qas::Move& left, const qas::Move& right) {
            return left.from * qas::board_size + left.to <
                   right.from * qas::board_size + right.to;
        });
        const std::size_t index = static_cast<std::size_t>((salt + ply * 7) % moves.size());
        qas::Undo undo;
        if (!qas::apply_move(state, moves[index], undo)) std::abort();
    }
    if (state.terminal != qas::Terminal::None) return qas::initial_state();
    return state;
}

bool legal_result(const qas::State& state, const qas::SearchResult& result) {
    const auto legal = qas::generate_legal_moves(state);
    return result.has_move &&
           std::find(legal.begin(), legal.end(), result.best_move) != legal.end();
}

}  // namespace

int main(int argc, char** argv) {
    const int depth = argc >= 2 ? std::max(1, std::atoi(argv[1])) : 4;
    const std::vector<std::pair<std::string, qas::State>> positions{
        {"initial", qas::initial_state()},
        {"early_4ply", deterministic_position(4, 1)},
        {"middle_9ply", deterministic_position(9, 3)}};

    std::cout << "mode,position,depth,score,legal,nodes,expanded,cutoffs,avg_L,tt_hits,tt_probes,time_ms,best\n";
    for (bool use_tt : {false, true}) {
        for (const auto& item : positions) {
            qas::AlphaBetaEngine engine(1U << 18U);
            const auto result = engine.search_fixed_depth(item.second, depth, use_tt);
            std::cout << (use_tt ? "AB+TT" : "AB") << ',' << item.first << ',' << depth
                      << ',' << result.score << ',' << (legal_result(item.second, result) ? 1 : 0)
                      << ',' << result.stats.searched_nodes << ',' << result.stats.expanded_nodes
                      << ',' << result.stats.cutoffs << ',' << std::fixed << std::setprecision(3)
                      << result.stats.average_legal_moves() << ',' << result.stats.tt_hits << ','
                      << result.stats.tt_probes << ',' << result.stats.elapsed_ms << ','
                      << qas::move_string(result.best_move) << '\n';
        }
    }
    return 0;
}

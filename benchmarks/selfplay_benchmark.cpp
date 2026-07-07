#include "search/alpha_beta.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

int main(int argc, char** argv) {
    const int games = argc >= 2 ? std::max(2, std::atoi(argv[1])) : 20;
    const int depth = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 3;
    int wins = 0;
    int losses = 0;
    int draws = 0;
    int illegal = 0;
    std::mt19937 random(0x514153U);

    for (int game = 0; game < games; ++game) {
        qas::State state = qas::initial_state();
        const qas::Side engine_side = game % 2 == 0 ? qas::Side::South : qas::Side::North;
        qas::AlphaBetaEngine engine(1U << 16U);
        while (state.terminal == qas::Terminal::None) {
            const auto legal = qas::generate_legal_moves(state);
            if (legal.empty()) {
                if (state.side_to_move == engine_side) ++losses;
                else ++wins;
                break;
            }
            qas::Move selected;
            if (state.side_to_move == engine_side) {
                const auto result = engine.search_fixed_depth(state, depth, true);
                selected = result.best_move;
                if (!result.has_move ||
                    std::find(legal.begin(), legal.end(), selected) == legal.end()) {
                    ++illegal;
                    ++losses;
                    break;
                }
            } else {
                std::uniform_int_distribution<std::size_t> distribution(0, legal.size() - 1);
                selected = legal[distribution(random)];
            }
            qas::Undo undo;
            if (!qas::apply_move(state, selected, undo)) {
                ++illegal;
                if (state.side_to_move == engine_side) ++losses;
                else ++wins;
                break;
            }
            if (state.terminal != qas::Terminal::None) {
                if (state.terminal == qas::Terminal::Draw) ++draws;
                else if (state.winner == qas::side_index(engine_side)) ++wins;
                else ++losses;
            }
        }
    }
    std::cout << "games=" << games << " depth=" << depth << " wins=" << wins
              << " losses=" << losses << " draws=" << draws << " illegal=" << illegal
              << '\n';
    return illegal == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

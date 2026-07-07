#include "search/alpha_beta.hpp"
#include "exact/equivalence.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool has_immediate_win(const qas::State& state) {
    for (const qas::Move& move : qas::generate_legal_moves(state)) {
        if (qas::is_immediate_winning_move(state, move)) return true;
    }
    return false;
}

qas::State safe_position(int plies, int salt) {
    qas::State state = qas::initial_state();
    for (int ply = 0; ply < plies; ++ply) {
        auto moves = qas::generate_legal_moves(state);
        std::sort(moves.begin(), moves.end(), [](const qas::Move& left, const qas::Move& right) {
            return left.from * qas::board_size + left.to <
                   right.from * qas::board_size + right.to;
        });
        bool selected = false;
        for (std::size_t offset = 0; offset < moves.size(); ++offset) {
            const std::size_t index =
                (static_cast<std::size_t>(salt + ply * 7) + offset) % moves.size();
            qas::State candidate = state;
            qas::Undo undo;
            if (qas::apply_move(candidate, moves[index], undo) &&
                candidate.terminal == qas::Terminal::None && !has_immediate_win(candidate)) {
                state = candidate;
                selected = true;
                break;
            }
        }
        if (!selected) break;
    }
    return state;
}

qas::State duplicate_hand_position() {
    qas::State state = qas::initial_state();
    for (int piece : {0, 1}) {
        state.board[state.pos[piece]] = -1;
        state.pos[piece] = static_cast<std::uint8_t>(qas::first_hand_slot + piece);
    }
    qas::recompute_hash(state);
    return state;
}

bool legal_result(const qas::State& state, const qas::SearchResult& result) {
    const auto legal = qas::generate_legal_moves(state);
    return result.has_move &&
           std::find(legal.begin(), legal.end(), result.best_move) != legal.end();
}

struct Mode {
    const char* name;
    bool tt;
    bool leq;
    std::size_t threshold;
};

}  // namespace

int main(int argc, char** argv) {
    const int depth = argc >= 2 ? std::max(1, std::atoi(argv[1])) : 4;
    const std::vector<std::pair<std::string, qas::State>> positions{
        {"initial", qas::initial_state()},
        {"safe_4ply", safe_position(4, 1)},
        {"safe_8ply", safe_position(8, 3)},
        {"duplicate_hands", duplicate_hand_position()}};
    const std::vector<Mode> modes{{"AB", false, false, 0},
                                  {"AB+TT", true, false, 0},
                                  {"AB+TT+LEQ_ALWAYS", true, true, 0},
                                  {"AB+TT+LEQ_TRIGGER", true, true, 12}};

    std::cout << "mode,position,depth,score,legal,nodes,expanded,raw_L,eq_L,dup_ratio,"
                 "cutoffs,tt_hits,tt_probes,group_ms,time_ms,best\n";
    for (const Mode& mode : modes) {
        for (const auto& item : positions) {
            qas::AlphaBetaEngine engine(1U << 18U);
            qas::SearchOptions options;
            options.max_depth = depth;
            options.time_limit_ms = 60'000;
            options.use_tt = mode.tt;
            if (mode.leq) qas::enable_successor_equivalence(options, mode.threshold);
            const auto result = engine.find_best_move(item.second, options);
            const auto& stats = result.stats;
            std::cout << mode.name << ',' << item.first << ',' << depth << ',' << result.score
                      << ',' << (legal_result(item.second, result) ? 1 : 0) << ','
                      << stats.searched_nodes << ',' << stats.expanded_nodes << ',' << std::fixed
                      << std::setprecision(3) << stats.average_legal_moves() << ','
                      << stats.average_equivalent_moves() << ',' << stats.duplicate_ratio() << ','
                      << stats.cutoffs << ',' << stats.tt_hits << ',' << stats.tt_probes << ','
                      << stats.leq_grouping_ms << ',' << stats.elapsed_ms << ','
                      << qas::move_string(result.best_move) << '\n';
        }
    }
    return 0;
}

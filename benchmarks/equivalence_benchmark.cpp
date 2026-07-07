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
    bool pvs;
    bool strong_ordering;
    bool leq;
};

}  // namespace

int main(int argc, char** argv) {
    const int depth = argc >= 2 ? std::max(1, std::atoi(argv[1])) : 4;
    const std::string mode_filter = argc >= 3 ? argv[2] : "";
    std::vector<std::pair<std::string, qas::State>> positions{
        {"initial", qas::initial_state()},
        {"safe_4ply", safe_position(4, 1)},
        {"safe_8ply", safe_position(8, 3)},
        {"duplicate_hands", duplicate_hand_position()}};
    qas::State near_draw = qas::initial_state();
    near_draw.turn = 252;
    qas::recompute_hash(near_draw);
    positions.push_back({"near_draw", near_draw});
    const std::vector<Mode> modes{{"AB", false, false, false, false},
                                  {"AB+TT", true, false, false, false},
                                  {"AB+TT+PVS", true, true, false, false},
                                  {"AB+TT+PVS+ORDER", true, true, true, false},
                                  {"AB+TT+PVS+ORDER+LEQ", true, true, true, true}};

    std::cout << "profile,mode,position,depth,time_ms,nodes,nps,best_move,score,avg_L,"
                 "cutoffs,first_cutoff_rate,avg_cutoff_rank,tt_probes,tt_hits,tt_hit_rate,"
                 "tt_size_mb,l_eq_calls,l_eq_dup_ratio,l_eq_ms,legal\n";
    for (const Mode& mode : modes) {
        if (!mode_filter.empty() && mode_filter != mode.name) continue;
        for (const auto& item : positions) {
            qas::AlphaBetaEngine engine(1U << 18U);
            qas::SearchOptions options;
            options.max_depth = depth;
            options.time_limit_ms = 60'000;
            options.soft_time_limit_ms = 60'000;
            options.hard_time_limit_ms = 60'000;
            options.use_tt = mode.tt;
            options.pvs_enabled = mode.pvs;
            options.aspiration_enabled = false;
            options.strong_ordering_enabled = mode.strong_ordering;
            options.killer_enabled = mode.strong_ordering;
            options.history_enabled = mode.strong_ordering;
            if (mode.leq) qas::enable_successor_equivalence(options, 12);
            const auto result = engine.find_best_move(item.second, options);
            const auto& stats = result.stats;
            const double nps = stats.elapsed_ms > 0 ? stats.searched_nodes * 1000.0 / stats.elapsed_ms : 0;
            const double first_rate = stats.cutoffs > 0 ? static_cast<double>(stats.first_move_cutoffs) / stats.cutoffs : 0;
            const double hit_rate = stats.tt_probes > 0 ? static_cast<double>(stats.tt_hits) / stats.tt_probes : 0;
            std::cout << "local_benchmark," << mode.name << ',' << item.first << ',' << depth
                      << ',' << std::fixed << std::setprecision(3) << stats.elapsed_ms << ','
                      << stats.searched_nodes << ',' << nps << ',' << qas::move_string(result.best_move)
                      << ',' << result.score << ',' << stats.average_legal_moves() << ','
                      << stats.cutoffs << ',' << first_rate << ',' << stats.average_cutoff_rank()
                      << ',' << stats.tt_probes << ',' << stats.tt_hits << ',' << hit_rate
                      << ',' << (engine.tt_bytes() >> 20U) << ',' << stats.leq_grouped_nodes
                      << ',' << stats.duplicate_ratio() << ',' << stats.leq_grouping_ms << ','
                      << (legal_result(item.second, result) ? 1 : 0) << '\n';
        }
    }
    return 0;
}

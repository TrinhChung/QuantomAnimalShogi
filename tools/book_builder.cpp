// Offline position-book builder: deep guarded analysis of saved positions.
//
// Usage: qas_book_builder <positions.txt> <think_ms> <verify_ms> <verify_max_ply> [config.json]
//
// <positions.txt> holds blocks of "# <label>" followed by a debug state (see README). Each position
// is analysed with the contest search stack (quiescence, L_eq, concurrent df-pn guard) for
// <think_ms>; the chosen move is then re-checked by a deeper df-pn (<verify_max_ply>, <verify_ms>).
// A refuted move is excluded and the position re-analysed (up to three times). Output, one line per
// unique position (by book key), tab separated:
//   label  key  encoded_move  move  score  refuted_by_deep_dfpn(0/1)  alternatives_refuted
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "book/position_book.hpp"
#include "core/config.hpp"
#include "exact/equivalence.hpp"
#include "search/dfpn.hpp"
#include "search/guarded_search.hpp"

namespace {

struct Position {
    std::string label;
    qas::State state;
};

std::vector<Position> load_positions(const std::string& path) {
    std::ifstream input(path);
    std::vector<Position> positions;
    std::string line;
    std::string label;
    std::ostringstream block;
    auto flush = [&]() {
        if (label.empty())
            return;
        std::istringstream stream(block.str());
        positions.push_back(Position{label, qas::parse_state(stream)});
    };
    while (std::getline(input, line)) {
        if (line.rfind("# ", 0) == 0) {
            flush();
            label = line.substr(2);
            block.str("");
            block.clear();
        } else {
            block << line << '\n';
        }
    }
    flush();
    return positions;
}

qas::SearchOptions contest_options(const qas::EngineConfig& config, int think_ms) {
    qas::SearchOptions options;
    options.max_depth = config.search.max_depth;
    options.hard_time_limit_ms = options.time_limit_ms = think_ms;
    options.soft_time_limit_ms = think_ms * 25 / 29;
    options.qsearch_depth = config.search.qsearch_depth;
    if (config.search.l_eq_enabled && config.search.l_eq_trigger_enabled) {
        qas::enable_successor_equivalence(options,
                                          config.search.l_eq_min_legal_count,
                                          config.search.l_eq_require_duplicate_hand_hint);
        options.reducer_min_depth = config.search.l_eq_min_depth_remaining;
        options.reducer_min_duplicate_ratio = config.search.l_eq_min_duplicate_ratio;
    }
    return options;
}

bool refuted_deeply(const qas::State& state, const qas::Move& move, int verify_ms, int max_ply) {
    qas::State after = state;
    qas::Undo undo;
    if (!qas::apply_move(after, move, undo) || after.terminal != qas::Terminal::None)
        return false;
    qas::DfpnSolver solver(std::size_t{1} << 23U);
    solver.reset(after.side_to_move, max_ply);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(verify_ms);
    return solver.prove(after, deadline).status == qas::ProofStatus::Proven;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: qas_book_builder <positions.txt> <think_ms> <verify_ms> "
                     "<verify_max_ply> [config.json]\n";
        return 2;
    }
    const int think_ms = std::stoi(argv[2]);
    const int verify_ms = std::stoi(argv[3]);
    const int verify_max_ply = std::stoi(argv[4]);
    const qas::EngineConfig config =
        qas::load_engine_config(argc > 5 ? argv[5] : "engine_config.json");
    qas::GuardOptions guard;
    guard.enabled = config.search.dfpn_guard_enabled;
    guard.max_ply = config.search.dfpn_guard_max_ply;

    std::set<std::string> seen;
    for (const Position& position : load_positions(argv[1])) {
        const std::string key = qas::PositionBook::key(position.state);
        if (!seen.insert(key).second || position.state.terminal != qas::Terminal::None)
            continue;
        qas::SearchOptions options = contest_options(config, think_ms);
        const auto legal = qas::generate_legal_moves(position.state);
        qas::SearchResult result;
        bool refuted = false;
        int alternatives_refuted = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            qas::AlphaBetaEngine engine(std::size_t{1} << 23U);
            result = qas::guarded_find_best_move(engine, position.state, options, guard);
            if (!result.has_move)
                break;
            refuted = refuted_deeply(position.state, result.best_move, verify_ms, verify_max_ply);
            if (!refuted || options.root_excluded.size() + 1 >= legal.size())
                break;
            options.root_excluded.push_back(result.best_move);
            ++alternatives_refuted;
        }
        if (!result.has_move)
            continue;
        std::cout << position.label << '\t' << key << '\t'
                  << qas::PositionBook::encode_move(position.state, result.best_move) << '\t'
                  << qas::move_string(result.best_move) << '\t' << result.score << '\t' << refuted
                  << '\t' << alternatives_refuted << std::endl;
    }
    return 0;
}

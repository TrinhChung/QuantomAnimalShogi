#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: psapi.h requires Windows base types.
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif

#include "exact/equivalence.hpp"
#include "io/protocol.hpp"
#include "search/alpha_beta.hpp"
#include "stage35_fixture.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kStage55Seed = 0x55CE'2026U;

enum class LeqMode { Off, Current, Relaxed, Strict, TargetA, TargetB, TargetC, Tactical };

struct CeilingFixture {
    std::string name;
    std::string group;
    std::uint32_t seed{0};
    qas::State state{};
    std::vector<int> action_sequence{};
};

struct FixtureMetadata {
    std::size_t pseudo_count{0};
    std::size_t legal_count{0};
    int uncertainty{0};
    int south_lions{0};
    int north_lions{0};
    int hand_count{0};
    int duplicate_hand_pairs{0};
    bool duplicate_hand_hint{false};
    bool near_terminal{false};
    bool near_try{false};
    bool near_catch{false};
    int horizon_remaining{0};
};

struct Options {
    std::string mode{"quick"};
    std::string out_dir{"local_reports/stage55"};
    bool full_corpus{false};
    int max_fixtures{0};
    int runs{1};
    int depth_min{8};
    int depth_max{11};
    int timeout_ms{30000};
    int tt_mb{512};
    int shard_count{1};
    int shard_index{0};
    std::vector<std::string> fixture_names{};
};

int popcount(qas::Mask mask) {
    int count = 0;
    for (; mask != 0; mask = static_cast<qas::Mask>(mask & (mask - 1)))
        ++count;
    return count;
}

int uncertainty(const qas::State& state) {
    int total = 0;
    for (qas::Mask mask : state.mask)
        total += popcount(mask);
    return total;
}

int hand_count(const qas::State& state) {
    return static_cast<int>(
        std::count_if(state.pos.begin(), state.pos.end(), [](std::uint8_t position) {
            return qas::is_hand_position(position);
        }));
}

int lion_candidates(const qas::State& state, qas::Side side) {
    int count = 0;
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        if (qas::owned_by(state, piece, side) &&
            qas::contains(state.mask[piece], qas::Animal::Lion))
            ++count;
    }
    return count;
}

int duplicate_hand_pairs(const qas::State& state) {
    int pairs = 0;
    for (int left = 0; left < qas::physical_piece_count; ++left) {
        if (!qas::is_hand_position(state.pos[left]) ||
            !qas::owned_by(state, left, state.side_to_move))
            continue;
        for (int right = left + 1; right < qas::physical_piece_count; ++right) {
            if (qas::is_hand_position(state.pos[right]) &&
                qas::owned_by(state, right, state.side_to_move) &&
                state.mask[left] == state.mask[right] &&
                qas::originated_from(state, left, qas::Side::North) ==
                    qas::originated_from(state, right, qas::Side::North)) {
                ++pairs;
            }
        }
    }
    return pairs;
}

bool has_immediate_terminal(const qas::State& state, qas::Terminal reason = qas::Terminal::None) {
    for (const qas::Move& move : qas::generate_legal_moves(state)) {
        qas::State copy = state;
        qas::Undo undo;
        if (!qas::apply_move(copy, move, undo))
            continue;
        const bool terminal =
            copy.terminal == qas::Terminal::Catch || copy.terminal == qas::Terminal::Try;
        if (terminal && (reason == qas::Terminal::None || copy.terminal == reason))
            return true;
    }
    return false;
}

FixtureMetadata metadata(const qas::State& state) {
    FixtureMetadata result;
    result.pseudo_count = qas::generate_pseudo_legal_moves(state).size();
    result.legal_count = qas::generate_legal_moves(state).size();
    result.uncertainty = uncertainty(state);
    result.south_lions = lion_candidates(state, qas::Side::South);
    result.north_lions = lion_candidates(state, qas::Side::North);
    result.hand_count = hand_count(state);
    result.duplicate_hand_pairs = duplicate_hand_pairs(state);
    result.duplicate_hand_hint = result.duplicate_hand_pairs > 0;
    result.near_try = has_immediate_terminal(state, qas::Terminal::Try);
    result.near_catch = has_immediate_terminal(state, qas::Terminal::Catch);
    result.near_terminal = result.near_try || result.near_catch || state.turn >= 248;
    result.horizon_remaining = std::max(0, 256 - static_cast<int>(state.turn));
    return result;
}

std::string terminal_name(const qas::State& state) {
    return qas::terminal_name(state.terminal);
}

std::string csv_bool(bool value) {
    return value ? "1" : "0";
}

std::string side_name(qas::Side side) {
    return side == qas::Side::South ? "south" : "north";
}

std::string move_text(const qas::Move& move) {
    return move.valid() ? qas::move_string(move) : "none";
}

std::string join_pv(const std::vector<qas::Move>& pv) {
    std::ostringstream output;
    for (std::size_t index = 0; index < pv.size(); ++index) {
        if (index != 0)
            output << ';';
        output << move_text(pv[index]);
    }
    return output.str();
}

std::string join_depth_reports(const std::vector<qas::DepthReport>& reports) {
    std::ostringstream output;
    for (std::size_t index = 0; index < reports.size(); ++index) {
        if (index != 0)
            output << ';';
        const auto& item = reports[index];
        output << item.depth << ':' << std::fixed << std::setprecision(2) << item.elapsed_ms << ':'
               << item.nodes << ':' << item.score << ':' << move_text(item.best_move);
    }
    return output.str();
}

bool best_move_is_legal(const qas::State& state, const qas::SearchResult& result) {
    const auto legal = qas::generate_legal_moves(state);
    if (legal.empty())
        return !result.has_move;
    return result.has_move &&
           std::find(legal.begin(), legal.end(), result.best_move) != legal.end();
}

std::size_t floor_power_of_two(std::size_t value) {
    if (value == 0)
        return 1;
    std::size_t result = 1;
    while (result <= value / 2)
        result *= 2;
    return result;
}

std::size_t tt_entries_from_mb(int tt_mb) {
    const std::size_t bytes = static_cast<std::size_t>(std::max(1, tt_mb)) * 1024ULL * 1024ULL;
    return floor_power_of_two(bytes / sizeof(qas::TTEntry));
}

bool reaches_try_rank(int square, qas::Side side) {
    if (square < 0 || square >= qas::board_size)
        return false;
    const int row = square / qas::board_width;
    return row == (side == qas::Side::South ? 0 : qas::board_height - 1);
}

int benchmark_piece_value(qas::Mask mask) {
    int total = 0;
    int count = 0;
    for (qas::Animal form : qas::all_forms) {
        if (!qas::contains(mask, form))
            continue;
        switch (form) {
            case qas::Animal::Chick:
                total += 100;
                break;
            case qas::Animal::Giraffe:
                total += 320;
                break;
            case qas::Animal::Elephant:
                total += 310;
                break;
            case qas::Animal::Lion:
                total += 1050;
                break;
            case qas::Animal::Hen:
                total += 420;
                break;
        }
        ++count;
    }
    return count == 0 ? 0 : total / count;
}

int tactical_representative_score(const qas::State& before,
                                  const qas::State& after,
                                  const qas::Move& move,
                                  const qas::Move& preferred) {
    if (move == preferred)
        return 2'000'000;
    int score = 0;
    if (after.terminal == qas::Terminal::Catch || after.terminal == qas::Terminal::Try)
        score += 1'500'000;
    const int captured = before.board[move.to];
    if (captured >= 0) {
        score += 250'000 + benchmark_piece_value(before.mask[captured]) * 20;
        if (qas::contains(before.mask[captured], qas::Animal::Lion))
            score += 1'000'000;
    }
    const int collapse = popcount(before.mask[move.piece]) - popcount(after.mask[move.piece]);
    score += collapse * 12'000;

    const qas::Side opponent_after_move = after.side_to_move;
    score += (lion_candidates(before, opponent_after_move) -
              lion_candidates(after, opponent_after_move)) *
             70'000;
    if (after.pos[move.piece] < qas::board_size &&
        qas::contains(after.mask[move.piece], qas::Animal::Lion) &&
        reaches_try_rank(after.pos[move.piece], before.side_to_move)) {
        score += 30'000;
    }
    return score - (static_cast<int>(move.from) * qas::board_size + move.to);
}

std::vector<qas::Move> representatives_from_classes(
    const std::vector<qas::SuccessorClass>& classes) {
    std::vector<qas::Move> representatives;
    representatives.reserve(classes.size());
    for (const qas::SuccessorClass& item : classes)
        representatives.push_back(item.representative);
    return representatives;
}

std::vector<qas::SuccessorClass> generate_tactical_successor_classes(
    const qas::State& state,
    const std::vector<qas::Move>& legal_moves,
    const qas::Move& preferred,
    qas::SuccessorReductionStats* stats) {
    struct ScoredClass {
        qas::SuccessorClass item{};
        int score{std::numeric_limits<int>::min()};
    };
    std::vector<ScoredClass> classes;
    classes.reserve(legal_moves.size());
    for (const qas::Move& move : legal_moves) {
        qas::State successor = state;
        qas::Undo undo;
        qas::RuleMetrics rule_metrics;
        const bool applied = stats == nullptr
                                 ? qas::apply_move(successor, move, undo)
                                 : qas::apply_move_profiled(successor, move, undo, rule_metrics);
        if (!applied)
            continue;
        if (stats != nullptr) {
            stats->propagation_calls += rule_metrics.propagation_calls;
            stats->propagation_ms += rule_metrics.propagation_ms;
        }

        qas::CanonKey key;
        if (stats != nullptr) {
            const auto begin = Clock::now();
            key = qas::canonical_key(successor);
            const auto end = Clock::now();
            ++stats->canonicalize_calls;
            stats->canonicalize_ms +=
                std::chrono::duration<double, std::milli>(end - begin).count();
        } else {
            key = qas::canonical_key(successor);
        }

        const int score = tactical_representative_score(state, successor, move, preferred);
        auto found = std::find_if(classes.begin(), classes.end(), [&key](const ScoredClass& item) {
            return item.item.key == key;
        });
        if (found == classes.end()) {
            classes.push_back(ScoredClass{qas::SuccessorClass{key, move, 1}, score});
        } else {
            ++found->item.multiplicity;
            if (score > found->score) {
                found->item.representative = move;
                found->score = score;
            }
        }
    }

    std::vector<qas::SuccessorClass> result;
    result.reserve(classes.size());
    for (const ScoredClass& item : classes)
        result.push_back(item.item);
    return result;
}

std::vector<qas::Move> reduce_successors_for_benchmark(const qas::State& state,
                                                       const std::vector<qas::Move>& legal_moves,
                                                       const qas::Move& preferred,
                                                       std::size_t threshold,
                                                       int min_depth,
                                                       int depth_remaining,
                                                       double minimum_duplicate_ratio,
                                                       bool require_hint,
                                                       bool low_time,
                                                       bool require_two_hand_pieces,
                                                       bool tactical_representative,
                                                       bool& applied,
                                                       qas::SuccessorReductionStats* stats) {
    applied = false;
    if (low_time || depth_remaining < min_depth || legal_moves.size() < threshold)
        return legal_moves;
    if (require_hint && duplicate_hand_pairs(state) == 0)
        return legal_moves;
    if (require_two_hand_pieces && hand_count(state) < 2)
        return legal_moves;

    applied = true;
    if (stats != nullptr) {
        ++stats->attempted_nodes;
        stats->input_legal_moves += legal_moves.size();
    }

    const auto classes =
        tactical_representative
            ? generate_tactical_successor_classes(state, legal_moves, preferred, stats)
            : qas::generate_equivalent_successor_classes(state, legal_moves, preferred, stats);
    if (stats != nullptr) {
        stats->output_representatives += classes.size();
        stats->estimated_saved_children += legal_moves.size() - classes.size();
    }

    const double duplicate_ratio =
        legal_moves.empty()
            ? 0.0
            : 1.0 - static_cast<double>(classes.size()) / static_cast<double>(legal_moves.size());
    if (duplicate_ratio < minimum_duplicate_ratio) {
        if (stats != nullptr)
            ++stats->rollback_low_duplicate_ratio;
        applied = false;
        return legal_moves;
    }
    return representatives_from_classes(classes);
}

std::vector<qas::Move> target_a_successor_reducer(const qas::State& state,
                                                  const std::vector<qas::Move>& legal_moves,
                                                  const qas::Move& preferred,
                                                  std::size_t threshold,
                                                  int min_depth,
                                                  int depth_remaining,
                                                  double minimum_duplicate_ratio,
                                                  bool require_hint,
                                                  bool low_time,
                                                  bool& applied,
                                                  qas::SuccessorReductionStats* stats) {
    (void)require_hint;
    return reduce_successors_for_benchmark(state,
                                           legal_moves,
                                           preferred,
                                           threshold,
                                           min_depth,
                                           depth_remaining,
                                           minimum_duplicate_ratio,
                                           false,
                                           low_time,
                                           true,
                                           false,
                                           applied,
                                           stats);
}

std::vector<qas::Move> target_b_successor_reducer(const qas::State& state,
                                                  const std::vector<qas::Move>& legal_moves,
                                                  const qas::Move& preferred,
                                                  std::size_t threshold,
                                                  int min_depth,
                                                  int depth_remaining,
                                                  double minimum_duplicate_ratio,
                                                  bool require_hint,
                                                  bool low_time,
                                                  bool& applied,
                                                  qas::SuccessorReductionStats* stats) {
    return reduce_successors_for_benchmark(state,
                                           legal_moves,
                                           preferred,
                                           threshold,
                                           min_depth,
                                           depth_remaining,
                                           minimum_duplicate_ratio,
                                           require_hint,
                                           low_time,
                                           false,
                                           false,
                                           applied,
                                           stats);
}

std::vector<qas::Move> target_c_successor_reducer(const qas::State& state,
                                                  const std::vector<qas::Move>& legal_moves,
                                                  const qas::Move& preferred,
                                                  std::size_t threshold,
                                                  int min_depth,
                                                  int depth_remaining,
                                                  double minimum_duplicate_ratio,
                                                  bool require_hint,
                                                  bool low_time,
                                                  bool& applied,
                                                  qas::SuccessorReductionStats* stats) {
    (void)require_hint;
    return reduce_successors_for_benchmark(state,
                                           legal_moves,
                                           preferred,
                                           threshold,
                                           min_depth,
                                           depth_remaining,
                                           minimum_duplicate_ratio,
                                           false,
                                           low_time,
                                           true,
                                           false,
                                           applied,
                                           stats);
}

std::vector<qas::Move> tactical_successor_reducer(const qas::State& state,
                                                  const std::vector<qas::Move>& legal_moves,
                                                  const qas::Move& preferred,
                                                  std::size_t threshold,
                                                  int min_depth,
                                                  int depth_remaining,
                                                  double minimum_duplicate_ratio,
                                                  bool require_hint,
                                                  bool low_time,
                                                  bool& applied,
                                                  qas::SuccessorReductionStats* stats) {
    return reduce_successors_for_benchmark(state,
                                           legal_moves,
                                           preferred,
                                           threshold,
                                           min_depth,
                                           depth_remaining,
                                           minimum_duplicate_ratio,
                                           require_hint,
                                           low_time,
                                           false,
                                           true,
                                           applied,
                                           stats);
}

std::uint64_t current_peak_rss_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize / (1024ULL * 1024ULL));
    }
#endif
    return 0;
}

std::uint64_t current_page_faults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.PageFaultCount);
    }
#endif
    return 0;
}

bool play_random_nonterminal(qas::State& state,
                             std::vector<int>& sequence,
                             std::mt19937& random,
                             int plies) {
    for (int ply = 0; ply < plies; ++ply) {
        auto moves = qas::generate_legal_moves(state);
        std::shuffle(moves.begin(), moves.end(), random);
        bool moved = false;
        for (const qas::Move& move : moves) {
            qas::State successor = state;
            qas::Undo undo;
            if (qas::apply_move(successor, move, undo) &&
                successor.terminal == qas::Terminal::None) {
                sequence.push_back(qas::encode_action_move(move));
                state = successor;
                moved = true;
                break;
            }
        }
        if (!moved)
            return false;
    }
    return true;
}

CeilingFixture reachable_fixture(const std::string& name,
                                 const std::string& group,
                                 std::uint32_t seed,
                                 int plies) {
    std::mt19937 random(seed);
    for (int attempt = 0; attempt < 500; ++attempt) {
        CeilingFixture fixture;
        fixture.name = name;
        fixture.group = group;
        fixture.seed = seed;
        fixture.state = qas::initial_state();
        if (play_random_nonterminal(fixture.state, fixture.action_sequence, random, plies))
            return fixture;
    }
    throw std::runtime_error("could not generate reachable fixture: " + name);
}

CeilingFixture selected_fixture(const std::string& name,
                                const std::string& group,
                                std::uint32_t seed,
                                int min_plies,
                                int max_plies,
                                const std::function<int(const qas::State&)>& quality) {
    std::mt19937 random(seed);
    CeilingFixture best;
    int best_score = std::numeric_limits<int>::min();
    for (int attempt = 0; attempt < 2500; ++attempt) {
        const int plies =
            min_plies +
            static_cast<int>(random() % static_cast<unsigned>(max_plies - min_plies + 1));
        CeilingFixture candidate;
        candidate.name = name;
        candidate.group = group;
        candidate.seed = seed;
        candidate.state = qas::initial_state();
        if (!play_random_nonterminal(candidate.state, candidate.action_sequence, random, plies))
            continue;
        const int score = quality(candidate.state);
        if (score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    if (best_score == std::numeric_limits<int>::min())
        throw std::runtime_error("could not select fixture: " + name);
    best.name = name;
    best.group = group;
    best.seed = seed;
    return best;
}

void set_near_draw_turn(CeilingFixture& fixture) {
    fixture.state.turn = 252;
    fixture.state.terminal = qas::Terminal::None;
    fixture.state.winner = -1;
    qas::recompute_hash(fixture.state);
}

CeilingFixture from_stage35(const qas::benchmark::Fixture& fixture) {
    return CeilingFixture{
        fixture.name, fixture.category, fixture.seed, fixture.state, fixture.action_sequence};
}

void add_unique(std::vector<CeilingFixture>& fixtures, CeilingFixture fixture) {
    const auto found = std::find_if(fixtures.begin(), fixtures.end(), [&](const auto& item) {
        return item.name == fixture.name;
    });
    if (found == fixtures.end())
        fixtures.push_back(std::move(fixture));
}

std::vector<CeilingFixture> build_corpus() {
    std::vector<CeilingFixture> fixtures;
    for (const auto& fixture : qas::benchmark::generate_stage35_fixtures())
        add_unique(fixtures, from_stage35(fixture));

    add_unique(fixtures,
               selected_fixture("high_branching_hand_stack",
                                "required",
                                kStage55Seed + 1U,
                                8,
                                48,
                                [](const qas::State& state) {
                                    const auto meta = metadata(state);
                                    return static_cast<int>(meta.legal_count) * 10 +
                                           meta.hand_count * 20;
                                }));
    add_unique(fixtures,
               selected_fixture("many_duplicate_hands",
                                "required",
                                kStage55Seed + 2U,
                                6,
                                56,
                                [](const qas::State& state) {
                                    const auto meta = metadata(state);
                                    return meta.duplicate_hand_pairs * 200 + meta.hand_count * 10;
                                }));
    add_unique(fixtures,
               selected_fixture("many_lion_candidates",
                                "required",
                                kStage55Seed + 3U,
                                4,
                                32,
                                [](const qas::State& state) {
                                    return 100 * (lion_candidates(state, qas::Side::South) +
                                                  lion_candidates(state, qas::Side::North)) +
                                           uncertainty(state);
                                }));
    add_unique(fixtures,
               selected_fixture("forced_try_defense",
                                "required",
                                kStage55Seed + 4U,
                                4,
                                40,
                                [](const qas::State& state) {
                                    return has_immediate_terminal(state, qas::Terminal::Try) ? 1000
                                                                                             : 0;
                                }));
    add_unique(
        fixtures,
        selected_fixture("forced_catch_defense",
                         "required",
                         kStage55Seed + 5U,
                         4,
                         40,
                         [](const qas::State& state) {
                             return has_immediate_terminal(state, qas::Terminal::Catch) ? 1000 : 0;
                         }));
    add_unique(fixtures,
               selected_fixture("low_uncertainty_endgame",
                                "required",
                                kStage55Seed + 6U,
                                24,
                                80,
                                [](const qas::State& state) {
                                    return 200 - uncertainty(state) * 5 -
                                           static_cast<int>(metadata(state).legal_count);
                                }));
    add_unique(fixtures,
               selected_fixture("maximal_uncertainty_midgame",
                                "required",
                                kStage55Seed + 7U,
                                8,
                                28,
                                [](const qas::State& state) { return uncertainty(state); }));

    const std::array<std::pair<const char*, int>, 4> ply_groups{
        {{"random_ply4", 4}, {"random_ply8", 8}, {"random_ply16", 16}, {"random_ply32", 32}}};
    for (const auto& [group, plies] : ply_groups) {
        for (int index = 0; index < 20; ++index) {
            std::ostringstream name;
            name << group << '_' << std::setw(2) << std::setfill('0') << index;
            add_unique(
                fixtures,
                reachable_fixture(name.str(),
                                  group,
                                  kStage55Seed + static_cast<std::uint32_t>(plies * 100 + index),
                                  plies));
        }
    }

    for (int index = 0; index < 20; ++index) {
        std::ostringstream suffix;
        suffix << std::setw(2) << std::setfill('0') << index;
        add_unique(fixtures,
                   selected_fixture("random_hand_pieces_" + suffix.str(),
                                    "random_hand_pieces",
                                    kStage55Seed + 1000U + index,
                                    6,
                                    60,
                                    [](const qas::State& state) { return hand_count(state); }));
        add_unique(fixtures,
                   selected_fixture("random_duplicate_hands_" + suffix.str(),
                                    "random_duplicate_hands",
                                    kStage55Seed + 2000U + index,
                                    6,
                                    60,
                                    [](const qas::State& state) {
                                        return duplicate_hand_pairs(state) * 100 +
                                               hand_count(state);
                                    }));
        add_unique(fixtures,
                   selected_fixture("random_high_uncertainty_" + suffix.str(),
                                    "random_high_uncertainty",
                                    kStage55Seed + 3000U + index,
                                    8,
                                    32,
                                    [](const qas::State& state) { return uncertainty(state); }));
        add_unique(
            fixtures,
            selected_fixture("random_low_uncertainty_" + suffix.str(),
                             "random_low_uncertainty",
                             kStage55Seed + 4000U + index,
                             12,
                             72,
                             [](const qas::State& state) { return 100 - uncertainty(state); }));
        add_unique(fixtures,
                   selected_fixture("random_near_terminal_" + suffix.str(),
                                    "random_near_terminal",
                                    kStage55Seed + 5000U + index,
                                    4,
                                    56,
                                    [](const qas::State& state) {
                                        return has_immediate_terminal(state) ? 1000 : 0;
                                    }));
        auto near_draw = selected_fixture(
            "random_near_draw_" + suffix.str(),
            "random_near_draw",
            kStage55Seed + 6000U + index,
            24,
            96,
            [](const qas::State& state) { return hand_count(state) * 10 + uncertainty(state); });
        set_near_draw_turn(near_draw);
        add_unique(fixtures, std::move(near_draw));
    }

    for (const CeilingFixture& fixture : fixtures) {
        std::string error;
        if (!qas::validate_state(fixture.state, &error))
            throw std::runtime_error("invalid fixture " + fixture.name + ": " + error);
    }
    return fixtures;
}

const std::vector<std::string>& important_names() {
    static const std::vector<std::string> names{"initial",
                                                "duplicate_hands",
                                                "high_uncertainty_midgame",
                                                "many_hands",
                                                "near_try",
                                                "near_catch",
                                                "maximal_uncertainty_midgame",
                                                "high_branching_hand_stack",
                                                "many_duplicate_hands",
                                                "forced_try_defense"};
    return names;
}

bool name_selected(const Options& options, const std::string& name) {
    if (options.fixture_names.empty())
        return true;
    return std::find(options.fixture_names.begin(), options.fixture_names.end(), name) !=
           options.fixture_names.end();
}

std::vector<CeilingFixture> select_fixtures(const std::vector<CeilingFixture>& corpus,
                                            const Options& options) {
    std::vector<CeilingFixture> selected;
    if (!options.fixture_names.empty()) {
        for (const std::string& name : options.fixture_names) {
            const auto found = std::find_if(corpus.begin(), corpus.end(), [&](const auto& fixture) {
                return fixture.name == name;
            });
            if (found != corpus.end())
                selected.push_back(*found);
        }
    } else if (options.full_corpus) {
        for (std::size_t index = 0; index < corpus.size(); ++index) {
            if (options.shard_count > 1 &&
                static_cast<int>(index % static_cast<std::size_t>(options.shard_count)) !=
                    options.shard_index) {
                continue;
            }
            selected.push_back(corpus[index]);
        }
    } else {
        for (const std::string& name : important_names()) {
            const auto found = std::find_if(corpus.begin(), corpus.end(), [&](const auto& fixture) {
                return fixture.name == name;
            });
            if (found != corpus.end() && name_selected(options, found->name))
                selected.push_back(*found);
        }
    }
    if (options.max_fixtures > 0 && static_cast<int>(selected.size()) > options.max_fixtures)
        selected.resize(static_cast<std::size_t>(options.max_fixtures));
    return selected;
}

void write_fixture_files(const std::vector<CeilingFixture>& fixtures, const std::string& out_dir) {
    const std::filesystem::path root(out_dir);
    std::filesystem::create_directories(root / "fixtures");
    std::ofstream manifest(root / "fixtures" / "manifest.txt");
    std::ofstream summary(root / "fixture_summary.csv");
    summary << "name,group,turn,side_to_move,hash,pseudo_moves,legal_moves,uncertainty,"
               "south_lion_candidates,north_lion_candidates,hand_count,duplicate_hand_pairs,"
               "duplicate_hand_hint,near_terminal,near_try,near_catch,horizon_remaining,terminal\n";
    for (const CeilingFixture& fixture : fixtures) {
        const auto meta = metadata(fixture.state);
        manifest << fixture.name << ".fixture\n";
        summary << fixture.name << ',' << fixture.group << ',' << fixture.state.turn << ','
                << side_name(fixture.state.side_to_move) << ',' << fixture.state.hash << ','
                << meta.pseudo_count << ',' << meta.legal_count << ',' << meta.uncertainty << ','
                << meta.south_lions << ',' << meta.north_lions << ',' << meta.hand_count << ','
                << meta.duplicate_hand_pairs << ',' << csv_bool(meta.duplicate_hand_hint) << ','
                << csv_bool(meta.near_terminal) << ',' << csv_bool(meta.near_try) << ','
                << csv_bool(meta.near_catch) << ',' << meta.horizon_remaining << ','
                << terminal_name(fixture.state) << '\n';

        std::ofstream output(root / "fixtures" / (fixture.name + ".fixture"));
        output << "name " << fixture.name << '\n'
               << "group " << fixture.group << '\n'
               << "seed " << fixture.seed << '\n'
               << "turn " << fixture.state.turn << '\n'
               << "side_to_move " << side_name(fixture.state.side_to_move) << '\n'
               << "hash " << fixture.state.hash << '\n'
               << "pseudo_move_count " << meta.pseudo_count << '\n'
               << "legal_move_count " << meta.legal_count << '\n'
               << "uncertainty " << meta.uncertainty << '\n'
               << "south_lion_candidates " << meta.south_lions << '\n'
               << "north_lion_candidates " << meta.north_lions << '\n'
               << "hand_count " << meta.hand_count << '\n'
               << "duplicate_hand_pairs " << meta.duplicate_hand_pairs << '\n'
               << "duplicate_hand_hint " << csv_bool(meta.duplicate_hand_hint) << '\n'
               << "near_terminal " << csv_bool(meta.near_terminal) << '\n'
               << "terminal " << terminal_name(fixture.state) << '\n'
               << "state_begin\n";
        qas::write_state(fixture.state, output);
        output << "state_end\n";
    }
}

std::string leq_mode_name(LeqMode mode) {
    switch (mode) {
        case LeqMode::Off:
            return "off";
        case LeqMode::Current:
            return "current";
        case LeqMode::Relaxed:
            return "relaxed";
        case LeqMode::Strict:
            return "strict";
        case LeqMode::TargetA:
            return "target_a";
        case LeqMode::TargetB:
            return "target_b";
        case LeqMode::TargetC:
            return "target_c";
        case LeqMode::Tactical:
            return "tactical_rep";
    }
    return "unknown";
}

void apply_leq_mode(qas::SearchOptions& options, LeqMode mode) {
    if (mode == LeqMode::Off)
        return;
    if (mode == LeqMode::Current) {
        qas::enable_successor_equivalence(options, 24, true);
        options.reducer_min_depth = 4;
        options.reducer_min_duplicate_ratio = 0.25;
    } else if (mode == LeqMode::Relaxed) {
        qas::enable_successor_equivalence(options, 8, false);
        options.reducer_min_depth = 3;
        options.reducer_min_duplicate_ratio = 0.10;
    } else if (mode == LeqMode::Strict) {
        qas::enable_successor_equivalence(options, 32, true);
        options.reducer_min_depth = 5;
        options.reducer_min_duplicate_ratio = 0.40;
    } else if (mode == LeqMode::TargetA) {
        options.successor_reducer = target_a_successor_reducer;
        options.reducer_threshold = 24;
        options.reducer_min_depth = 4;
        options.reducer_min_duplicate_ratio = 0.25;
        options.reducer_require_duplicate_hint = false;
    } else if (mode == LeqMode::TargetB) {
        options.successor_reducer = target_b_successor_reducer;
        options.reducer_threshold = 28;
        options.reducer_min_depth = 3;
        options.reducer_min_duplicate_ratio = 0.0;
        options.reducer_require_duplicate_hint = true;
    } else if (mode == LeqMode::TargetC) {
        options.successor_reducer = target_c_successor_reducer;
        options.reducer_threshold = 24;
        options.reducer_min_depth = 4;
        options.reducer_min_duplicate_ratio = 0.20;
        options.reducer_require_duplicate_hint = false;
    } else if (mode == LeqMode::Tactical) {
        options.successor_reducer = tactical_successor_reducer;
        options.reducer_threshold = 24;
        options.reducer_min_depth = 4;
        options.reducer_min_duplicate_ratio = 0.25;
        options.reducer_require_duplicate_hint = true;
    }
}

qas::SearchOptions search_options(
    int depth, int timeout_ms, bool iterative, LeqMode leq_mode, bool instrumentation = true) {
    qas::SearchOptions options;
    options.max_depth = depth;
    options.time_limit_ms = timeout_ms;
    options.soft_time_limit_ms = timeout_ms;
    options.hard_time_limit_ms = timeout_ms;
    options.iterative_deepening_enabled = iterative;
    options.pvs_enabled = true;
    options.aspiration_enabled = iterative;
    options.use_tt = true;
    options.strong_ordering_enabled = true;
    options.benchmark_instrumentation_enabled = instrumentation;
    options.optimized_eval_enabled = true;
    options.propagation_mode = qas::PropagationMode::LineageLut;
    apply_leq_mode(options, leq_mode);
    return options;
}

double rate(std::uint64_t part, std::uint64_t total) {
    return total == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(total);
}

double non_negative(double value) {
    return value < 0.0 ? 0.0 : value;
}

double leq_duplicate_ratio(const qas::SearchStats& stats) {
    return stats.leq_attempt_input_moves == 0
               ? 0.0
               : 1.0 - static_cast<double>(stats.leq_attempt_output_representatives) /
                           static_cast<double>(stats.leq_attempt_input_moves);
}

void write_search_header(std::ofstream& output) {
    output
        << "run,fixture,fixture_group,mode,leq_mode,tt_size_mb,depth,time_limit_ms,completed,"
           "elapsed_ms,timeout_hit,last_completed_depth,started_depth,timeout_depth,nodes,nps,"
           "effective_branching_factor,node_growth_ratio,best_move,root_score,pv_line,"
           "per_depth_reports,legal_move_avg,max_legal_moves,pseudo_move_avg,pseudo_to_legal_ratio,"
           "tt_probe_count,tt_hit_rate,tt_exact_hit_rate,tt_cutoff_rate,tt_replacement_count,"
           "tt_collisions,first_move_cutoff_rate,avg_cutoff_rank,killer_cutoffs,history_cutoffs,"
           "beta_cutoffs,pvs_research_count,aspiration_retries,aspiration_fail_high,"
           "aspiration_fail_low,movegen_ms,legal_filter_ms,apply_move_ms,propagation_ms,"
           "terminal_check_ms,eval_ms,ordering_ms,L_eq_calls,L_eq_ms,L_eq_input_moves,"
           "L_eq_output_representatives,L_eq_duplicate_ratio,L_eq_rollback_count,"
           "L_eq_estimated_saved_children,canonicalize_calls,canonicalize_ms,undo_move_calls,"
           "undo_move_ms,hash_recompute_ms,tt_move_present_rate,tt_move_cutoff_rate,"
           "tt_move_present,tt_move_cutoffs,move_ordering_calls,immediate_win_ordering_calls,"
           "prevent_loss_ordering_calls,killer_score_calls,history_score_calls,eval_terminal_ms,"
           "eval_material_mask_ms,eval_mobility_ms,eval_lion_safety_ms,eval_immediate_setup_ms,"
           "eval_immediate_movegen_ms,eval_immediate_filter_ms,eval_immediate_transition_ms,"
           "estimated_exclusive_movegen_ms,estimated_exclusive_legal_filter_ms,"
           "estimated_exclusive_apply_ms,estimated_exclusive_propagation_ms,"
           "estimated_exclusive_terminal_ms,estimated_exclusive_eval_ms,"
           "estimated_exclusive_ordering_ms,estimated_exclusive_tt_ms,"
           "estimated_exclusive_L_eq_ms,peak_rss_mb,page_faults,illegal_move_count,"
           "cutoff_rank_histogram\n";
}

void write_search_row(std::ofstream& output,
                      int run,
                      const CeilingFixture& fixture,
                      const std::string& mode,
                      LeqMode leq_mode,
                      int tt_mb,
                      int depth,
                      int time_limit_ms,
                      const qas::SearchResult& result,
                      std::uint64_t previous_nodes) {
    const auto& stats = result.stats;
    const auto& rules = stats.rule_metrics;
    const bool fixed = mode == "fixed";
    const bool completed = fixed ? stats.depth_reached >= depth : stats.depth_reached > 0;
    const double elapsed_seconds = stats.elapsed_ms / 1000.0;
    const double nps = elapsed_seconds <= 0.0 ? 0.0 : stats.searched_nodes / elapsed_seconds;
    const double ebf =
        stats.searched_nodes == 0 ? 0.0 : std::pow(stats.searched_nodes, 1.0 / depth);
    const double growth =
        previous_nodes == 0 ? 0.0 : static_cast<double>(stats.searched_nodes) / previous_nodes;
    const double pseudo_avg = stats.expanded_nodes == 0
                                  ? 0.0
                                  : static_cast<double>(rules.pseudo_moves_generated) /
                                        static_cast<double>(stats.expanded_nodes);
    const double pseudo_to_legal =
        rules.pseudo_moves_generated == 0
            ? 0.0
            : static_cast<double>(stats.generated_legal_moves) / rules.pseudo_moves_generated;
    const int illegal_count = best_move_is_legal(fixture.state, result) ? 0 : 1;
    std::ostringstream histogram;
    for (std::size_t index = 0; index < stats.cutoff_rank_histogram.size(); ++index) {
        if (index != 0)
            histogram << ';';
        histogram << stats.cutoff_rank_histogram[index];
    }
    const double average_apply_ms =
        rules.apply_move_internal_calls == 0
            ? 0.0
            : rules.apply_move_internal_ms / static_cast<double>(rules.apply_move_internal_calls);
    const double legal_filter_apply_estimate =
        average_apply_ms * static_cast<double>(rules.legal_filter_apply_calls);
    const double estimated_exclusive_movegen_ms = rules.pseudo_move_generation_ms;
    const double estimated_exclusive_legal_filter_ms =
        non_negative(rules.legal_filter_ms - legal_filter_apply_estimate);
    const double estimated_exclusive_apply_ms =
        non_negative(rules.apply_move_internal_ms - stats.propagation_ms - rules.terminal_check_ms -
                     rules.hash_recompute_ms);
    const double estimated_exclusive_propagation_ms = stats.propagation_ms;
    const double estimated_exclusive_terminal_ms = rules.terminal_check_ms;
    const double estimated_exclusive_eval_ms = stats.eval_ms;
    const double estimated_exclusive_ordering_ms = stats.move_order_ms;
    const double estimated_exclusive_tt_ms = 0.0;
    const double estimated_exclusive_leq_ms = stats.leq_grouping_ms;

    output << run << ',' << fixture.name << ',' << fixture.group << ',' << mode << ','
           << leq_mode_name(leq_mode) << ',' << tt_mb << ',' << depth << ',' << time_limit_ms << ','
           << csv_bool(completed) << ',' << std::fixed << std::setprecision(3) << stats.elapsed_ms
           << ',' << csv_bool(stats.timeout_hit) << ',' << stats.depth_reached << ','
           << stats.started_depth << ',' << stats.timeout_depth << ',' << stats.searched_nodes
           << ',' << std::setprecision(1) << nps << ',' << std::setprecision(4) << ebf << ','
           << growth << ',' << move_text(result.best_move) << ',' << result.score << ','
           << join_pv(result.pv_line) << ',' << join_depth_reports(stats.completed_depths) << ','
           << stats.average_legal_moves() << ',' << stats.max_legal_moves << ',' << pseudo_avg
           << ',' << pseudo_to_legal << ',' << stats.tt_probes << ','
           << rate(stats.tt_hits, stats.tt_probes) << ','
           << rate(stats.tt_exact_hits, stats.tt_probes) << ','
           << rate(stats.tt_cutoffs, stats.tt_probes) << ',' << stats.tt_replacements << ','
           << stats.tt_collisions_detected << ',' << rate(stats.first_move_cutoffs, stats.cutoffs)
           << ',' << stats.average_cutoff_rank() << ',' << stats.killer_cutoffs << ','
           << stats.history_cutoffs << ',' << stats.cutoffs << ',' << stats.pvs_researches << ','
           << stats.aspiration_retries << ',' << stats.aspiration_fail_high << ','
           << stats.aspiration_fail_low << ',' << stats.movegen_ms << ',' << rules.legal_filter_ms
           << ',' << rules.apply_move_internal_ms << ',' << stats.propagation_ms << ','
           << rules.terminal_check_ms << ',' << stats.eval_ms << ',' << stats.move_order_ms << ','
           << stats.leq_calls << ',' << stats.leq_grouping_ms << ','
           << stats.leq_attempt_input_moves << ',' << stats.leq_attempt_output_representatives
           << ',' << leq_duplicate_ratio(stats) << ',' << stats.leq_rollback_low_duplicate_ratio
           << ',' << stats.leq_estimated_saved_children << ',' << stats.canonicalize_calls << ','
           << stats.canonicalize_ms << ',' << stats.undo_move_calls << ',' << stats.undo_move_ms
           << ',' << rules.hash_recompute_ms << ','
           << rate(stats.tt_move_present, stats.move_order_calls) << ','
           << rate(stats.tt_move_cutoffs, stats.cutoffs) << ',' << stats.tt_move_present << ','
           << stats.tt_move_cutoffs << ',' << stats.move_order_calls << ','
           << stats.immediate_win_ordering_calls << ',' << stats.prevent_loss_ordering_calls << ','
           << stats.killer_score_calls << ',' << stats.history_score_calls << ','
           << stats.eval_components.terminal.elapsed_ms << ','
           << stats.eval_components.material_mask.elapsed_ms << ','
           << stats.eval_components.mobility.elapsed_ms << ','
           << stats.eval_components.lion_safety.elapsed_ms << ','
           << stats.eval_components.immediate_setup.elapsed_ms << ','
           << stats.eval_components.immediate_movegen.elapsed_ms << ','
           << stats.eval_components.immediate_filter.elapsed_ms << ','
           << stats.eval_components.immediate_transition.elapsed_ms << ','
           << estimated_exclusive_movegen_ms << ',' << estimated_exclusive_legal_filter_ms << ','
           << estimated_exclusive_apply_ms << ',' << estimated_exclusive_propagation_ms << ','
           << estimated_exclusive_terminal_ms << ',' << estimated_exclusive_eval_ms << ','
           << estimated_exclusive_ordering_ms << ',' << estimated_exclusive_tt_ms << ','
           << estimated_exclusive_leq_ms << ',' << current_peak_rss_mb() << ','
           << current_page_faults() << ',' << illegal_count << ',' << histogram.str() << '\n';
    output.flush();
}

qas::SearchResult run_search(const CeilingFixture& fixture,
                             int depth,
                             int timeout_ms,
                             bool iterative,
                             LeqMode leq_mode,
                             int tt_mb) {
    qas::AlphaBetaEngine engine(tt_entries_from_mb(tt_mb));
    return engine.find_best_move(fixture.state,
                                 search_options(depth, timeout_ms, iterative, leq_mode, true));
}

qas::SearchResult run_search_with_propagation(const CeilingFixture& fixture,
                                              int depth,
                                              int timeout_ms,
                                              qas::PropagationMode propagation_mode,
                                              int tt_mb) {
    qas::AlphaBetaEngine engine(tt_entries_from_mb(tt_mb));
    auto options = search_options(depth, timeout_ms, false, LeqMode::Current, true);
    options.propagation_mode = propagation_mode;
    return engine.find_best_move(fixture.state, options);
}

std::vector<CeilingFixture> validation_fixtures(const std::vector<CeilingFixture>& corpus,
                                                const Options& options) {
    if (!options.fixture_names.empty())
        return select_fixtures(corpus, options);

    static const std::vector<std::string> names{"initial",
                                                "duplicate_hands",
                                                "high_uncertainty_midgame",
                                                "high_branching_hand_stack",
                                                "random_ply4_18",
                                                "random_hand_pieces_11",
                                                "random_duplicate_hands_02"};
    Options filtered = options;
    filtered.fixture_names = names;
    filtered.full_corpus = false;
    return select_fixtures(corpus, filtered);
}

void run_validation(const std::vector<CeilingFixture>& corpus, const Options& options) {
    const auto fixtures = validation_fixtures(corpus, options);
    std::filesystem::create_directories(options.out_dir);
    std::ofstream output(std::filesystem::path(options.out_dir) / "shallow_validation.csv");
    output << "fixture,depth,reference_completed,lut_completed,reference_score,lut_score,"
              "reference_best,lut_best,reference_nodes,lut_nodes,reference_illegal,lut_illegal,"
              "passed\n";
    int failures = 0;
    for (const CeilingFixture& fixture : fixtures) {
        for (int depth = 1; depth <= 3; ++depth) {
            const auto reference =
                run_search_with_propagation(fixture,
                                            depth,
                                            options.timeout_ms,
                                            qas::PropagationMode::PermutationReference,
                                            options.tt_mb);
            const auto lut = run_search_with_propagation(fixture,
                                                         depth,
                                                         options.timeout_ms,
                                                         qas::PropagationMode::LineageLut,
                                                         options.tt_mb);
            const bool reference_completed = reference.stats.depth_reached >= depth;
            const bool lut_completed = lut.stats.depth_reached >= depth;
            const int reference_illegal = best_move_is_legal(fixture.state, reference) ? 0 : 1;
            const int lut_illegal = best_move_is_legal(fixture.state, lut) ? 0 : 1;
            const bool passed = reference_completed && lut_completed &&
                                reference.score == lut.score &&
                                reference.has_move == lut.has_move &&
                                (!reference.has_move || reference.best_move == lut.best_move) &&
                                reference.stats.searched_nodes == lut.stats.searched_nodes &&
                                reference_illegal == 0 && lut_illegal == 0;
            if (!passed)
                ++failures;
            output << fixture.name << ',' << depth << ',' << csv_bool(reference_completed) << ','
                   << csv_bool(lut_completed) << ',' << reference.score << ',' << lut.score << ','
                   << move_text(reference.best_move) << ',' << move_text(lut.best_move) << ','
                   << reference.stats.searched_nodes << ',' << lut.stats.searched_nodes << ','
                   << reference_illegal << ',' << lut_illegal << ',' << csv_bool(passed) << '\n';
            output.flush();
            std::cout << "validate fixture=" << fixture.name << " depth=" << depth
                      << " passed=" << (passed ? "yes" : "no") << '\n';
        }
    }
    if (failures != 0)
        throw std::runtime_error("shallow validation failures: " + std::to_string(failures));
}

void run_fixed(const std::vector<CeilingFixture>& fixtures, const Options& options) {
    std::filesystem::create_directories(options.out_dir);
    std::ofstream output(std::filesystem::path(options.out_dir) / "fixed_depth.csv");
    write_search_header(output);
    for (int run = 1; run <= options.runs; ++run) {
        for (const CeilingFixture& fixture : fixtures) {
            std::uint64_t previous_nodes = 0;
            for (int depth = options.depth_min; depth <= options.depth_max; ++depth) {
                const auto result = run_search(
                    fixture, depth, options.timeout_ms, false, LeqMode::Current, options.tt_mb);
                write_search_row(output,
                                 run,
                                 fixture,
                                 "fixed",
                                 LeqMode::Current,
                                 options.tt_mb,
                                 depth,
                                 options.timeout_ms,
                                 result,
                                 previous_nodes);
                previous_nodes = result.stats.searched_nodes;
                std::cout << "fixed run=" << run << " fixture=" << fixture.name
                          << " depth=" << depth
                          << " completed=" << (result.stats.depth_reached >= depth ? "yes" : "no")
                          << " elapsed_ms=" << result.stats.elapsed_ms << '\n';
                if (result.stats.depth_reached < depth)
                    break;
            }
        }
    }
}

void run_iterative(const std::vector<CeilingFixture>& fixtures, const Options& options) {
    std::vector<int> limits{1000, 3000, 5000, 10000, 20000, 30000};
    if (options.timeout_ms >= 60000)
        limits.push_back(60000);
    std::filesystem::create_directories(options.out_dir);
    std::ofstream output(std::filesystem::path(options.out_dir) / "iterative.csv");
    write_search_header(output);
    for (int run = 1; run <= options.runs; ++run) {
        for (const CeilingFixture& fixture : fixtures) {
            for (int limit : limits) {
                const auto result =
                    run_search(fixture, 64, limit, true, LeqMode::Current, options.tt_mb);
                write_search_row(output,
                                 run,
                                 fixture,
                                 "iterative",
                                 LeqMode::Current,
                                 options.tt_mb,
                                 64,
                                 limit,
                                 result,
                                 0);
                std::cout << "iter run=" << run << " fixture=" << fixture.name
                          << " limit_ms=" << limit
                          << " depth_reached=" << result.stats.depth_reached
                          << " elapsed_ms=" << result.stats.elapsed_ms << '\n';
            }
        }
    }
}

void run_tt_matrix(const std::vector<CeilingFixture>& fixtures, const Options& options) {
    static constexpr std::array<int, 5> sizes{64, 128, 256, 512, 1024};
    std::filesystem::create_directories(options.out_dir);
    std::ofstream output(std::filesystem::path(options.out_dir) / "tt_matrix.csv");
    write_search_header(output);
    for (int run = 1; run <= options.runs; ++run) {
        for (const CeilingFixture& fixture : fixtures) {
            for (int tt_mb : sizes) {
                for (int depth : {10, 11}) {
                    const auto result = run_search(
                        fixture, depth, options.timeout_ms, false, LeqMode::Current, tt_mb);
                    write_search_row(output,
                                     run,
                                     fixture,
                                     "tt_fixed",
                                     LeqMode::Current,
                                     tt_mb,
                                     depth,
                                     options.timeout_ms,
                                     result,
                                     0);
                    std::cout << "tt run=" << run << " fixture=" << fixture.name
                              << " tt_mb=" << tt_mb << " depth=" << depth << " completed="
                              << (result.stats.depth_reached >= depth ? "yes" : "no")
                              << " elapsed_ms=" << result.stats.elapsed_ms << '\n';
                    if (result.stats.depth_reached < depth)
                        break;
                }
                const auto iterative =
                    run_search(fixture, 64, 30000, true, LeqMode::Current, tt_mb);
                write_search_row(output,
                                 run,
                                 fixture,
                                 "tt_iterative",
                                 LeqMode::Current,
                                 tt_mb,
                                 64,
                                 30000,
                                 iterative,
                                 0);
            }
        }
    }
}

void run_leq_matrix(const std::vector<CeilingFixture>& fixtures, const Options& options) {
    static constexpr std::array<LeqMode, 8> modes{LeqMode::Off,
                                                  LeqMode::Current,
                                                  LeqMode::Relaxed,
                                                  LeqMode::Strict,
                                                  LeqMode::TargetA,
                                                  LeqMode::TargetB,
                                                  LeqMode::TargetC,
                                                  LeqMode::Tactical};
    std::filesystem::create_directories(options.out_dir);
    std::ofstream output(std::filesystem::path(options.out_dir) / "leq_matrix.csv");
    write_search_header(output);
    for (int run = 1; run <= options.runs; ++run) {
        for (const CeilingFixture& fixture : fixtures) {
            const auto meta = metadata(fixture.state);
            if (options.fixture_names.empty() && !meta.duplicate_hand_hint &&
                fixture.name != "many_hands" && fixture.name != "high_branching_hand_stack")
                continue;
            for (LeqMode mode : modes) {
                for (int depth = options.depth_min; depth <= options.depth_max; ++depth) {
                    const auto result =
                        run_search(fixture, depth, options.timeout_ms, false, mode, options.tt_mb);
                    write_search_row(output,
                                     run,
                                     fixture,
                                     "leq",
                                     mode,
                                     options.tt_mb,
                                     depth,
                                     options.timeout_ms,
                                     result,
                                     0);
                    std::cout << "leq run=" << run << " fixture=" << fixture.name
                              << " mode=" << leq_mode_name(mode) << " depth=" << depth
                              << " completed="
                              << (result.stats.depth_reached >= depth ? "yes" : "no")
                              << " elapsed_ms=" << result.stats.elapsed_ms << '\n';
                    if (result.stats.depth_reached < depth)
                        break;
                }
                const auto iterative = run_search(fixture, 64, 30000, true, mode, options.tt_mb);
                write_search_row(output,
                                 run,
                                 fixture,
                                 "leq_iterative",
                                 mode,
                                 options.tt_mb,
                                 64,
                                 30000,
                                 iterative,
                                 0);
            }
        }
    }
}

std::vector<std::string> split_names(const std::string& text) {
    std::vector<std::string> names;
    std::string current;
    std::istringstream input(text);
    while (std::getline(input, current, ',')) {
        if (!current.empty())
            names.push_back(current);
    }
    return names;
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&](const std::string& name) -> std::string {
            if (index + 1 >= argc)
                throw std::runtime_error("missing value for " + name);
            return argv[++index];
        };
        if (arg == "--mode")
            options.mode = require_value(arg);
        else if (arg == "--out-dir")
            options.out_dir = require_value(arg);
        else if (arg == "--full-corpus")
            options.full_corpus = true;
        else if (arg == "--max-fixtures")
            options.max_fixtures = std::stoi(require_value(arg));
        else if (arg == "--runs")
            options.runs = std::max(1, std::stoi(require_value(arg)));
        else if (arg == "--depth-min")
            options.depth_min = std::stoi(require_value(arg));
        else if (arg == "--depth-max")
            options.depth_max = std::stoi(require_value(arg));
        else if (arg == "--timeout-ms")
            options.timeout_ms = std::stoi(require_value(arg));
        else if (arg == "--tt-mb")
            options.tt_mb = std::stoi(require_value(arg));
        else if (arg == "--shard-count")
            options.shard_count = std::max(1, std::stoi(require_value(arg)));
        else if (arg == "--shard-index")
            options.shard_index = std::max(0, std::stoi(require_value(arg)));
        else if (arg == "--fixtures")
            options.fixture_names = split_names(require_value(arg));
        else
            throw std::runtime_error("unknown argument: " + arg);
    }
    if (options.shard_index >= options.shard_count)
        throw std::runtime_error("--shard-index must be less than --shard-count");
    return options;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        const auto corpus = build_corpus();
        write_fixture_files(corpus, options.out_dir);
        const auto selected = select_fixtures(corpus, options);
        std::cout << "Stage 5.5 corpus fixtures=" << corpus.size()
                  << " selected=" << selected.size() << " out_dir=" << options.out_dir
                  << " shard=" << options.shard_index << '/' << options.shard_count << '\n';

        if (options.mode == "corpus")
            return EXIT_SUCCESS;
        if (options.mode == "validate") {
            run_validation(corpus, options);
            return EXIT_SUCCESS;
        }
        if (options.mode == "fixed" || options.mode == "quick")
            run_fixed(selected, options);
        if (options.mode == "iterative" || options.mode == "quick")
            run_iterative(selected, options);
        if (options.mode == "tt" || options.mode == "quick")
            run_tt_matrix(selected, options);
        if (options.mode == "leq" || options.mode == "quick")
            run_leq_matrix(selected, options);
        if (options.mode == "all") {
            run_fixed(selected, options);
            run_iterative(selected, options);
            run_tt_matrix(selected, options);
            run_leq_matrix(selected, options);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "stage55 ceiling benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

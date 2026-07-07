#pragma once

#include "rules/game.hpp"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qas {

inline constexpr int mate_score = 1'000'000;
inline constexpr int search_infinity = 1'100'000;

enum class BoundType : std::uint8_t { Exact, Lower, Upper };

struct SearchOptions {
    using StopPolicy = bool (*)(std::uint64_t searched_nodes, void* context);
    int max_depth{8};
    int time_limit_ms{1000};
    bool use_tt{true};
    using SuccessorReducer = std::vector<Move> (*)(const State&, const std::vector<Move>&,
                                                    const Move&, std::size_t, bool&);
    SuccessorReducer successor_reducer{nullptr};
    std::size_t reducer_threshold{12};
    StopPolicy stop_policy{nullptr};
    void* stop_context{nullptr};
};

struct DepthReport {
    int depth{0};
    int score{0};
    Move best_move{};
    std::uint64_t nodes{0};
    double elapsed_ms{0.0};
};

struct SearchStats {
    std::uint64_t searched_nodes{0};
    std::uint64_t expanded_nodes{0};
    std::uint64_t cutoffs{0};
    std::uint64_t generated_legal_moves{0};
    std::uint64_t equivalent_successor_moves{0};
    std::uint64_t leq_grouped_nodes{0};
    std::uint64_t tt_probes{0};
    std::uint64_t tt_hits{0};
    int depth_reached{0};
    double elapsed_ms{0.0};
    double leq_grouping_ms{0.0};
    std::vector<DepthReport> completed_depths;

    double average_legal_moves() const {
        return expanded_nodes == 0
                   ? 0.0
                   : static_cast<double>(generated_legal_moves) /
                         static_cast<double>(expanded_nodes);
    }
    double average_equivalent_moves() const {
        return expanded_nodes == 0
                   ? 0.0
                   : static_cast<double>(equivalent_successor_moves) /
                         static_cast<double>(expanded_nodes);
    }
    double duplicate_ratio() const {
        return generated_legal_moves == 0
                   ? 0.0
                   : 1.0 - static_cast<double>(equivalent_successor_moves) /
                               static_cast<double>(generated_legal_moves);
    }
};

struct SearchResult {
    Move best_move{};
    int score{0};
    bool has_move{false};
    SearchStats stats{};
};

struct TTEntry {
    std::uint64_t key{0};
    std::int32_t score{0};
    std::int16_t depth{-1};
    BoundType bound{BoundType::Exact};
    Move best_move{};
    std::uint8_t age{0};
};

class AlphaBetaEngine {
public:
    explicit AlphaBetaEngine(std::size_t table_entries = 1U << 18U);

    SearchResult find_best_move(const State& root, const SearchOptions& options = {});
    SearchResult search_fixed_depth(const State& root, int depth, bool use_tt = true);
    int evaluate(const State& state) const;
    void clear_tt();

private:
    std::vector<TTEntry> table_;
    std::size_t table_mask_{0};
    SearchStats stats_{};
    SearchOptions options_{};
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point deadline_{};
    bool stopped_{false};
    std::uint8_t age_{0};
    std::array<std::array<Move, 2>, 64> killers_{};
    std::array<std::array<int, board_size>, physical_piece_count> history_{};

    int negamax(State& state, int depth, int alpha, int beta, int ply);
    bool search_root(State& state, int depth, Move& best_move, int& best_score);
    bool should_stop();
    int terminal_score(const State& state, int ply) const;
    int move_order_score(const State& state, const Move& move, const Move& tt_move, int ply) const;
    void order_moves(const State& state, std::vector<Move>& moves, const Move& tt_move,
                     int ply) const;
    std::vector<Move> generate_search_moves(const State& state, const Move& tt_move);
    TTEntry* probe(std::uint64_t key);
    const TTEntry* probe(std::uint64_t key) const;
    void store(std::uint64_t key, int depth, int score, BoundType bound, const Move& move);
};

}  // namespace qas

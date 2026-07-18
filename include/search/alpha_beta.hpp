#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "rules/game.hpp"

namespace qas {

inline constexpr int mate_score = 1'000'000;
inline constexpr int search_infinity = 1'100'000;
inline constexpr std::uint16_t kInvalidPackedMove = (std::numeric_limits<std::uint16_t>::max)();

enum class BoundType : std::uint8_t { Exact, Lower, Upper };

struct SuccessorReductionStats {
    std::uint64_t canonicalize_calls{0};
    std::uint64_t propagation_calls{0};
    double canonicalize_ms{0.0};
    double propagation_ms{0.0};
};

struct SearchOptions {
    using StopPolicy = bool (*)(std::uint64_t searched_nodes, void* context);
    int max_depth{8};
    int time_limit_ms{1000};  // legacy alias used when soft/hard are zero
    int soft_time_limit_ms{0};
    int hard_time_limit_ms{0};
    bool iterative_deepening_enabled{true};
    bool pvs_enabled{true};
    bool aspiration_enabled{true};
    int aspiration_initial_window{50};
    int aspiration_max_retries{3};
    bool use_tt{true};
    bool tt_clear_each_move{false};
    bool tt_age_enabled{true};
    bool strong_ordering_enabled{true};
    bool tt_move_ordering_enabled{true};
    bool immediate_win_ordering_enabled{true};
    bool prevent_loss_ordering_enabled{true};
    bool capture_ordering_enabled{true};
    bool lion_reduction_ordering_enabled{true};
    bool try_threat_ordering_enabled{true};
    bool mask_collapse_ordering_enabled{true};
    bool killer_enabled{true};
    bool history_enabled{true};
    bool benchmark_instrumentation_enabled{false};
    bool optimized_eval_enabled{true};
    PropagationMode propagation_mode{PropagationMode::LineageLut};
    std::uint64_t history_decay_interval{65'536};
    using SuccessorReducer = std::vector<Move> (*)(const State&,
                                                   const std::vector<Move>&,
                                                   const Move&,
                                                   std::size_t,
                                                   int,
                                                   int,
                                                   double,
                                                   bool,
                                                   bool,
                                                   bool&,
                                                   SuccessorReductionStats*);
    SuccessorReducer successor_reducer{nullptr};
    std::size_t reducer_threshold{12};
    int reducer_min_depth{2};
    double reducer_min_duplicate_ratio{0.0};
    bool reducer_require_duplicate_hint{true};
    bool reducer_disable_low_time{true};
    StopPolicy stop_policy{nullptr};
    void* stop_context{nullptr};
};

struct DepthReport {
    int depth{0};
    int score{0};
    Move best_move{};
    std::uint64_t nodes{0};
    double elapsed_ms{0.0};
    std::vector<Move> pv_line{};
};

struct TimedCounter {
    std::uint64_t calls{0};
    double elapsed_ms{0.0};
};

struct EvalComponentProfile {
    TimedCounter terminal{};
    TimedCounter material_mask{};
    TimedCounter mobility{};
    TimedCounter lion_safety{};
    TimedCounter immediate_setup{};
    TimedCounter immediate_movegen{};
    TimedCounter immediate_filter{};
    TimedCounter immediate_transition{};
};

struct SearchStats {
    std::uint64_t searched_nodes{0};
    std::uint64_t expanded_nodes{0};
    std::uint64_t cutoffs{0};
    std::uint64_t generated_legal_moves{0};
    std::uint64_t max_legal_moves{0};
    std::uint64_t equivalent_successor_moves{0};
    std::uint64_t leq_grouped_nodes{0};
    std::uint64_t tt_probes{0};
    std::uint64_t tt_hits{0};
    std::uint64_t tt_exact_hits{0};
    std::uint64_t tt_lower_hits{0};
    std::uint64_t tt_upper_hits{0};
    std::uint64_t first_move_cutoffs{0};
    std::uint64_t cutoff_rank_sum{0};
    std::uint64_t tt_move_used{0};
    std::uint64_t tt_move_cutoffs{0};
    std::uint64_t tt_replacements{0};
    std::uint64_t tt_collisions_detected{0};
    std::uint64_t tt_age_replacements{0};
    std::uint64_t tt_depth_replacements{0};
    std::uint64_t killer_cutoffs{0};
    std::uint64_t history_cutoffs{0};
    std::uint64_t pvs_researches{0};
    std::uint64_t aspiration_retries{0};
    std::uint64_t leq_skipped_moves{0};
    std::uint64_t leq_raw_moves{0};
    std::uint64_t leq_group_moves{0};
    std::uint64_t canonicalize_calls{0};
    std::uint64_t propagation_calls{0};
    std::uint64_t movegen_calls{0};
    std::uint64_t eval_calls{0};
    int depth_reached{0};
    int started_depth{0};
    int timeout_depth{0};
    bool timeout_hit{false};
    double elapsed_ms{0.0};
    double leq_grouping_ms{0.0};
    double canonicalize_ms{0.0};
    double propagation_ms{0.0};
    double movegen_ms{0.0};
    std::uint64_t move_order_calls{0};
    double move_order_ms{0.0};
    double eval_ms{0.0};
    RuleMetrics rule_metrics{};
    EvalComponentProfile eval_components{};
    std::vector<DepthReport> completed_depths;

    /// @brief Computes the mean raw legal branching factor of expanded nodes.
    /// @return Average legal moves, or zero when no node was expanded.
    double average_legal_moves() const {
        return expanded_nodes == 0 ? 0.0
                                   : static_cast<double>(generated_legal_moves) /
                                         static_cast<double>(expanded_nodes);
    }
    /// @brief Computes the mean branching factor after successor reduction.
    /// @return Average retained moves, or zero when no node was expanded.
    double average_equivalent_moves() const {
        return expanded_nodes == 0 ? 0.0
                                   : static_cast<double>(equivalent_successor_moves) /
                                         static_cast<double>(expanded_nodes);
    }
    /// @brief Computes the fraction of generated moves removed as equivalent successors.
    /// @return Duplicate ratio in `[0, 1]`, or zero when no move was generated.
    double duplicate_ratio() const {
        return generated_legal_moves == 0 ? 0.0
                                          : 1.0 - static_cast<double>(equivalent_successor_moves) /
                                                      static_cast<double>(generated_legal_moves);
    }
    /// @brief Computes the mean move rank that produced a beta cutoff.
    /// @return Average one-based cutoff rank, or zero when no cutoff occurred.
    double average_cutoff_rank() const {
        return cutoffs == 0 ? 0.0 : static_cast<double>(cutoff_rank_sum) / cutoffs;
    }
};

struct SearchResult {
    Move best_move{};
    int score{0};
    bool has_move{false};
    std::vector<Move> pv_line{};
    SearchStats stats{};
};

struct TTEntry {
    std::uint64_t key{0};
    std::int32_t score{0};
    std::int16_t depth{-1};
    std::uint16_t move16{kInvalidPackedMove};
    std::uint8_t flag{static_cast<std::uint8_t>(BoundType::Exact)};
    std::uint8_t age{0};
    std::uint8_t generation{0};
    std::uint8_t padding{0};
};

class AlphaBetaEngine {
   public:
    /// @brief Constructs a search engine with a fixed-size transposition table.
    /// @param table_entries Power-of-two number of table entries.
    /// @throws std::invalid_argument If the entry count is zero or not a power of two.
    explicit AlphaBetaEngine(std::size_t table_entries = 1U << 18U);

    /// @brief Searches for the best legal root move under runtime options.
    /// @param root Immutable root state.
    /// @param options Search depth, time, ordering and instrumentation options.
    /// @return Best completed-depth result with a legal timeout fallback when available.
    SearchResult find_best_move(const State& root, const SearchOptions& options = {});

    /// @brief Runs the standard search path with a fixed depth and long deadline.
    /// @param root Immutable root state.
    /// @param depth Requested search depth, clamped to at least one.
    /// @param use_tt Whether transposition-table probing and storage are enabled.
    /// @return Search result from the requested fixed depth.
    SearchResult search_fixed_depth(const State& root, int depth, bool use_tt = true);

    /// @brief Evaluates a state from its side-to-move perspective.
    /// @param state State to evaluate without mutation.
    /// @return Heuristic search score.
    int evaluate(const State& state) const;

    /// @brief Clears every transposition-table entry.
    void clear_tt();

    /// @brief Returns the number of allocated transposition-table entries.
    /// @return Fixed table entry count.
    std::size_t tt_entry_count() const { return table_.size(); }

    /// @brief Returns transposition-table storage in bytes.
    /// @return Entry count multiplied by entry size.
    std::size_t tt_bytes() const { return table_.size() * sizeof(TTEntry); }

   private:
    struct EvalScratch {
        std::array<std::uint16_t, 2> occupancy{};
        std::array<std::uint16_t, 2> attack_mask{};
        std::array<int, 2> mobility{};
        std::array<int, 2> lion_candidates{};
        std::vector<Move> pseudo_moves{};
        std::vector<Move> immediate_candidates{};

        /// @brief Reserves reusable evaluation buffers for the recursive hot path.
        EvalScratch();
    };

    std::vector<TTEntry> table_;
    std::size_t table_mask_{0};
    SearchStats stats_{};
    SearchOptions options_{};
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point soft_deadline_{};
    std::chrono::steady_clock::time_point hard_deadline_{};
    bool stopped_{false};
    std::uint8_t age_{0};
    std::array<std::array<Move, 2>, 64> killers_{};
    std::array<std::array<std::array<int, board_size>, external_source_count>, 2> history_{};
    std::uint64_t history_updates_{0};
    static constexpr std::size_t kMaxSearchPly = 256;
    std::array<std::vector<Move>, kMaxSearchPly> move_pool_{};
    std::array<std::vector<Move>, kMaxSearchPly> candidate_pool_{};
    std::array<std::vector<std::pair<int, Move>>, kMaxSearchPly> score_pool_{};
    std::array<std::vector<Move>, kMaxSearchPly> pv_pool_{};
    EvalScratch eval_scratch_{};

    /// @brief Searches one recursive negamax node with alpha-beta and optional PVS.
    /// @param state Mutable node state restored after every child.
    /// @param depth Remaining search depth.
    /// @param alpha Lower search-window bound.
    /// @param beta Upper search-window bound.
    /// @param ply Distance from the root.
    /// @return Node score, or zero when the search stops mid-node.
    int negamax(State& state, int depth, int alpha, int beta, int ply);

    /// @brief Searches all root moves inside one aspiration window.
    /// @param state Mutable root copy restored after every child.
    /// @param depth Requested root depth.
    /// @param alpha Lower root-window bound.
    /// @param beta Upper root-window bound.
    /// @param best_move Receives the best move when the iteration completes.
    /// @param best_score Receives the best score when the iteration completes.
    /// @return `false` when interrupted; otherwise `true`, including no-move roots.
    bool search_root(
        State& state, int depth, int alpha, int beta, Move& best_move, int& best_score);

    /// @brief Polls the injected stop policy and hard deadline.
    /// @return `true` after any stop condition has latched.
    bool should_stop();

    /// @brief Converts a terminal rules result into a mate-distance search score.
    /// @param state Terminal state.
    /// @param ply Distance from the root.
    /// @return Score from the current side-to-move perspective.
    int terminal_score(const State& state, int ply) const;

    /// @brief Scores one move for deterministic search ordering.
    /// @param state Source state.
    /// @param move Move to score.
    /// @param tt_move Preferred transposition-table move, if valid.
    /// @param ply Current search ply.
    /// @return Higher-is-earlier ordering score.
    int move_order_score(const State& state, const Move& move, const Move& tt_move, int ply);

    /// @brief Stable-sorts legal moves by their ordering score.
    /// @param state Source state.
    /// @param moves Move buffer to reorder in place.
    /// @param tt_move Preferred transposition-table move, if valid.
    /// @param ply Current search ply.
    void order_moves(const State& state, std::vector<Move>& moves, const Move& tt_move, int ply);

    /// @brief Generates legal moves and optionally reduces equivalent successors.
    /// @param state Source state.
    /// @param tt_move Preferred representative move for reduction.
    /// @param depth Remaining search depth.
    /// @param ply Current search ply used to select reusable buffers.
    /// @param moves Buffer receiving retained moves.
    void generate_search_moves(
        const State& state, const Move& tt_move, int depth, int ply, std::vector<Move>& moves);

    /// @brief Returns the mutable direct-mapped transposition slot for a key.
    /// @param key Full Zobrist key.
    /// @return Pointer to the selected table slot.
    TTEntry* probe(std::uint64_t key);

    /// @brief Returns the immutable direct-mapped transposition slot for a key.
    /// @param key Full Zobrist key.
    /// @return Pointer to the selected table slot.
    const TTEntry* probe(std::uint64_t key) const;

    /// @brief Stores a search result according to depth and age replacement policy.
    /// @param key Full Zobrist key.
    /// @param depth Searched remaining depth.
    /// @param score Node score in root-relative mate-distance form.
    /// @param bound Bound semantics of the score.
    /// @param move Best move associated with the entry.
    /// @param ply Distance from the root used for mate-score normalization.
    void store(std::uint64_t key, int depth, int score, BoundType bound, const Move& move, int ply);

    /// @brief Evaluates a search leaf and records optional instrumentation.
    /// @param state Leaf state.
    /// @return Heuristic score from the side-to-move perspective.
    int evaluate_search(const State& state);

    /// @brief Runs the reference evaluator used for differential verification.
    /// @param state State to evaluate.
    /// @param profile Optional per-component timing sink.
    /// @return Reference heuristic score.
    int evaluate_baseline(const State& state, EvalComponentProfile* profile) const;

    /// @brief Runs the allocation-conscious optimized evaluator.
    /// @param state State to evaluate.
    /// @param scratch Reusable evaluation workspace.
    /// @param profile Optional per-component timing sink.
    /// @return Optimized heuristic score.
    int evaluate_optimized(const State& state,
                           EvalScratch& scratch,
                           EvalComponentProfile* profile) const;
    /// @brief Applies a move through rules and accumulates optional search metrics.
    /// @param state Search state to mutate.
    /// @param move Legal candidate move.
    /// @param undo Receives the pre-move state.
    /// @return `true` when the transition succeeds.
    bool apply_search_move(State& state, const Move& move, Undo& undo);

    /// @brief Rebuilds the principal-variation prefix at a ply.
    /// @param ply Ply whose variation buffer is updated.
    /// @param move New best move at the ply.
    void update_pv(int ply, const Move& move);
};

static_assert(sizeof(TTEntry) <= 24, "TTEntry must remain cache-conscious");

}  // namespace qas

#include "search/guarded_search.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "search/dfpn.hpp"

namespace qas {
namespace {

using Clock = std::chrono::steady_clock;

/// @brief Tests whether a move is listed.
bool listed(const std::vector<Move>& moves, const Move& move) {
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

/// @brief Milliseconds elapsed since a start point.
int elapsed_ms(Clock::time_point start) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

/// @brief Shared state between the decision thread and the df-pn worker.
struct WorkerContext {
    RootGuard shared;
    std::atomic<bool> stop{false};
    std::atomic<bool> own_win_found{false};
    Move own_win{};
};

/// @brief Worker body: own-win attempt, then refutation passes with growing time slices.
void run_worker(WorkerContext& context,
                const State& root,
                const std::vector<Move>& legal,
                const GuardOptions& guard,
                int total_ms,
                Clock::time_point end_time) {
    const Side us = root.side_to_move;
    DfpnSolver own_solver(guard.own_table_entries);
    own_solver.reset(us, guard.max_ply);
    const auto own_deadline =
        std::min(end_time, Clock::now() + std::chrono::milliseconds(std::max(50, total_ms / 8)));
    const DfpnResult own = own_solver.prove(root, own_deadline, ~std::uint64_t{0}, &context.stop);
    if (own.status == ProofStatus::Proven && own.winning_move.valid()) {
        context.own_win = own.winning_move;
        context.own_win_found = true;
        context.shared.stop_search = true;
        return;
    }

    DfpnSolver refuter(guard.refute_table_entries);
    refuter.reset(opposite(us), guard.max_ply);
    std::vector<Move> refuted;
    std::vector<Move> cleared;
    int slice_ms = std::max(20, total_ms / 60);
    while (!context.stop.load() && Clock::now() < end_time) {
        std::vector<Move> queue;
        Move best_at_start;
        {
            std::lock_guard<std::mutex> lock(context.shared.mutex);
            best_at_start = context.shared.best;
            if (best_at_start.valid())
                queue.push_back(best_at_start);
            queue.insert(
                queue.end(), context.shared.root_order.begin(), context.shared.root_order.end());
        }
        queue.insert(queue.end(), legal.begin(), legal.end());
        std::vector<Move> pass;
        for (const Move& move : queue) {
            if (!listed(refuted, move) && !listed(cleared, move) && !listed(pass, move))
                pass.push_back(move);
        }
        if (pass.empty())
            return;
        for (const Move& move : pass) {
            if (context.stop.load() || Clock::now() >= end_time)
                return;
            State after = root;
            Undo undo;
            if (!apply_move(after, move, undo) || after.terminal != Terminal::None) {
                cleared.push_back(move);
                continue;
            }
            const auto deadline =
                std::min(end_time, Clock::now() + std::chrono::milliseconds(slice_ms));
            const ProofStatus verdict =
                refuter.prove(after, deadline, ~std::uint64_t{0}, &context.stop).status;
            if (verdict == ProofStatus::Proven) {
                refuted.push_back(move);
                std::lock_guard<std::mutex> lock(context.shared.mutex);
                if (refuted.size() < legal.size())
                    context.shared.excluded = refuted;
            } else if (verdict == ProofStatus::Disproven) {
                cleared.push_back(move);
            }
            std::lock_guard<std::mutex> lock(context.shared.mutex);
            if (context.shared.best != best_at_start)
                break;  // re-prioritize around the new alpha-beta best move
        }
        slice_ms = std::min(slice_ms * 2, std::max(200, total_ms / 3));
    }
}

}  // namespace

SearchResult guarded_find_best_move(AlphaBetaEngine& engine,
                                    const State& root,
                                    const SearchOptions& options,
                                    const GuardOptions& guard,
                                    GuardReport* report) {
    GuardReport local;
    const std::vector<Move> legal = generate_legal_moves(root);
    if (!guard.enabled || root.terminal != Terminal::None || legal.size() <= 1) {
        if (report != nullptr)
            *report = local;
        return engine.find_best_move(root, options);
    }

    // Statically excluded moves (e.g. an offline forbidden list) are never refuted again or
    // re-allowed; when every legal move is excluded, all of them stay candidates.
    std::vector<Move> candidates;
    for (const Move& move : legal) {
        if (!listed(options.root_excluded, move))
            candidates.push_back(move);
    }
    if (candidates.empty())
        candidates = legal;

    const auto start = Clock::now();
    const int total_ms =
        options.hard_time_limit_ms > 0 ? options.hard_time_limit_ms : options.time_limit_ms;
    const int reserve_ms = std::clamp(total_ms / 10, 150, std::max(150, guard.reserve_cap_ms));
    const auto end_time = start + std::chrono::milliseconds(std::max(0, total_ms - 150));
    WorkerContext context;
    std::thread worker([&]() { run_worker(context, root, candidates, guard, total_ms, end_time); });

    SearchOptions alpha_beta = options;
    alpha_beta.root_guard = &context.shared;
    alpha_beta.hard_time_limit_ms = alpha_beta.time_limit_ms = std::max(50, total_ms - reserve_ms);
    alpha_beta.soft_time_limit_ms =
        options.soft_time_limit_ms > 0
            ? std::min(options.soft_time_limit_ms, alpha_beta.hard_time_limit_ms * 25 / 29)
            : 0;
    SearchResult result = engine.find_best_move(root, alpha_beta);

    std::vector<Move> excluded;
    {
        std::lock_guard<std::mutex> lock(context.shared.mutex);
        excluded = context.shared.excluded;
    }
    context.stop = true;
    worker.join();

    if (context.own_win_found.load()) {
        local.own_forced_win = true;
        result.best_move = context.own_win;
        result.has_move = true;
        result.score = mate_score;
    } else if (listed(excluded, result.best_move) && excluded.size() < candidates.size()) {
        const int left = total_ms - elapsed_ms(start) - 100;
        if (left > 50) {
            SearchOptions again = options;
            again.root_excluded.insert(again.root_excluded.end(), excluded.begin(), excluded.end());
            again.hard_time_limit_ms = again.time_limit_ms = left;
            again.soft_time_limit_ms = left * 25 / 29;
            const SearchResult replacement = engine.find_best_move(root, again);
            if (replacement.has_move && !listed(excluded, replacement.best_move)) {
                result = replacement;
                local.re_searched = true;
            }
        }
    }
    local.excluded_moves = static_cast<int>(excluded.size());
    if (report != nullptr)
        *report = local;
    return result;
}

}  // namespace qas

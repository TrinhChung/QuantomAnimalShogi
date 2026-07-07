#include "search/alpha_beta.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace qas {
namespace {

int popcount(Mask mask) {
    int count = 0;
    for (; mask != 0; mask = static_cast<Mask>(mask & (mask - 1))) ++count;
    return count;
}

int expected_piece_value(Mask mask) {
    constexpr std::array<int, 5> values{100, 320, 310, 1050, 420};
    int total = 0;
    int count = 0;
    for (Animal form : all_forms) {
        if (contains(mask, form)) {
            total += values[static_cast<std::size_t>(form)];
            ++count;
        }
    }
    return count == 0 ? 0 : total / count;
}

int lion_candidate_count(const State& state, Side owner) {
    int count = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (owned_by(state, piece, owner) && contains(state.mask[piece], Animal::Lion)) ++count;
    }
    return count;
}

int geometric_mobility(const State& state, Side side) {
    int result = 0;
    const auto& tables = move_tables();
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (!owned_by(state, piece, side)) continue;
        if (is_hand_position(state.pos[piece])) {
            for (int square = 0; square < board_size; ++square) {
                if (state.board[square] == -1) ++result;
            }
            continue;
        }
        const auto destinations =
            tables.quantum_move_table[state.mask[piece]][side_index(side)][state.pos[piece]];
        for (int square = 0; square < board_size; ++square) {
            if ((destinations & (1U << square)) == 0) continue;
            const int occupant = state.board[square];
            if (occupant < 0 || !owned_by(state, occupant, side)) ++result;
        }
    }
    return result;
}

bool side_has_immediate_win(const State& state, Side side) {
    State copy = state;
    copy.side_to_move = side;
    copy.terminal = Terminal::None;
    copy.winner = -1;
    recompute_hash(copy);
    const auto moves = generate_pseudo_legal_moves(copy);
    for (const Move& move : moves) {
        Undo undo;
        if (apply_move(copy, move, undo) &&
            (copy.terminal == Terminal::Catch || copy.terminal == Terminal::Try)) {
            return true;
        }
        copy = state;
        copy.side_to_move = side;
        copy.terminal = Terminal::None;
        copy.winner = -1;
        recompute_hash(copy);
    }
    return false;
}

int progress_to_try(int square, Side side) {
    if (square >= board_size) return 0;
    const int row = square / board_width;
    return side == Side::South ? board_height - 1 - row : row;
}

}  // namespace

AlphaBetaEngine::AlphaBetaEngine(std::size_t table_entries) {
    if (table_entries == 0 || (table_entries & (table_entries - 1)) != 0) {
        throw std::invalid_argument("transposition table size must be a power of two");
    }
    table_.resize(table_entries);
    table_mask_ = table_entries - 1;
}

void AlphaBetaEngine::clear_tt() {
    std::fill(table_.begin(), table_.end(), TTEntry{});
}

TTEntry* AlphaBetaEngine::probe(std::uint64_t key) { return &table_[key & table_mask_]; }
const TTEntry* AlphaBetaEngine::probe(std::uint64_t key) const {
    return &table_[key & table_mask_];
}

void AlphaBetaEngine::store(std::uint64_t key, int depth, int score, BoundType bound,
                            const Move& move) {
    if (!options_.use_tt) return;
    TTEntry& entry = *probe(key);
    if (entry.key != key && entry.age == age_ && entry.depth > depth) return;
    if (entry.key == key && entry.depth > depth && entry.bound == BoundType::Exact) return;
    entry.key = key;
    entry.depth = static_cast<std::int16_t>(depth);
    entry.score = score;
    entry.bound = bound;
    entry.best_move = move;
    entry.age = age_;
}

bool AlphaBetaEngine::should_stop() {
    if (stopped_) return true;
    if (options_.stop_policy != nullptr &&
        options_.stop_policy(stats_.searched_nodes, options_.stop_context)) {
        stopped_ = true;
        return true;
    }
    if ((stats_.searched_nodes & 255U) == 0U &&
        std::chrono::steady_clock::now() >= deadline_) {
        stopped_ = true;
    }
    return stopped_;
}

int AlphaBetaEngine::terminal_score(const State& state, int ply) const {
    if (state.terminal == Terminal::Draw) return 0;
    if (state.terminal == Terminal::Catch || state.terminal == Terminal::Try) {
        const bool current_won = state.winner == side_index(state.side_to_move);
        return current_won ? mate_score - ply : -mate_score + ply;
    }
    if (state.terminal == Terminal::Illegal) return -mate_score + ply;
    return 0;
}

int AlphaBetaEngine::evaluate(const State& state) const {
    if (state.terminal != Terminal::None) return terminal_score(state, 0);

    int south_score = 0;
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        const Side owner = owned_by(state, piece, Side::South) ? Side::South : Side::North;
        const int sign = owner == Side::South ? 1 : -1;
        const Mask mask = state.mask[piece];
        south_score += sign * expected_piece_value(mask);

        const int uncertainty = popcount(mask);
        south_score += sign * (uncertainty - 1) * 12;

        if (contains(mask, Animal::Lion)) {
            const bool on_board = state.pos[piece] < board_size;
            const Side enemy = opposite(owner);
            if (on_board && is_square_attacked(state, state.pos[piece], enemy)) {
                south_score -= sign * (mask == bit(Animal::Lion) ? 700 : 180);
            }
            if (on_board) {
                south_score += sign * progress_to_try(state.pos[piece], owner) *
                               (mask == bit(Animal::Lion) ? 90 : 25);
            }
            if (mask == bit(Animal::Lion) && !on_board) south_score -= sign * 900;
        }
    }

    south_score += 18 * (geometric_mobility(state, Side::South) -
                         geometric_mobility(state, Side::North));
    south_score += 80 * (lion_candidate_count(state, Side::South) -
                         lion_candidate_count(state, Side::North));

    if (side_has_immediate_win(state, Side::South)) south_score += 3500;
    if (side_has_immediate_win(state, Side::North)) south_score -= 3500;

    return state.side_to_move == Side::South ? south_score : -south_score;
}

int AlphaBetaEngine::move_order_score(const State& state, const Move& move,
                                      const Move& tt_move, int ply) const {
    if (move == tt_move) return 2'000'000;
    int score = 0;
    if (ply < static_cast<int>(killers_.size())) {
        if (move == killers_[ply][0]) score += 180'000;
        else if (move == killers_[ply][1]) score += 160'000;
    }
    score += history_[move.piece][move.to];
    const int target = state.board[move.to];
    if (target >= 0) {
        score += 250'000 + expected_piece_value(state.mask[target]) * 20;
        if (state.mask[target] == bit(Animal::Lion)) score += 1'000'000;
    }

    State after = state;
    Undo undo;
    if (!apply_move(after, move, undo)) return -2'000'000;
    if (after.terminal == Terminal::Catch || after.terminal == Terminal::Try) {
        return 1'500'000 + score;
    }
    const int before_lions = lion_candidate_count(state, after.side_to_move);
    const int after_lions = lion_candidate_count(after, after.side_to_move);
    score += (before_lions - after_lions) * 70'000;
    score += (popcount(state.mask[move.piece]) - popcount(after.mask[move.piece])) * 12'000;
    if (side_has_immediate_win(state, opposite(state.side_to_move)) &&
        !side_has_immediate_win(after, after.side_to_move)) {
        score += 400'000;
    }
    if (after.pos[move.piece] < board_size &&
        contains(after.mask[move.piece], Animal::Lion)) {
        score += progress_to_try(after.pos[move.piece], state.side_to_move) * 4000;
        if (after.mask[move.piece] == bit(Animal::Lion) &&
            is_square_attacked(after, after.pos[move.piece], after.side_to_move)) {
            score -= 180'000;
        }
    }
    score -= evaluate(after) / 4;
    return score;
}

void AlphaBetaEngine::order_moves(const State& state, std::vector<Move>& moves,
                                  const Move& tt_move, int ply) const {
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(moves.size());
    for (const Move& move : moves) {
        scored.emplace_back(move_order_score(state, move, tt_move, ply), move);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    for (std::size_t index = 0; index < moves.size(); ++index) moves[index] = scored[index].second;
}

std::vector<Move> AlphaBetaEngine::generate_search_moves(const State& state,
                                                         const Move& tt_move) {
    auto moves = generate_legal_moves(state);
    ++stats_.expanded_nodes;
    stats_.generated_legal_moves += moves.size();
    if (options_.successor_reducer != nullptr) {
        const auto begin = std::chrono::steady_clock::now();
        bool applied = false;
        moves = options_.successor_reducer(state, moves, tt_move,
                                           options_.reducer_threshold, applied);
        const auto end = std::chrono::steady_clock::now();
        if (applied) {
            stats_.leq_grouping_ms +=
                std::chrono::duration<double, std::milli>(end - begin).count();
            ++stats_.leq_grouped_nodes;
        }
    }
    stats_.equivalent_successor_moves += moves.size();
    return moves;
}

int AlphaBetaEngine::negamax(State& state, int depth, int alpha, int beta, int ply) {
    ++stats_.searched_nodes;
    if (should_stop()) return 0;
    if (state.terminal != Terminal::None) return terminal_score(state, ply);
    if (depth == 0) return evaluate(state);

    const int original_alpha = alpha;
    Move tt_move;
    if (options_.use_tt) {
        ++stats_.tt_probes;
        const TTEntry* entry = probe(state.hash);
        if (entry->key == state.hash) {
            ++stats_.tt_hits;
            tt_move = entry->best_move;
            if (entry->depth >= depth) {
                if (entry->bound == BoundType::Exact) return entry->score;
                if (entry->bound == BoundType::Lower) alpha = std::max(alpha, entry->score);
                if (entry->bound == BoundType::Upper) beta = std::min(beta, entry->score);
                if (alpha >= beta) return entry->score;
            }
        }
    }

    auto moves = generate_search_moves(state, tt_move);
    if (moves.empty()) return -mate_score + ply;
    order_moves(state, moves, tt_move, ply);

    int best = -search_infinity;
    Move best_move;
    for (const Move& move : moves) {
        Undo undo;
        if (!apply_move(state, move, undo)) continue;
        const int score = -negamax(state, depth - 1, -beta, -alpha, ply + 1);
        undo_move(state, undo);
        if (stopped_) return 0;
        if (score > best) {
            best = score;
            best_move = move;
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            ++stats_.cutoffs;
            if (state.board[move.to] == -1 && ply < static_cast<int>(killers_.size())) {
                if (killers_[ply][0] != move) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = move;
                }
                int& history = history_[move.piece][move.to];
                history = std::min(500'000, history + depth * depth * 64);
            }
            break;
        }
    }

    BoundType bound = BoundType::Exact;
    if (best <= original_alpha) bound = BoundType::Upper;
    else if (best >= beta) bound = BoundType::Lower;
    store(state.hash, depth, best, bound, best_move);
    return best;
}

bool AlphaBetaEngine::search_root(State& state, int depth, Move& best_move, int& best_score) {
    Move tt_move;
    if (options_.use_tt) {
        ++stats_.tt_probes;
        const TTEntry* entry = probe(state.hash);
        if (entry->key == state.hash) {
            ++stats_.tt_hits;
            tt_move = entry->best_move;
        }
    }
    auto moves = generate_search_moves(state, tt_move);
    if (moves.empty()) return true;
    order_moves(state, moves, tt_move, 0);
    int alpha = -search_infinity;
    const int beta = search_infinity;
    int best = -search_infinity;
    Move local_best = moves.front();
    for (const Move& move : moves) {
        if (should_stop()) return false;
        Undo undo;
        if (!apply_move(state, move, undo)) continue;
        const int score = -negamax(state, depth - 1, -beta, -alpha, 1);
        undo_move(state, undo);
        if (stopped_) return false;
        if (score > best) {
            best = score;
            local_best = move;
        }
        alpha = std::max(alpha, score);
    }
    best_move = local_best;
    best_score = best;
    store(state.hash, depth, best, BoundType::Exact, best_move);
    return true;
}

SearchResult AlphaBetaEngine::find_best_move(const State& root, const SearchOptions& options) {
    options_ = options;
    stats_ = {};
    stopped_ = false;
    ++age_;
    killers_ = {};
    for (auto& piece : history_) {
        for (int& value : piece) value /= 2;
    }
    start_ = std::chrono::steady_clock::now();
    deadline_ = start_ + std::chrono::milliseconds(std::max(0, options.time_limit_ms));

    SearchResult result;
    const auto fallback = generate_legal_moves(root);
    if (fallback.empty()) {
        result.score = root.terminal == Terminal::None ? -mate_score : terminal_score(root, 0);
        result.stats = stats_;
        return result;
    }
    result.best_move = fallback.front();
    result.has_move = true;
    result.score = evaluate(root);

    State state = root;
    for (int depth = 1; depth <= std::max(1, options.max_depth); ++depth) {
        Move depth_move;
        int depth_score = 0;
        const std::uint64_t nodes_before = stats_.searched_nodes;
        const auto depth_start = std::chrono::steady_clock::now();
        if (!search_root(state, depth, depth_move, depth_score)) break;
        const auto depth_end = std::chrono::steady_clock::now();
        result.best_move = depth_move;
        result.score = depth_score;
        stats_.depth_reached = depth;
        stats_.completed_depths.push_back(DepthReport{
            depth, depth_score, depth_move, stats_.searched_nodes - nodes_before,
            std::chrono::duration<double, std::milli>(depth_end - depth_start).count()});
        if (std::abs(depth_score) >= mate_score - 256) break;
    }
    const auto end = std::chrono::steady_clock::now();
    stats_.elapsed_ms = std::chrono::duration<double, std::milli>(end - start_).count();
    result.stats = stats_;
    return result;
}

SearchResult AlphaBetaEngine::search_fixed_depth(const State& root, int depth, bool use_tt) {
    SearchOptions options;
    options.max_depth = std::max(1, depth);
    options.time_limit_ms = 60'000;
    options.use_tt = use_tt;
    return find_best_move(root, options);
}

}  // namespace qas

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

std::uint16_t pack_move(const Move& move) {
    if (!move.valid()) return 0xFFFFU;
    return static_cast<std::uint16_t>(move.to | (move.from << 4U) | (move.piece << 9U));
}

Move unpack_move(std::uint16_t packed) {
    if (packed == 0xFFFFU) return {};
    return Move{static_cast<std::uint8_t>((packed >> 9U) & 7U),
                static_cast<std::uint8_t>((packed >> 4U) & 31U),
                static_cast<std::uint8_t>(packed & 15U)};
}

int score_to_tt(int score, int ply) {
    if (score > mate_score - 512) return score + ply;
    if (score < -mate_score + 512) return score - ply;
    return score;
}

int score_from_tt(int score, int ply) {
    if (score > mate_score - 512) return score - ply;
    if (score < -mate_score + 512) return score + ply;
    return score;
}

}  // namespace

AlphaBetaEngine::AlphaBetaEngine(std::size_t table_entries) {
    if (table_entries == 0 || (table_entries & (table_entries - 1)) != 0) {
        throw std::invalid_argument("transposition table size must be a power of two");
    }
    table_.resize(table_entries);
    table_mask_ = table_entries - 1;
    for (std::size_t ply = 0; ply < max_search_ply; ++ply) {
        move_pool_[ply].reserve(128);
        candidate_pool_[ply].reserve(128);
        score_pool_[ply].reserve(128);
    }
}

void AlphaBetaEngine::clear_tt() {
    std::fill(table_.begin(), table_.end(), TTEntry{});
}

TTEntry* AlphaBetaEngine::probe(std::uint64_t key) { return &table_[key & table_mask_]; }
const TTEntry* AlphaBetaEngine::probe(std::uint64_t key) const {
    return &table_[key & table_mask_];
}

void AlphaBetaEngine::store(std::uint64_t key, int depth, int score, BoundType bound,
                            const Move& move, int ply) {
    if (!options_.use_tt) return;
    TTEntry& entry = *probe(key);
    const BoundType old_bound = static_cast<BoundType>(entry.flag);
    if (entry.key != 0 && entry.key != key && entry.age == age_ && entry.depth > depth) return;
    if (entry.key == key && entry.depth > depth && old_bound == BoundType::Exact) return;
    entry.key = key;
    entry.depth = static_cast<std::int16_t>(depth);
    entry.score = score_to_tt(score, ply);
    entry.flag = static_cast<std::uint8_t>(bound);
    entry.move16 = pack_move(move);
    entry.age = age_;
    entry.generation = age_;
}

bool AlphaBetaEngine::should_stop() {
    if (stopped_) return true;
    if (options_.stop_policy != nullptr &&
        options_.stop_policy(stats_.searched_nodes, options_.stop_context)) {
        stopped_ = true;
        return true;
    }
    if ((stats_.searched_nodes & 63U) == 0U &&
        std::chrono::steady_clock::now() >= hard_deadline_) {
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
    if (options_.tt_move_ordering_enabled && move == tt_move) return 2'000'000;
    int score = 0;
    if (options_.killer_enabled && ply < static_cast<int>(killers_.size())) {
        if (move == killers_[ply][0]) score += 180'000;
        else if (move == killers_[ply][1]) score += 160'000;
    }
    if (options_.history_enabled) {
        score += history_[side_index(state.side_to_move)][move.from][move.to];
    }
    const int target = state.board[move.to];
    if (target >= 0 && options_.capture_ordering_enabled) {
        score += 250'000 + expected_piece_value(state.mask[target]) * 20;
        if (state.mask[target] == bit(Animal::Lion)) score += 1'000'000;
    }

    const Mask move_mask = move.from < board_size
        ? move_tables().move_possible_mask[side_index(state.side_to_move)][move.from][move.to]
        : state.mask[move.piece];
    if (options_.mask_collapse_ordering_enabled) {
        score += (popcount(state.mask[move.piece]) -
                  popcount(static_cast<Mask>(state.mask[move.piece] & move_mask))) * 12'000;
    }
    if (options_.try_threat_ordering_enabled && contains(state.mask[move.piece], Animal::Lion) &&
        move.to / board_width == (state.side_to_move == Side::South ? 0 : board_height - 1)) {
        score += 30'000;
    }
    if (!options_.strong_ordering_enabled || ply > 0) return score;

    State after = state;
    Undo undo;
    if (!apply_move(after, move, undo)) return -2'000'000;
    if (options_.immediate_win_ordering_enabled &&
        (after.terminal == Terminal::Catch || after.terminal == Terminal::Try)) {
        return 1'500'000 + score;
    }
    const int before_lions = lion_candidate_count(state, after.side_to_move);
    const int after_lions = lion_candidate_count(after, after.side_to_move);
    if (options_.lion_reduction_ordering_enabled)
        score += (before_lions - after_lions) * 70'000;
    if (options_.prevent_loss_ordering_enabled &&
        side_has_immediate_win(state, opposite(state.side_to_move)) &&
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
                                  const Move& tt_move, int ply) {
    auto& scored = score_pool_[std::min<std::size_t>(ply, max_search_ply - 1)];
    scored.clear();
    for (const Move& move : moves) {
        scored.emplace_back(move_order_score(state, move, tt_move, ply), move);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    for (std::size_t index = 0; index < moves.size(); ++index) moves[index] = scored[index].second;
}

void AlphaBetaEngine::generate_search_moves(const State& state, const Move& tt_move,
                                            int depth, int ply,
                                            std::vector<Move>& moves) {
    auto& scratch = candidate_pool_[std::min<std::size_t>(ply, max_search_ply - 1)];
    generate_legal_moves(state, moves, scratch);
    const std::size_t raw_move_count = moves.size();
    ++stats_.expanded_nodes;
    stats_.generated_legal_moves += moves.size();
    if (options_.successor_reducer != nullptr) {
        const auto begin = std::chrono::steady_clock::now();
        bool applied = false;
        const bool low_time = options_.reducer_disable_low_time &&
                              std::chrono::steady_clock::now() >= soft_deadline_;
        moves = options_.successor_reducer(state, moves, tt_move,
                                           options_.reducer_threshold,
                                           options_.reducer_min_depth, depth,
                                           options_.reducer_require_duplicate_hint,
                                           low_time, applied);
        const auto end = std::chrono::steady_clock::now();
        if (applied) {
            stats_.leq_grouping_ms +=
                std::chrono::duration<double, std::milli>(end - begin).count();
            ++stats_.leq_grouped_nodes;
            stats_.leq_skipped_moves += raw_move_count - moves.size();
        }
    }
    stats_.equivalent_successor_moves += moves.size();
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
            tt_move = unpack_move(entry->move16);
            if (entry->depth >= depth) {
                const int tt_score = score_from_tt(entry->score, ply);
                const BoundType bound = static_cast<BoundType>(entry->flag);
                if (bound == BoundType::Exact) { ++stats_.tt_exact_hits; return tt_score; }
                if (bound == BoundType::Lower) { ++stats_.tt_lower_hits; alpha = std::max(alpha, tt_score); }
                if (bound == BoundType::Upper) { ++stats_.tt_upper_hits; beta = std::min(beta, tt_score); }
                if (alpha >= beta) return tt_score;
            }
        }
    }

    auto& moves = move_pool_[std::min<std::size_t>(ply, max_search_ply - 1)];
    generate_search_moves(state, tt_move, depth, ply, moves);
    if (moves.empty()) return -mate_score + ply;
    order_moves(state, moves, tt_move, ply);
    if (tt_move.valid() && std::find(moves.begin(), moves.end(), tt_move) != moves.end()) {
        ++stats_.tt_move_used;
    }

    int best = -search_infinity;
    Move best_move;
    bool first = true;
    int move_index = 0;
    for (const Move& move : moves) {
        Undo undo;
        if (!apply_move(state, move, undo)) continue;
        int score = 0;
        if (first || !options_.pvs_enabled) {
            score = -negamax(state, depth - 1, -beta, -alpha, ply + 1);
        } else {
            score = -negamax(state, depth - 1, -alpha - 1, -alpha, ply + 1);
            if (!stopped_ && score > alpha && score < beta) {
                ++stats_.pvs_researches;
                score = -negamax(state, depth - 1, -beta, -alpha, ply + 1);
            }
        }
        undo_move(state, undo);
        if (stopped_) return 0;
        if (score > best) {
            best = score;
            best_move = move;
        }
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            ++stats_.cutoffs;
            stats_.cutoff_rank_sum += static_cast<std::uint64_t>(move_index + 1);
            if (move_index == 0) ++stats_.first_move_cutoffs;
            if (move == tt_move) ++stats_.tt_move_cutoffs;
            const bool quiet = state.board[move.to] == -1;
            if (quiet && options_.killer_enabled && ply < static_cast<int>(killers_.size())) {
                if (killers_[ply][0] != move) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = move;
                }
                ++stats_.killer_cutoffs;
            }
            if (quiet && options_.history_enabled) {
                int& history = history_[side_index(state.side_to_move)][move.from][move.to];
                history = std::min(500'000, history + depth * depth * 64);
                ++history_updates_;
                ++stats_.history_cutoffs;
                if (options_.history_decay_interval > 0 &&
                    history_updates_ % options_.history_decay_interval == 0) {
                    for (auto& side : history_) for (auto& from : side)
                        for (int& value : from) value /= 2;
                }
            }
            break;
        }
        first = false;
        ++move_index;
    }

    BoundType bound = BoundType::Exact;
    if (best <= original_alpha) bound = BoundType::Upper;
    else if (best >= beta) bound = BoundType::Lower;
    store(state.hash, depth, best, bound, best_move, ply);
    return best;
}

bool AlphaBetaEngine::search_root(State& state, int depth, int alpha, int beta,
                                  Move& best_move, int& best_score) {
    const int alpha_original = alpha;
    const int beta_original = beta;
    Move tt_move;
    if (options_.use_tt) {
        ++stats_.tt_probes;
        const TTEntry* entry = probe(state.hash);
        if (entry->key == state.hash) {
            ++stats_.tt_hits;
            tt_move = unpack_move(entry->move16);
        }
    }
    auto& moves = move_pool_[0];
    generate_search_moves(state, tt_move, depth, 0, moves);
    if (moves.empty()) return true;
    order_moves(state, moves, tt_move, 0);
    int best = -search_infinity;
    Move local_best = moves.front();
    bool first = true;
    for (const Move& move : moves) {
        if (should_stop()) return false;
        Undo undo;
        if (!apply_move(state, move, undo)) continue;
        int score = 0;
        if (first || !options_.pvs_enabled) {
            score = -negamax(state, depth - 1, -beta, -alpha, 1);
        } else {
            score = -negamax(state, depth - 1, -alpha - 1, -alpha, 1);
            if (!stopped_ && score > alpha && score < beta) {
                ++stats_.pvs_researches;
                score = -negamax(state, depth - 1, -beta, -alpha, 1);
            }
        }
        undo_move(state, undo);
        if (stopped_) return false;
        if (score > best) {
            best = score;
            local_best = move;
        }
        alpha = std::max(alpha, score);
        first = false;
        if (alpha >= beta) break;
    }
    best_move = local_best;
    best_score = best;
    const BoundType bound = best <= alpha_original ? BoundType::Upper
                           : best >= beta_original ? BoundType::Lower
                                                   : BoundType::Exact;
    store(state.hash, depth, best, bound, best_move, 0);
    return true;
}

SearchResult AlphaBetaEngine::find_best_move(const State& root, const SearchOptions& options) {
    options_ = options;
    stats_ = {};
    stopped_ = false;
    if (options_.tt_clear_each_move) clear_tt();
    if (options_.tt_age_enabled) ++age_;
    killers_ = {};
    for (auto& side : history_) for (auto& from : side)
        for (int& value : from) value /= 2;
    start_ = std::chrono::steady_clock::now();
    const int hard_ms = options.hard_time_limit_ms > 0
                            ? options.hard_time_limit_ms : options.time_limit_ms;
    const int soft_ms = options.soft_time_limit_ms > 0
                            ? options.soft_time_limit_ms : std::max(0, hard_ms * 9 / 10);
    soft_deadline_ = start_ + std::chrono::milliseconds(std::max(0, soft_ms));
    hard_deadline_ = start_ + std::chrono::milliseconds(std::max(0, hard_ms));

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
    const int first_depth = options.iterative_deepening_enabled ? 1 : std::max(1, options.max_depth);
    for (int depth = first_depth; depth <= std::max(1, options.max_depth); ++depth) {
        if (depth > first_depth && std::chrono::steady_clock::now() >= soft_deadline_) break;
        Move depth_move;
        int depth_score = 0;
        const std::uint64_t nodes_before = stats_.searched_nodes;
        const auto depth_start = std::chrono::steady_clock::now();
        int alpha = -search_infinity;
        int beta = search_infinity;
        int window = std::max(1, options.aspiration_initial_window);
        if (options.aspiration_enabled && depth > 1 && stats_.depth_reached > 0 &&
            std::abs(result.score) < mate_score - 512) {
            alpha = std::max(-search_infinity, result.score - window);
            beta = std::min(search_infinity, result.score + window);
        }
        int retries = 0;
        bool completed = false;
        while (!stopped_) {
            if (!search_root(state, depth, alpha, beta, depth_move, depth_score)) break;
            if (depth_score > alpha && depth_score < beta) { completed = true; break; }
            ++stats_.aspiration_retries;
            ++retries;
            if (retries > options.aspiration_max_retries ||
                std::chrono::steady_clock::now() >= soft_deadline_) {
                alpha = -search_infinity; beta = search_infinity;
            } else {
                window *= 2;
                if (depth_score <= alpha) alpha = std::max(-search_infinity, depth_score - window);
                if (depth_score >= beta) beta = std::min(search_infinity, depth_score + window);
            }
            if (alpha == -search_infinity && beta == search_infinity && retries > options.aspiration_max_retries + 1) break;
        }
        if (!completed || stopped_) break;
        const auto depth_end = std::chrono::steady_clock::now();
        result.best_move = depth_move;
        result.score = depth_score;
        stats_.depth_reached = depth;
        stats_.completed_depths.push_back(DepthReport{
            depth, depth_score, depth_move, stats_.searched_nodes - nodes_before,
            std::chrono::duration<double, std::milli>(depth_end - depth_start).count()});
        if (std::abs(depth_score) >= mate_score - 256) break;
        if (!options.iterative_deepening_enabled) break;
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
    options.soft_time_limit_ms = 60'000;
    options.hard_time_limit_ms = 60'000;
    options.use_tt = use_tt;
    return find_best_move(root, options);
}

}  // namespace qas

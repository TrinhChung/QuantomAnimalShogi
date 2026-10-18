#include "search/eval_features.hpp"

#include <algorithm>

namespace qas {
namespace {

constexpr std::uint16_t kBoardBits = (1U << board_size) - 1U;
constexpr int kNoCandidateEscapes = 4;

constexpr std::array<int, 32> build_expected_values() {
    constexpr std::array<int, 5> values{100, 320, 310, 1050, 420};
    std::array<int, 32> result{};
    for (unsigned mask = 1; mask < result.size(); ++mask) {
        int total = 0;
        int count = 0;
        for (unsigned form = 0; form < values.size(); ++form) {
            if ((mask & (1U << form)) != 0) {
                total += values[form];
                ++count;
            }
        }
        result[mask] = total / count;
    }
    return result;
}

inline constexpr auto kExpectedValues = build_expected_values();

int bits(unsigned value) {
    int count = 0;
    for (; value != 0; value &= value - 1)
        ++count;
    return count;
}

int try_progress(int square, int owner) {
    const int row = square / board_width;
    return owner == 0 ? board_height - 1 - row : row;
}

/// @brief Mirrors the legacy evaluation's immediate Catch/Try probe.
bool immediate_win(const State& state, Side side, std::vector<Move>& buffer) {
    State copy = state;
    copy.side_to_move = side;
    copy.terminal = Terminal::None;
    copy.winner = kNoWinner;
    recompute_hash(copy);
    generate_pseudo_legal_moves(copy, buffer);
    const int back_rank = side == Side::South ? 0 : board_height - 1;
    for (const Move& move : buffer) {
        const int target = state.board[move.to];
        const bool can_catch = target >= 0 && contains(state.mask[target], Animal::Lion);
        const bool can_try =
            contains(state.mask[move.piece], Animal::Lion) && move.to / board_width == back_rank;
        if (!can_catch && !can_try)
            continue;
        Undo undo;
        if (!apply_move(copy, move, undo))
            continue;
        const bool won = (copy.terminal == Terminal::Catch || copy.terminal == Terminal::Try) &&
                         copy.winner == side_index(side);
        if (won)
            return true;
        undo_move(copy, undo);
    }
    return false;
}

}  // namespace

const EvalWeights& legacy_eval_weights() {
    static const EvalWeights weights = [] {
        EvalWeights w{};
        w[kFeatMaterial] = 1;
        w[kFeatIdentities] = 12;
        w[kFeatMobility] = 18;
        w[kFeatUsSureLionAttacked] = -700;
        w[kFeatThemSureLionAttacked] = 700;
        w[kFeatUsLionCandAttacked] = -180;
        w[kFeatThemLionCandAttacked] = 180;
        w[kFeatSureLionTryProgress] = 90;
        w[kFeatCandTryProgress] = 25;
        w[kFeatSureLionInHand] = -900;
        w[kFeatLionCandidates] = 80;
        w[kFeatUsImmediateWin] = 3500;
        w[kFeatThemImmediateWin] = -3500;
        for (int& value : w)
            value *= kEvalWeightScale;
        return w;
    }();
    return weights;
}

EvalFeatures extract_eval_features(const State& state, std::vector<Move>& buffer) {
    const auto& tables = move_tables();
    const int us = side_index(state.side_to_move);
    std::array<std::array<int, kEvalFeatureCount>, 2> side{};  // per-owner raw terms
    std::array<std::uint16_t, 2> occupancy{};
    std::array<std::uint16_t, 2> attacks{};
    std::array<int, 2> hand{};
    int empty_count = 0;
    std::uint16_t empty = 0;
    for (int square = 0; square < board_size; ++square) {
        const int piece = state.board[square];
        if (piece < 0) {
            ++empty_count;
            empty |= static_cast<std::uint16_t>(1U << square);
        } else {
            occupancy[(state.owner_bits >> piece) & 1U] |= static_cast<std::uint16_t>(1U << square);
        }
    }
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        const int owner = (state.owner_bits >> piece) & 1U;
        if (is_hand_position(state.pos[piece]))
            ++hand[owner];
        else
            attacks[owner] |= tables.quantum_move_table[state.mask[piece]][owner][state.pos[piece]];
    }

    std::array<int, 2> min_escapes{9, 9};
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        const int owner = (state.owner_bits >> piece) & 1U;
        const int enemy = 1 - owner;
        auto& f = side[owner];
        const Mask mask = state.mask[piece];
        const bool lion = contains(mask, Animal::Lion);
        const bool sure_lion = mask == bit(Animal::Lion);
        f[kFeatMaterial] += kExpectedValues[mask];
        f[kFeatIdentities] += bits(mask) - 1;
        if (lion)
            ++f[kFeatLionCandidates];
        if (is_hand_position(state.pos[piece])) {
            f[kFeatMobility] += empty_count;
            if (sure_lion)
                ++f[kFeatSureLionInHand];
            continue;
        }
        const int square = state.pos[piece];
        const std::uint16_t square_bit = static_cast<std::uint16_t>(1U << square);
        f[kFeatMobility] += bits(tables.quantum_move_table[mask][owner][square] &
                                 static_cast<std::uint16_t>(~occupancy[owner]) & kBoardBits);
        const bool attacked = (attacks[enemy] & square_bit) != 0;
        const bool defended = (attacks[owner] & square_bit) != 0;
        if (!lion) {
            if (attacked && !defended)
                ++f[kFeatUsHanging];
            continue;
        }
        if (attacked)
            ++f[sure_lion ? kFeatUsSureLionAttacked : kFeatUsLionCandAttacked];
        f[sure_lion ? kFeatSureLionTryProgress : kFeatCandTryProgress] +=
            try_progress(square, owner);
        const std::uint16_t zone = tables.move_table[static_cast<int>(Animal::Lion)][owner][square];
        min_escapes[owner] =
            std::min(min_escapes[owner],
                     bits(zone & static_cast<std::uint16_t>(~occupancy[owner] & ~attacks[enemy]) &
                          kBoardBits));
        if (defended)
            ++f[kFeatUsLionDefended];
        int pressure = 0;
        for (int other = 0; other < physical_piece_count; ++other) {
            if (((state.owner_bits >> other) & 1U) != static_cast<unsigned>(enemy) ||
                is_hand_position(state.pos[other]))
                continue;
            if (tables.quantum_move_table[state.mask[other]][enemy][state.pos[other]] &
                (zone | square_bit))
                ++pressure;
        }
        f[kFeatUsLionPressure] = std::max(f[kFeatUsLionPressure], pressure);
        if (hand[enemy] > 0)
            f[kFeatUsDropThreat] = std::max(f[kFeatUsDropThreat], bits(zone & empty));
    }
    for (int owner = 0; owner < 2; ++owner) {
        side[owner][kFeatUsLionEscapes] =
            min_escapes[owner] == 9 ? kNoCandidateEscapes : min_escapes[owner];
        side[owner][kFeatHandPieces] = hand[owner];
    }

    const auto& mine = side[us];
    const auto& theirs = side[1 - us];
    EvalFeatures out{};
    for (int term : {kFeatMaterial,
                     kFeatIdentities,
                     kFeatMobility,
                     kFeatSureLionTryProgress,
                     kFeatCandTryProgress,
                     kFeatSureLionInHand,
                     kFeatLionCandidates,
                     kFeatHandPieces})
        out[term] = mine[term] - theirs[term];
    // Per-side terms are accumulated in the "Us" slot and redistributed to Us/Them here.
    const std::array<std::pair<int, int>, 6> paired{
        {{kFeatUsSureLionAttacked, kFeatThemSureLionAttacked},
         {kFeatUsLionCandAttacked, kFeatThemLionCandAttacked},
         {kFeatUsLionEscapes, kFeatThemLionEscapes},
         {kFeatUsLionDefended, kFeatThemLionDefended},
         {kFeatUsLionPressure, kFeatThemLionPressure},
         {kFeatUsDropThreat, kFeatThemDropThreat}}};
    for (const auto& [own, other] : paired) {
        out[own] = mine[own];
        out[other] = theirs[own];
    }
    out[kFeatUsHanging] = mine[kFeatUsHanging];
    out[kFeatThemHanging] = theirs[kFeatUsHanging];
    out[kFeatUsImmediateWin] = immediate_win(state, state.side_to_move, buffer) ? 1 : 0;
    out[kFeatThemImmediateWin] = immediate_win(state, opposite(state.side_to_move), buffer) ? 1 : 0;
    out[kFeatTempo] = 1;
    return out;
}

int weighted_eval(const EvalFeatures& features, const EvalWeights& weights) {
    long long total = 0;
    for (int i = 0; i < kEvalFeatureCount; ++i)
        total += static_cast<long long>(features[i]) * weights[i];
    return static_cast<int>(total / kEvalWeightScale);
}

}  // namespace qas

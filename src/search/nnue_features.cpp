#include "search/nnue_features.hpp"

namespace qas {

NnueActive nnue_active_features(const State& state) {
    const Side us = state.side_to_move;
    NnueActive active{};
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        const std::uint8_t position = state.pos[piece];
        int location = board_size;  // hand
        if (position < board_size)
            location = us == Side::South ? position : board_size - 1 - position;
        const int theirs = owned_by(state, piece, us) ? 0 : 1;
        const int their_origin = originated_from(state, piece, us) ? 0 : 1;
        active[piece] = ((location * 2 + theirs) * 2 + their_origin) * 32 + state.mask[piece];
    }
    return active;
}

}  // namespace qas

#include "rules/game.hpp"

#include <array>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace qas {

State parse_state(std::istream& input) {
    std::string side_text;
    int turn = 0;
    if (!(input >> side_text >> turn) || turn < 0 || turn > 256) {
        throw std::invalid_argument("state header must be: <S|N> <turn 0..256>");
    }
    State state;
    state.board.fill(-1);
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        state.pos[piece] = static_cast<std::uint8_t>(first_hand_slot + piece);
    }
    state.mask.fill(0);
    state.side_to_move = (side_text == "N" || side_text == "n") ? Side::North : Side::South;
    if (side_text != "S" && side_text != "s" && side_text != "N" && side_text != "n") {
        throw std::invalid_argument("side must be S or N");
    }
    state.turn = static_cast<std::uint16_t>(turn);
    std::array<bool, physical_piece_count> seen{};
    for (int line = 0; line < physical_piece_count; ++line) {
        int id = -1;
        std::string owner;
        std::string origin;
        std::string position;
        std::string mask_text;
        if (!(input >> id >> owner >> origin >> position >> mask_text) || id < 0 ||
            id >= physical_piece_count || seen[id]) {
            throw std::invalid_argument("piece line must be: id owner origin position mask");
        }
        seen[id] = true;
        if (owner == "N" || owner == "n") state.owner_bits |= static_cast<std::uint8_t>(1U << id);
        else if (owner != "S" && owner != "s") throw std::invalid_argument("invalid owner");
        if (origin == "N" || origin == "n") state.origin_bits |= static_cast<std::uint8_t>(1U << id);
        else if (origin != "S" && origin != "s") throw std::invalid_argument("invalid origin");
        if (!position.empty() && (position[0] == 'H' || position[0] == 'h')) {
            const int slot = position.size() == 1 ? id : std::stoi(position.substr(1));
            if (slot < 0 || slot >= physical_piece_count) {
                throw std::invalid_argument("invalid hand slot");
            }
            state.pos[id] = static_cast<std::uint8_t>(first_hand_slot + slot);
        } else {
            const int square = std::stoi(position);
            if (square < 0 || square >= board_size || state.board[square] != -1) {
                throw std::invalid_argument("invalid or occupied square");
            }
            state.pos[id] = static_cast<std::uint8_t>(square);
            state.board[square] = static_cast<std::int8_t>(id);
        }
        Mask mask = 0;
        for (char code : mask_text) mask |= bit(parse_animal(std::string(1, code)));
        state.mask[id] = mask;
    }
    if (!propagate(state)) throw std::invalid_argument("state has contradictory masks");
    if (state.turn >= 256) state.terminal = Terminal::Draw;
    recompute_hash(state);
    std::string error;
    if (!validate_state(state, &error)) throw std::invalid_argument(error);
    return state;
}

void write_state(const State& state, std::ostream& output) {
    output << (state.side_to_move == Side::South ? 'S' : 'N') << ' ' << state.turn << '\n';
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        output << piece << ' ' << (owned_by(state, piece, Side::South) ? 'S' : 'N') << ' '
               << (originated_from(state, piece, Side::South) ? 'S' : 'N') << ' ';
        if (is_hand_position(state.pos[piece])) {
            output << 'H' << static_cast<int>(state.pos[piece] - first_hand_slot);
        } else {
            output << static_cast<int>(state.pos[piece]);
        }
        output << ' ' << mask_string(state.mask[piece]) << '\n';
    }
}

std::string move_string(const Move& move) {
    if (!move.valid()) return "invalid";
    std::ostringstream output;
    output << 'p' << static_cast<int>(move.piece) << ':';
    if (is_hand_position(move.from)) output << "hand" << static_cast<int>(move.from - first_hand_slot);
    else output << static_cast<int>(move.from);
    output << "->" << static_cast<int>(move.to);
    return output.str();
}

std::string terminal_name(Terminal terminal) {
    switch (terminal) {
        case Terminal::None: return "none";
        case Terminal::Catch: return "catch";
        case Terminal::Try: return "try";
        case Terminal::Draw: return "draw";
        case Terminal::Illegal: return "illegal";
    }
    return "unknown";
}

bool same_state(const State& left, const State& right) {
    return left.board == right.board && left.pos == right.pos && left.mask == right.mask &&
           left.owner_bits == right.owner_bits && left.origin_bits == right.origin_bits &&
           left.side_to_move == right.side_to_move && left.turn == right.turn &&
           left.terminal == right.terminal && left.winner == right.winner &&
           left.hash == right.hash;
}

}  // namespace qas

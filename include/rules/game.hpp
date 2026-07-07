#pragma once

#include "core/animal.hpp"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace qas {

inline constexpr int board_width = 3;
inline constexpr int board_height = 4;
inline constexpr int board_size = board_width * board_height;
inline constexpr int physical_piece_count = 8;
inline constexpr int external_source_count = 20;
inline constexpr std::uint8_t first_hand_slot = board_size;
inline constexpr std::uint8_t hand_square = first_hand_slot;
inline constexpr int external_action_count = 240;

enum class Terminal : std::uint8_t { None, Catch, Try, Draw, Illegal };

struct Move {
    std::uint8_t piece{255};
    std::uint8_t from{first_hand_slot};
    std::uint8_t to{255};

    bool operator==(const Move& other) const {
        return piece == other.piece && from == other.from && to == other.to;
    }
    bool operator!=(const Move& other) const { return !(*this == other); }
    bool valid() const {
        return piece < physical_piece_count && from < external_source_count && to < board_size;
    }
};

struct State {
    std::array<std::int8_t, board_size> board{};
    std::array<std::uint8_t, physical_piece_count> pos{};
    std::array<Mask, physical_piece_count> mask{};
    std::uint8_t owner_bits{0};       // bit=1 means North
    std::uint8_t origin_bits{0};      // immutable lineage group; bit=1 means North
    Side side_to_move{Side::South};
    std::uint16_t turn{0};
    Terminal terminal{Terminal::None};
    std::int8_t winner{-1};           // 0 South, 1 North, -1 none
    std::uint64_t hash{0};
};

struct Undo {
    State previous{};
};

struct MoveTables {
    // [form][side][square] -> destination bitboard
    std::array<std::array<std::array<std::uint16_t, board_size>, 2>, 5> move_table{};
    // [mask][side][square] -> union destination bitboard
    std::array<std::array<std::array<std::uint16_t, board_size>, 2>, 32>
        quantum_move_table{};
    // [side][source][destination] -> forms capable of the move
    std::array<std::array<std::array<Mask, board_size>, board_size>, 2>
        move_possible_mask{};
};

const MoveTables& move_tables();

State initial_state();
bool validate_state(const State& state, std::string* error = nullptr);
bool propagate(State& state);
void recompute_hash(State& state);
std::uint64_t zobrist_hash(const State& state);

bool apply_move(State& state, const Move& move, Undo& undo);
void undo_move(State& state, const Undo& undo);

std::vector<Move> generate_pseudo_legal_moves(const State& state);
std::vector<Move> generate_legal_moves(const State& state);
void generate_pseudo_legal_moves(const State& state, std::vector<Move>& output);
void generate_legal_moves(const State& state, std::vector<Move>& output,
                          std::vector<Move>& scratch);
bool is_square_attacked(const State& state, int square, Side by_side);
bool is_immediate_winning_move(const State& state, const Move& move);

State parse_state(std::istream& input);
void write_state(const State& state, std::ostream& output);
std::string move_string(const Move& move);
std::string terminal_name(Terminal terminal);

bool same_state(const State& left, const State& right);
Side opposite(Side side);
int side_index(Side side);
bool owned_by(const State& state, int piece, Side side);
bool originated_from(const State& state, int piece, Side side);
bool is_hand_position(std::uint8_t position);

}  // namespace qas

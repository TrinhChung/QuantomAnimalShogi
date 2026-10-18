#include "book/position_book.hpp"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "exact/equivalence.hpp"

namespace qas {
namespace {

/// @brief Appends a value as fixed-width lowercase hexadecimal.
void append_hex(std::ostringstream& output, unsigned value, int width) {
    output << std::hex << std::setw(width) << std::setfill('0') << value;
}

/// @brief Validates the textual move format without resolving it.
bool well_formed_move(const std::string& text) {
    if (text.size() < 4)
        return false;
    const auto dash = text.find('-');
    if (dash == std::string::npos || dash + 1 >= text.size())
        return false;
    if (text[0] == 'b')
        return dash > 1;
    if (text[0] == 'h')
        return dash > 2 && (text[1] == 'S' || text[1] == 'N');
    return false;
}

}  // namespace

std::string PositionBook::key(const State& state) {
    const CanonKey canonical = canonical_key(state);
    std::ostringstream output;
    for (const std::uint8_t value : canonical.board)
        append_hex(output, value, 2);
    output << '.';
    for (const std::uint16_t value : canonical.pieces)
        append_hex(output, value, 4);
    output << '.' << (canonical.side_to_move == Side::South ? 'S' : 'N') << '.' << std::dec
           << canonical.turn << '.' << static_cast<int>(canonical.terminal);
    return output.str();
}

std::string PositionBook::encode_move(const State& state, const Move& move) {
    std::ostringstream output;
    if (is_hand_position(move.from)) {
        output << 'h' << (originated_from(state, move.piece, Side::North) ? 'N' : 'S')
               << mask_string(state.mask[move.piece]);
    } else {
        output << 'b' << static_cast<int>(move.from);
    }
    output << '-' << static_cast<int>(move.to);
    return output.str();
}

bool PositionBook::add(const std::string& key, const std::string& move) {
    if (key.empty() || !well_formed_move(move))
        return false;
    entries_[key] = move;
    return true;
}

bool PositionBook::load(const std::string& path, std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr)
            *error = "cannot open book file: " + path;
        return false;
    }
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        std::istringstream fields(line);
        std::string key;
        std::string move;
        if (!(fields >> key))
            continue;
        if (!(fields >> move) || !add(key, move)) {
            if (error != nullptr && error->empty())
                *error = "malformed book line " + std::to_string(line_number);
        }
    }
    return true;
}

std::optional<Move> PositionBook::lookup(const State& state) const {
    if (entries_.empty() || state.terminal != Terminal::None)
        return std::nullopt;
    const auto found = entries_.find(key(state));
    if (found == entries_.end())
        return std::nullopt;
    for (const Move& move : generate_legal_moves(state)) {
        if (encode_move(state, move) == found->second)
            return move;
    }
    return std::nullopt;  // stale or illegal entry: ignore it
}

}  // namespace qas

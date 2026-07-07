#include "io/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace qas {
namespace {

std::size_t find_matching_bracket(const std::string& text, std::size_t opening) {
    int depth = 0;
    for (std::size_t index = opening; index < text.size(); ++index) {
        if (text[index] == '[') ++depth;
        else if (text[index] == ']' && --depth == 0) return index;
    }
    throw std::invalid_argument("unterminated JSON array");
}

std::vector<int> array_integers(const std::string& text, std::size_t key_position) {
    const std::size_t opening = text.find('[', key_position);
    if (opening == std::string::npos) throw std::invalid_argument("JSON array is missing");
    const std::size_t closing = find_matching_bracket(text, opening);
    std::vector<int> values;
    for (std::size_t index = opening + 1; index < closing;) {
        if (text[index] == '-' || std::isdigit(static_cast<unsigned char>(text[index]))) {
            bool negative = false;
            if (text[index] == '-') {
                negative = true;
                ++index;
            }
            int value = 0;
            while (index < closing &&
                   std::isdigit(static_cast<unsigned char>(text[index]))) {
                value = value * 10 + (text[index++] - '0');
            }
            values.push_back(negative ? -value : value);
        } else {
            ++index;
        }
    }
    return values;
}

int integer_after_key(const std::string& text, const std::string& key) {
    const std::size_t key_position = text.find(key);
    if (key_position == std::string::npos) throw std::invalid_argument("missing JSON key " + key);
    std::size_t index = text.find(':', key_position);
    if (index == std::string::npos) throw std::invalid_argument("missing JSON colon");
    ++index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    int value = 0;
    bool found = false;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        found = true;
        value = value * 10 + (text[index++] - '0');
    }
    if (!found) throw std::invalid_argument("JSON integer is missing");
    return value;
}

std::string command_value(const std::string& text) {
    const std::size_t key = text.find("\"command\"");
    if (key == std::string::npos) throw std::invalid_argument("missing command");
    const std::size_t colon = text.find(':', key);
    const std::size_t quote = text.find('"', colon + 1);
    const std::size_t end = text.find('"', quote + 1);
    if (colon == std::string::npos || quote == std::string::npos || end == std::string::npos) {
        throw std::invalid_argument("invalid command string");
    }
    return text.substr(quote + 1, end - quote - 1);
}

State state_from_observation(const std::vector<int>& entries, int turn) {
    if (entries.size() != 20U * 9U) {
        throw std::invalid_argument("observation must contain 20 arrays of 9 integers");
    }
    State state;
    state.board.fill(-1);
    state.mask.fill(0);
    state.side_to_move = Side::South;  // Protocol rotates the current player to the bottom.
    state.turn = static_cast<std::uint16_t>(turn);
    std::array<int, 2> origin_counts{};
    int occupied_count = 0;
    for (int source = 0; source < external_source_count; ++source) {
        const int* entry = entries.data() + source * 9;
        const bool occupied = std::any_of(entry, entry + 9, [](int value) { return value != 0; });
        if (!occupied) continue;
        for (int field = 0; field < 9; ++field) {
            if (entry[field] != 0 && entry[field] != 1) {
                throw std::invalid_argument("observation flags must be binary");
            }
        }
        if (entry[5] + entry[6] != 1 || entry[7] + entry[8] != 1) {
            throw std::invalid_argument("piece origin and owner flags must be one-hot");
        }
        const int origin = entry[6];
        if (origin_counts[origin] >= 4) throw std::invalid_argument("too many origin pieces");
        const int piece = origin * 4 + origin_counts[origin]++;
        Mask mask = 0;
        if (entry[0]) mask |= bit(Animal::Chick);
        if (entry[1]) mask |= bit(Animal::Giraffe);
        if (entry[2]) mask |= bit(Animal::Elephant);
        if (entry[3]) mask |= bit(Animal::Lion);
        if (entry[4]) mask |= bit(Animal::Hen);
        if (mask == 0) throw std::invalid_argument("occupied piece has empty identity mask");
        state.mask[piece] = mask;
        state.pos[piece] = static_cast<std::uint8_t>(source);
        if (entry[8]) state.owner_bits |= static_cast<std::uint8_t>(1U << piece);
        if (origin == 1) state.origin_bits |= static_cast<std::uint8_t>(1U << piece);
        if (source < board_size) state.board[source] = static_cast<std::int8_t>(piece);
        ++occupied_count;
    }
    if (occupied_count != physical_piece_count || origin_counts[0] != 4 ||
        origin_counts[1] != 4) {
        throw std::invalid_argument("observation must contain eight pieces, four per origin");
    }
    if (!propagate(state)) throw std::invalid_argument("observation masks contradict quotas");
    if (turn >= 256) state.terminal = Terminal::Draw;
    recompute_hash(state);
    return state;
}

}  // namespace

int encode_action_index(int source_index, int destination_index) {
    if (source_index < 0 || source_index >= external_source_count || destination_index < 0 ||
        destination_index >= board_size) {
        throw std::out_of_range("action coordinates are outside protocol range");
    }
    return source_index * board_size + destination_index;
}

std::pair<int, int> decode_action_index(int action_index) {
    if (action_index < 0 || action_index >= external_action_count) {
        throw std::out_of_range("action index is outside [0, 239]");
    }
    return {action_index / board_size, action_index % board_size};
}

Move decode_action_move(const State& state, int action_index) {
    const auto coordinates = decode_action_index(action_index);
    for (int piece = 0; piece < physical_piece_count; ++piece) {
        if (state.pos[piece] == coordinates.first) {
            return Move{static_cast<std::uint8_t>(piece),
                        static_cast<std::uint8_t>(coordinates.first),
                        static_cast<std::uint8_t>(coordinates.second)};
        }
    }
    return {};
}

int encode_action_move(const Move& move) {
    if (!move.valid()) return -1;
    return encode_action_index(move.from, move.to);
}

ProtocolMessage parse_protocol_message(const std::string& json_line) {
    ProtocolMessage message;
    const std::string command = command_value(json_line);
    if (command == "end_game") {
        message.command = ProtocolCommand::EndGame;
        return message;
    }
    if (command != "get_action") throw std::invalid_argument("unsupported command");
    message.command = ProtocolCommand::GetAction;

    const std::size_t first_observation = json_line.find("\"observation\"");
    const std::size_t piece_observation =
        json_line.find("\"observation\"", first_observation + 1);
    const std::size_t action_mask = json_line.find("\"action_mask\"");
    if (piece_observation == std::string::npos || action_mask == std::string::npos) {
        throw std::invalid_argument("get_action payload is incomplete");
    }
    const auto entries = array_integers(json_line, piece_observation);
    const auto actions = array_integers(json_line, action_mask);
    if (actions.size() != external_action_count) {
        throw std::invalid_argument("action_mask must contain 240 integers");
    }
    for (int action = 0; action < external_action_count; ++action) {
        if (actions[action] != 0 && actions[action] != 1) {
            throw std::invalid_argument("action_mask values must be binary");
        }
        message.action_mask[action] = static_cast<std::uint8_t>(actions[action]);
    }
    const int turn = integer_after_key(json_line, "\"turn\"");
    if (turn < 0 || turn > 256) throw std::invalid_argument("turn is outside [0, 256]");
    message.state = state_from_observation(entries, turn);
    return message;
}

bool action_is_allowed(const ProtocolMessage& message, int action_index) {
    return message.command == ProtocolCommand::GetAction && action_index >= 0 &&
           action_index < external_action_count && message.action_mask[action_index] != 0;
}

}  // namespace qas

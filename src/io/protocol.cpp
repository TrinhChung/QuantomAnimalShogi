#include "io/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace qas {
namespace {

constexpr std::size_t kOriginSouthField = kAnimalFormCount;
constexpr std::size_t kOriginNorthField = kOriginSouthField + 1;
constexpr std::size_t kOwnerSouthField = kOriginNorthField + 1;
constexpr std::size_t kOwnerNorthField = kOwnerSouthField + 1;

static_assert(kOwnerNorthField + 1 == kObservationFieldCount,
              "observation field layout must match its declared width");

/// @brief Tests whether two binary flags encode exactly one selected value.
/// @param first First binary flag.
/// @param second Second binary flag.
/// @return `true` when exactly one flag is set.
bool is_one_hot_pair(int first, int second) {
    return first + second == 1;
}

/// @brief Finds the closing bracket that balances a JSON array opening.
/// @param text JSON text to scan.
/// @param opening Index of the opening bracket.
/// @return Index of the matching closing bracket.
/// @throws std::invalid_argument If the array is unterminated.
std::size_t find_matching_bracket(const std::string& text, std::size_t opening) {
    int depth = 0;
    for (std::size_t index = opening; index < text.size(); ++index) {
        if (text[index] == '[')
            ++depth;
        else if (text[index] == ']' && --depth == 0)
            return index;
    }
    throw std::invalid_argument("unterminated JSON array");
}

/// @brief Extracts every integer from the JSON array following a key.
/// @param text JSON text to scan.
/// @param key_position Position of the property key.
/// @return Integers in source order, or an empty vector when no array follows.
std::vector<int> array_integers(const std::string& text, std::size_t key_position) {
    const std::size_t opening = text.find('[', key_position);
    if (opening == std::string::npos)
        throw std::invalid_argument("JSON array is missing");
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
            while (index < closing && std::isdigit(static_cast<unsigned char>(text[index]))) {
                value = value * 10 + (text[index++] - '0');
            }
            values.push_back(negative ? -value : value);
        } else {
            ++index;
        }
    }
    return values;
}

/// @brief Parses the first integer following a JSON key.
/// @param text JSON text to scan.
/// @param key Quoted property name.
/// @return Parsed integer, or zero when the key or value is absent.
int integer_after_key(const std::string& text, const std::string& key) {
    const std::size_t key_position = text.find(key);
    if (key_position == std::string::npos)
        throw std::invalid_argument("missing JSON key " + key);
    std::size_t index = text.find(':', key_position);
    if (index == std::string::npos)
        throw std::invalid_argument("missing JSON colon");
    ++index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])))
        ++index;
    int value = 0;
    bool found = false;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        found = true;
        value = value * 10 + (text[index++] - '0');
    }
    if (!found)
        throw std::invalid_argument("JSON integer is missing");
    return value;
}

/// @brief Extracts and validates the protocol command string.
/// @param text Complete protocol JSON line.
/// @return Command value.
/// @throws std::invalid_argument If the command property is missing or malformed.
std::string command_value(const std::string& text) {
    const std::size_t key = text.find("\"command\"");
    if (key == std::string::npos)
        throw std::invalid_argument("missing command");
    const std::size_t colon = text.find(':', key);
    const std::size_t quote = text.find('"', colon + 1);
    const std::size_t end = text.find('"', quote + 1);
    if (colon == std::string::npos || quote == std::string::npos || end == std::string::npos) {
        throw std::invalid_argument("invalid command string");
    }
    return text.substr(quote + 1, end - quote - 1);
}

/// @brief Reconstructs a native state from the flattened official observation.
/// @param entries Flattened 20-by-9 observation values.
/// @param turn Current turn count.
/// @return Validated native state with a current hash.
/// @throws std::invalid_argument If observation shape, flags or identities are invalid.
State state_from_observation(const std::vector<int>& entries, int turn) {
    if (entries.size() != kObservationValueCount) {
        throw std::invalid_argument("observation must contain 20 arrays of 9 integers");
    }
    State state;
    state.board.fill(kEmptyBoardSquare);
    state.mask.fill(0);
    state.side_to_move = Side::South;  // Protocol rotates the current player to the bottom.
    state.turn = static_cast<std::uint16_t>(turn);
    std::array<int, kSideCount> origin_counts{};
    int occupied_count = 0;
    for (int source = 0; source < external_source_count; ++source) {
        const int* entry = entries.data() + source * kObservationFieldCount;
        const bool occupied = std::any_of(
            entry, entry + kObservationFieldCount, [](int value) { return value != 0; });
        if (!occupied)
            continue;
        for (std::size_t field = 0; field < kObservationFieldCount; ++field) {
            if (entry[field] != 0 && entry[field] != 1) {
                throw std::invalid_argument("observation flags must be binary");
            }
        }
        const bool valid_origin =
            is_one_hot_pair(entry[kOriginSouthField], entry[kOriginNorthField]);
        const bool valid_owner = is_one_hot_pair(entry[kOwnerSouthField], entry[kOwnerNorthField]);
        if (!valid_origin || !valid_owner) {
            throw std::invalid_argument("piece origin and owner flags must be one-hot");
        }
        const int origin = entry[kOriginNorthField];
        if (origin_counts[origin] >= kPiecesPerOrigin)
            throw std::invalid_argument("too many origin pieces");
        const int piece = origin * kPiecesPerOrigin + origin_counts[origin]++;
        Mask mask = 0;
        for (Animal form : all_forms) {
            if (entry[static_cast<std::size_t>(form)] != 0)
                mask |= bit(form);
        }
        if (mask == 0)
            throw std::invalid_argument("occupied piece has empty identity mask");
        state.mask[piece] = mask;
        state.pos[piece] = static_cast<std::uint8_t>(source);
        if (entry[kOwnerNorthField])
            state.owner_bits |= static_cast<std::uint8_t>(1U << piece);
        if (origin == 1)
            state.origin_bits |= static_cast<std::uint8_t>(1U << piece);
        if (source < board_size)
            state.board[source] = static_cast<std::int8_t>(piece);
        ++occupied_count;
    }
    const bool origins_complete = std::all_of(origin_counts.begin(),
                                              origin_counts.end(),
                                              [](int count) { return count == kPiecesPerOrigin; });
    if (occupied_count != physical_piece_count || !origins_complete) {
        throw std::invalid_argument("observation must contain eight pieces, four per origin");
    }
    if (!propagate(state))
        throw std::invalid_argument("observation masks contradict quotas");
    if (turn >= kTurnLimit)
        state.terminal = Terminal::Draw;
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
    if (!move.valid())
        return -1;
    return encode_action_index(move.from, move.to);
}

ProtocolMessage parse_protocol_message(const std::string& json_line) {
    ProtocolMessage message;
    const std::string command = command_value(json_line);
    if (command == "end_game") {
        message.command = ProtocolCommand::EndGame;
        return message;
    }
    if (command != "get_action")
        throw std::invalid_argument("unsupported command");
    message.command = ProtocolCommand::GetAction;

    const std::size_t first_observation = json_line.find("\"observation\"");
    const std::size_t piece_observation = json_line.find("\"observation\"", first_observation + 1);
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
    if (turn < 0 || turn > kTurnLimit)
        throw std::invalid_argument("turn is outside [0, 256]");
    message.state = state_from_observation(entries, turn);
    return message;
}

bool action_is_allowed(const ProtocolMessage& message, int action_index) {
    return message.command == ProtocolCommand::GetAction && action_index >= 0 &&
           action_index < external_action_count && message.action_mask[action_index] != 0;
}

}  // namespace qas

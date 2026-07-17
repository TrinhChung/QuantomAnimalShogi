#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "io/protocol.hpp"
#include "json_output.hpp"
#include "rules/game.hpp"

namespace {

using qas::Animal;
using qas::Mask;
using qas::Move;
using qas::Side;
using qas::State;

int public_square(int square, Side perspective) {
    if (square >= qas::board_size || perspective == Side::South)
        return square;
    return qas::board_size - 1 - square;
}

int actual_square(int square, Side perspective) {
    return public_square(square, perspective);
}

int public_action(const Move& move, Side perspective) {
    return qas::encode_action_index(public_square(move.from, perspective),
                                    public_square(move.to, perspective));
}

Move actual_move(const State& state, int action) {
    const auto [public_source, public_destination] = qas::decode_action_index(action);
    const int source = actual_square(public_source, state.side_to_move);
    const int destination = actual_square(public_destination, state.side_to_move);
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        if (state.pos[piece] == source) {
            return Move{static_cast<std::uint8_t>(piece),
                        static_cast<std::uint8_t>(source),
                        static_cast<std::uint8_t>(destination)};
        }
    }
    return {};
}

std::vector<int> legal_actions(const State& state) {
    std::vector<int> actions;
    for (const Move& move : qas::generate_legal_moves(state))
        actions.push_back(public_action(move, state.side_to_move));
    std::sort(actions.begin(), actions.end());
    return actions;
}

int uncertainty_score(const State& state) {
    int score = 0;
    for (Mask mask : state.mask) {
        while (mask != 0) {
            ++score;
            mask = static_cast<Mask>(mask & (mask - 1));
        }
    }
    return score;
}

int hand_piece_count(const State& state) {
    return static_cast<int>(std::count_if(state.pos.begin(), state.pos.end(), [](std::uint8_t pos) {
        return qas::is_hand_position(pos);
    }));
}

int lion_candidate_count(const State& state, Side side) {
    int count = 0;
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        if (qas::originated_from(state, piece, side) &&
            qas::contains(state.mask[piece], Animal::Lion)) {
            ++count;
        }
    }
    return count;
}

std::string protocol_request(const State& state, const std::vector<int>& actions) {
    std::array<std::array<int, 9>, qas::external_source_count> observation{};
    const Side perspective = state.side_to_move;
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        const int source = public_square(state.pos[piece], perspective);
        auto& entry = observation[static_cast<std::size_t>(source)];
        entry[0] = qas::contains(state.mask[piece], Animal::Chick) ? 1 : 0;
        entry[1] = qas::contains(state.mask[piece], Animal::Giraffe) ? 1 : 0;
        entry[2] = qas::contains(state.mask[piece], Animal::Elephant) ? 1 : 0;
        entry[3] = qas::contains(state.mask[piece], Animal::Lion) ? 1 : 0;
        entry[4] = qas::contains(state.mask[piece], Animal::Hen) ? 1 : 0;
        const bool own_origin = qas::originated_from(state, piece, perspective);
        const bool own_piece = qas::owned_by(state, piece, perspective);
        entry[5] = own_origin ? 1 : 0;
        entry[6] = own_origin ? 0 : 1;
        entry[7] = own_piece ? 1 : 0;
        entry[8] = own_piece ? 0 : 1;
    }

    std::array<int, qas::external_action_count> mask{};
    for (const int action : actions)
        mask[static_cast<std::size_t>(action)] = 1;

    std::ostringstream output;
    output << "{\"command\":\"get_action\",\"observation\":{\"observation\":[";
    for (int source = 0; source < qas::external_source_count; ++source) {
        if (source != 0)
            output << ',';
        output << '[';
        for (int field = 0; field < 9; ++field) {
            if (field != 0)
                output << ',';
            output
                << observation[static_cast<std::size_t>(source)][static_cast<std::size_t>(field)];
        }
        output << ']';
    }
    output << "],\"action_mask\":[";
    for (int action = 0; action < qas::external_action_count; ++action) {
        if (action != 0)
            output << ',';
        output << mask[static_cast<std::size_t>(action)];
    }
    output << "],\"turn\":" << state.turn << "}}";
    return output.str();
}

std::string state_text(const State& state) {
    std::ostringstream output;
    qas::write_state(state, output);
    return output.str();
}

void write_snapshot(const State& state) {
    const std::vector<int> actions = legal_actions(state);
    std::string mask;
    mask.reserve(qas::external_action_count);
    std::size_t action_index = 0;
    for (int action = 0; action < qas::external_action_count; ++action) {
        const bool enabled = action_index < actions.size() && actions[action_index] == action;
        mask.push_back(enabled ? '1' : '0');
        if (enabled)
            ++action_index;
    }
    std::cout << "{\"ok\":true,\"state_text\":";
    qas::evaluation::write_json_string(std::cout, state_text(state));
    std::cout << ",\"state_hash\":" << state.hash << ",\"side\":\""
              << (state.side_to_move == Side::South ? "south" : "north")
              << "\",\"turn\":" << state.turn
              << ",\"remaining_horizon\":" << std::max(0, 256 - state.turn) << ",\"terminal\":\""
              << qas::terminal_name(state.terminal) << "\",\"winner\":";
    if (state.winner < 0)
        std::cout << "null";
    else
        std::cout << static_cast<int>(state.winner);
    std::cout << ",\"legal_actions\":[";
    for (std::size_t index = 0; index < actions.size(); ++index) {
        if (index != 0)
            std::cout << ',';
        std::cout << actions[index];
    }
    std::cout << "],\"action_mask\":\"" << mask << "\",\"legal_count\":" << actions.size()
              << ",\"uncertainty_score\":" << uncertainty_score(state)
              << ",\"hand_piece_count\":" << hand_piece_count(state)
              << ",\"lion_candidate_counts\":[" << lion_candidate_count(state, Side::South) << ','
              << lion_candidate_count(state, Side::North) << "],\"protocol_request\":";
    qas::evaluation::write_json_string(std::cout, protocol_request(state, actions));
    std::cout << "}" << std::endl;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (character >= 'a' && character <= 'f')
        return 10 + character - 'a';
    return -1;
}

std::string decode_hex(const std::string& encoded) {
    if (encoded.size() % 2 != 0)
        throw std::invalid_argument("hex state has odd length");
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const int high = hex_value(encoded[index]);
        const int low = hex_value(encoded[index + 1]);
        if (high < 0 || low < 0)
            throw std::invalid_argument("hex state contains a non-hex character");
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

State parse_text(const std::string& text) {
    std::istringstream input(text);
    return qas::parse_state(input);
}

void write_error(const std::string& type, const std::string& message) {
    std::cout << "{\"ok\":false,\"error_type\":\"" << qas::evaluation::json_escape(type)
              << "\",\"error_message\":";
    qas::evaluation::write_json_string(std::cout, message);
    std::cout << "}" << std::endl;
}

int serve() {
    State state = qas::initial_state();
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            if (line == "INITIAL") {
                state = qas::initial_state();
                write_snapshot(state);
            } else if (line == "SNAPSHOT") {
                write_snapshot(state);
            } else if (line.rfind("LOAD_HEX ", 0) == 0) {
                state = parse_text(decode_hex(line.substr(9)));
                write_snapshot(state);
            } else if (line.rfind("APPLY ", 0) == 0) {
                const int action = std::stoi(line.substr(6));
                const std::vector<int> actions = legal_actions(state);
                if (!std::binary_search(actions.begin(), actions.end(), action)) {
                    write_error("illegal_action",
                                "action is not enabled by the authoritative mask");
                    continue;
                }
                qas::Undo undo;
                const Move move = actual_move(state, action);
                if (!move.valid() || !qas::apply_move(state, move, undo)) {
                    throw std::runtime_error("authoritative legal action failed to apply");
                }
                write_snapshot(state);
            } else if (line == "QUIT") {
                return 0;
            } else {
                write_error("invalid_command",
                            "expected INITIAL, SNAPSHOT, LOAD_HEX, APPLY, or QUIT");
            }
        } catch (const std::exception& error) {
            write_error("referee_error", error.what());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "serve")
        return serve();
    std::cerr << "usage: qas_evaluation_referee serve\n";
    return 2;
}

#include "io/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string initial_json(const std::array<int, qas::external_action_count>& mask) {
    const std::array<int, qas::kObservationFieldCount> empty{};
    const std::array<int, qas::kObservationFieldCount> mine{1, 1, 1, 1, 0, 1, 0, 1, 0};
    const std::array<int, qas::kObservationFieldCount> opponent{1, 1, 1, 1, 0, 0, 1, 0, 1};
    std::ostringstream output;
    output << "{\"command\":\"get_action\",\"observation\":{\"observation\":[";
    for (int source = 0; source < qas::external_source_count; ++source) {
        if (source != 0)
            output << ',';
        const bool mine_square = source == 7 || source == 9 || source == 10 || source == 11;
        const bool opponent_square = source == 0 || source == 1 || source == 2 || source == 4;
        const auto& entry = mine_square ? mine : (opponent_square ? opponent : empty);
        output << '[';
        for (std::size_t field = 0; field < qas::kObservationFieldCount; ++field) {
            if (field != 0)
                output << ',';
            output << entry[field];
        }
        output << ']';
    }
    output << "],\"action_mask\":[";
    for (int action = 0; action < qas::external_action_count; ++action) {
        if (action != 0)
            output << ',';
        output << mask[action];
    }
    output << "],\"turn\":0}}";
    return output.str();
}

void test_action_codec_all_indices() {
    for (int action = 0; action < qas::external_action_count; ++action) {
        const auto coordinates = qas::decode_action_index(action);
        check(qas::encode_action_index(coordinates.first, coordinates.second) == action,
              "all 240 action indices round-trip");
    }
}

void test_get_action_parse_and_mask_match() {
    std::array<int, qas::external_action_count> provisional{};
    const auto first_parse = qas::parse_protocol_message(initial_json(provisional));
    std::array<int, qas::external_action_count> official_mask{};
    for (const qas::Move& move : qas::generate_legal_moves(first_parse.state)) {
        official_mask[qas::encode_action_move(move)] = 1;
    }
    const auto message = qas::parse_protocol_message(initial_json(official_mask));
    check(message.command == qas::ProtocolCommand::GetAction, "get_action command parses");
    check(message.state.turn == 0 && message.state.hash == qas::zobrist_hash(message.state),
          "protocol state includes turn and a current hash");

    std::array<int, qas::external_action_count> generated{};
    for (const qas::Move& move : qas::generate_legal_moves(message.state)) {
        const int action = qas::encode_action_move(move);
        generated[action] = 1;
        check(qas::decode_action_move(message.state, action) == move,
              "legal internal move round-trips through official action codec");
        check(qas::action_is_allowed(message, action),
              "generated legal move is enabled by fixture action_mask");
    }
    check(generated == official_mask, "internal generator exactly matches fixture action_mask");
}

void test_end_game_parse() {
    const auto message = qas::parse_protocol_message("{\"command\":\"end_game\"}");
    check(message.command == qas::ProtocolCommand::EndGame, "end_game command parses");
}

}  // namespace

int main() {
    test_action_codec_all_indices();
    test_get_action_parse_and_mask_match();
    test_end_game_parse();
    if (failures != 0) {
        std::cerr << failures << " protocol test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All protocol tests passed\n";
    return EXIT_SUCCESS;
}

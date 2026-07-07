#pragma once

#include "rules/game.hpp"

#include <array>
#include <string>
#include <utility>

namespace qas {

enum class ProtocolCommand : std::uint8_t { GetAction, EndGame };

struct ProtocolMessage {
    ProtocolCommand command{ProtocolCommand::EndGame};
    State state{};
    std::array<std::uint8_t, external_action_count> action_mask{};
};

int encode_action_index(int source_index, int destination_index);
std::pair<int, int> decode_action_index(int action_index);
Move decode_action_move(const State& state, int action_index);
int encode_action_move(const Move& move);

ProtocolMessage parse_protocol_message(const std::string& json_line);
bool action_is_allowed(const ProtocolMessage& message, int action_index);

}  // namespace qas

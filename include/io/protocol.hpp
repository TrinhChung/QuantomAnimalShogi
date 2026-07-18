#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "rules/game.hpp"

namespace qas {

// One animal-form flag plus one-hot origin and owner fields per observation entry.
inline constexpr std::size_t kObservationFieldCount = kAnimalFormCount + 2 * kSideCount;
inline constexpr std::size_t kObservationValueCount =
    static_cast<std::size_t>(external_source_count) * kObservationFieldCount;

enum class ProtocolCommand : std::uint8_t { GetAction, EndGame };

struct ProtocolMessage {
    ProtocolCommand command{ProtocolCommand::EndGame};
    State state{};
    std::array<std::uint8_t, external_action_count> action_mask{};
};

/// @brief Encodes protocol source and destination coordinates into an action index.
/// @param source_index Board or hand source in the protocol range `[0, 19]`.
/// @param destination_index Board destination in the protocol range `[0, 11]`.
/// @return Encoded action index in `[0, 239]`.
/// @throws std::out_of_range If either coordinate is outside the protocol range.
int encode_action_index(int source_index, int destination_index);

/// @brief Decodes an external action index into source and destination coordinates.
/// @param action_index Encoded action index in `[0, 239]`.
/// @return Pair containing source then destination.
/// @throws std::out_of_range If the action index is invalid.
std::pair<int, int> decode_action_index(int action_index);

/// @brief Resolves an external action index to the physical piece in a state.
/// @param state State used to identify the piece at the decoded source.
/// @param action_index Encoded external action.
/// @return Internal move, or an invalid move when no movable piece matches the source.
/// @throws std::out_of_range If the action index is invalid.
Move decode_action_move(const State& state, int action_index);

/// @brief Encodes an internal move using the official action-index formula.
/// @param move Internal move to encode.
/// @return Encoded external action index.
/// @throws std::out_of_range If the move coordinates are outside protocol ranges.
int encode_action_move(const Move& move);

/// @brief Parses one JSON-line contest protocol message.
/// @param json_line Complete protocol line.
/// @return Parsed command, state and action mask.
/// @throws std::invalid_argument If required fields or invariants are invalid.
ProtocolMessage parse_protocol_message(const std::string& json_line);

/// @brief Tests whether an action is enabled by a protocol message mask.
/// @param message Parsed protocol message.
/// @param action_index Action index to query.
/// @return `true` only for an in-range action with a nonzero mask entry.
bool action_is_allowed(const ProtocolMessage& message, int action_index);

}  // namespace qas

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <string>
#include <vector>

#include "core/animal.hpp"

namespace qas {

inline constexpr int board_width = 3;
inline constexpr int board_height = 4;
inline constexpr int board_size = board_width * board_height;
inline constexpr int physical_piece_count = 8;
inline constexpr int external_source_count = board_size + physical_piece_count;
inline constexpr std::uint8_t first_hand_slot = board_size;
inline constexpr std::uint8_t hand_square = first_hand_slot;
inline constexpr int external_action_count = external_source_count * board_size;
inline constexpr int kPiecesPerOrigin = physical_piece_count / static_cast<int>(kSideCount);
inline constexpr std::uint16_t kTurnLimit = 256;
inline constexpr std::size_t kMoveBufferCapacity = 128;
inline constexpr std::int8_t kEmptyBoardSquare = -1;
inline constexpr std::int8_t kNoWinner = -1;
inline constexpr std::uint8_t kInvalidMoveField = (std::numeric_limits<std::uint8_t>::max)();

static_assert(physical_piece_count % static_cast<int>(kSideCount) == 0,
              "physical pieces must divide evenly between origins");
static_assert(external_source_count <= kInvalidMoveField && board_size <= kInvalidMoveField &&
                  physical_piece_count <= kInvalidMoveField,
              "invalid move sentinel must lie outside every valid move field");

enum class Terminal : std::uint8_t { None, Catch, Try, Draw, Illegal };

struct Move {
    std::uint8_t piece{kInvalidMoveField};
    std::uint8_t from{first_hand_slot};
    std::uint8_t to{kInvalidMoveField};

    /// @brief Compares the physical piece and both move coordinates.
    /// @param other Move to compare.
    /// @return `true` when every move field matches.
    bool operator==(const Move& other) const {
        return piece == other.piece && from == other.from && to == other.to;
    }
    /// @brief Tests moves for inequality.
    /// @param other Move to compare.
    /// @return `true` when at least one move field differs.
    bool operator!=(const Move& other) const { return !(*this == other); }

    /// @brief Tests whether every move field lies in its representable domain.
    /// @return `true` for a physical piece, external source and board destination.
    bool valid() const {
        return piece < physical_piece_count && from < external_source_count && to < board_size;
    }
};

struct State {
    std::array<std::int8_t, board_size> board{};
    std::array<std::uint8_t, physical_piece_count> pos{};
    std::array<Mask, physical_piece_count> mask{};
    std::uint8_t owner_bits{0};   // bit=1 means North
    std::uint8_t origin_bits{0};  // immutable lineage group; bit=1 means North
    Side side_to_move{Side::South};
    std::uint16_t turn{0};
    Terminal terminal{Terminal::None};
    std::int8_t winner{kNoWinner};  // 0 South, 1 North, -1 none
    std::uint64_t hash{0};
};

struct Undo {
    State previous{};
};

enum class PropagationMode : std::uint8_t { LineageLut, PermutationReference };

struct RuleMetrics {
    std::uint64_t pseudo_move_generation_calls{0};
    std::uint64_t pseudo_moves_generated{0};
    std::uint64_t legal_filter_calls{0};
    std::uint64_t legal_moves_generated{0};
    std::uint64_t pseudo_moves_rejected{0};
    std::uint64_t apply_move_internal_calls{0};
    std::uint64_t apply_move_internal_successes{0};
    std::uint64_t apply_move_internal_failures{0};
    std::uint64_t propagation_calls{0};
    std::uint64_t propagation_iterations{0};
    std::uint64_t hash_recompute_calls{0};
    double pseudo_move_generation_ms{0.0};
    double legal_filter_ms{0.0};
    double apply_move_internal_ms{0.0};
    double propagation_ms{0.0};
    double hash_recompute_ms{0.0};
};

struct MoveTables {
    // [form][side][square] -> destination bitboard
    std::array<std::array<std::array<std::uint16_t, board_size>, kSideCount>, kAnimalFormCount>
        move_table{};
    // [mask][side][square] -> union destination bitboard
    std::array<std::array<std::array<std::uint16_t, board_size>, kSideCount>, kAnimalFormMaskCount>
        quantum_move_table{};
    // [side][source][destination] -> forms capable of the move
    std::array<std::array<std::array<Mask, board_size>, board_size>, kSideCount>
        move_possible_mask{};
};

/// @brief Returns the immutable precomputed movement tables.
/// @return Process-wide movement-table instance.
const MoveTables& move_tables();

/// @brief Constructs the official initial game state and current Zobrist hash.
/// @return Deterministic initial state.
State initial_state();

/// @brief Validates structural, identity-propagation and hash invariants.
/// @param state State to validate without mutation.
/// @param error Optional destination for a human-readable failure reason.
/// @return `true` when every state invariant holds.
bool validate_state(const State& state, std::string* error = nullptr);

/// @brief Propagates identity constraints to a fixed point using a selected implementation.
/// @param state State whose piece masks may be reduced.
/// @param mode Propagation implementation used for the calculation.
/// @return `false` when the masks are contradictory or invalid.
bool propagate(State& state, PropagationMode mode = PropagationMode::LineageLut);

/// @brief Recomputes and stores the full Zobrist hash of a state.
/// @param state State whose hash field is updated.
void recompute_hash(State& state);

/// @brief Computes the full Zobrist hash without mutating the state.
/// @param state State to hash.
/// @return Hash covering positions, masks, ownership, side, turn and terminal result.
std::uint64_t zobrist_hash(const State& state);

/// @brief Applies one complete transition using a selected propagation implementation.
/// @param state State to mutate; restored to its input value on failure.
/// @param move Candidate internal move.
/// @param undo Receives the complete pre-move state on success.
/// @param mode Propagation implementation used by the transition.
/// @return `true` when the move is legal and the transition succeeds.
/// @post On success, propagation is at a fixed point and the stored hash is current.
bool apply_move(State& state,
                const Move& move,
                Undo& undo,
                PropagationMode mode = PropagationMode::LineageLut);

/// @brief Applies a profiled transition using a selected propagation implementation.
/// @param state State to mutate; restored to its input value on failure.
/// @param move Candidate internal move.
/// @param undo Receives the complete pre-move state on success.
/// @param metrics Metrics sink updated by the transition.
/// @param mode Propagation implementation used by the transition.
/// @return `true` when the move is legal and the transition succeeds.
bool apply_move_profiled(State& state,
                         const Move& move,
                         Undo& undo,
                         RuleMetrics& metrics,
                         PropagationMode mode = PropagationMode::LineageLut);

/// @brief Restores every value-relevant field captured before an applied move.
/// @param state State to restore.
/// @param undo Undo record produced by apply_move or apply_move_profiled.
void undo_move(State& state, const Undo& undo);

/// @brief Generates moves allowed by local geometry and occupancy only.
/// @param state Source state.
/// @return Pseudo-legal moves in deterministic piece/destination order.
std::vector<Move> generate_pseudo_legal_moves(const State& state);

/// @brief Generates fully legal moves using a selected propagation implementation.
/// @param state Source state.
/// @param mode Propagation implementation used while validating candidates.
/// @return Legal moves in deterministic order.
std::vector<Move> generate_legal_moves(const State& state,
                                       PropagationMode mode = PropagationMode::LineageLut);

/// @brief Writes pseudo-legal moves into a reusable output buffer.
/// @param state Source state.
/// @param output Buffer cleared and filled with pseudo-legal moves.
void generate_pseudo_legal_moves(const State& state, std::vector<Move>& output);

/// @brief Writes legal moves using reusable buffers and selected propagation.
/// @param state Source state.
/// @param output Buffer cleared and filled with legal moves.
/// @param scratch Reusable pseudo-legal candidate buffer.
/// @param mode Propagation implementation used while filtering candidates.
void generate_legal_moves(const State& state,
                          std::vector<Move>& output,
                          std::vector<Move>& scratch,
                          PropagationMode mode = PropagationMode::LineageLut);
/// @brief Writes profiled legal moves using selected propagation.
/// @param state Source state.
/// @param output Buffer cleared and filled with legal moves.
/// @param scratch Reusable pseudo-legal candidate buffer.
/// @param metrics Metrics sink updated by generation and filtering.
/// @param mode Propagation implementation used while filtering candidates.
void generate_legal_moves_profiled(const State& state,
                                   std::vector<Move>& output,
                                   std::vector<Move>& scratch,
                                   RuleMetrics& metrics,
                                   PropagationMode mode = PropagationMode::LineageLut);
/// @brief Tests whether any candidate identity can attack a board square.
/// @param state Source state.
/// @param square Board square to test.
/// @param by_side Attacking side.
/// @return `true` when at least one owned board piece can reach the square.
bool is_square_attacked(const State& state, int square, Side by_side);

/// @brief Tests whether a legal move immediately wins by Catch or Try for its mover.
/// @param state Source state.
/// @param move Candidate move.
/// @return `true` when applying the move ends with the current side as winner.
bool is_immediate_winning_move(const State& state, const Move& move);

/// @brief Parses the deterministic debug state text format.
/// @param input Stream containing one complete state.
/// @return Validated state with a current hash.
/// @throws std::invalid_argument If syntax or state invariants are invalid.
State parse_state(std::istream& input);

/// @brief Serializes a state in the deterministic debug text format.
/// @param state State to serialize.
/// @param output Destination stream.
void write_state(const State& state, std::ostream& output);

/// @brief Formats an internal move for diagnostics.
/// @param move Move to format.
/// @return Stable human-readable move string.
std::string move_string(const Move& move);

/// @brief Returns the stable name of a terminal result.
/// @param terminal Terminal result to name.
/// @return Human-readable terminal name.
std::string terminal_name(Terminal terminal);

/// @brief Compares every value-relevant field, including the stored hash.
/// @param left First state.
/// @param right Second state.
/// @return `true` when both states are byte-semantically equivalent.
bool same_state(const State& left, const State& right);

/// @brief Returns the opposing player side.
/// @param side Current side.
/// @return North for South and South for North.
Side opposite(Side side);

/// @brief Converts a side to its stable array index.
/// @param side Side to convert.
/// @return Zero for South or one for North.
int side_index(Side side);

/// @brief Tests current ownership of a physical piece.
/// @param state Source state.
/// @param piece Physical piece index.
/// @param side Side to compare against.
/// @return `true` when the current owner matches the side.
bool owned_by(const State& state, int piece, Side side);

/// @brief Tests the immutable origin side of a physical piece.
/// @param state Source state.
/// @param piece Physical piece index.
/// @param side Origin side to compare against.
/// @return `true` when the piece originated from the side.
bool originated_from(const State& state, int piece, Side side);

/// @brief Tests whether a position value names an external hand slot.
/// @param position Position value to inspect.
/// @return `true` for hand slots `[12, 19]`.
bool is_hand_position(std::uint8_t position);

}  // namespace qas

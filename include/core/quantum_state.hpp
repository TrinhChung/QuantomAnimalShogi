#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "core/animal.hpp"

namespace qas {

class Contradiction : public std::logic_error {
   public:
    using std::logic_error::logic_error;
};

struct Probability {
    std::size_t matching{};
    std::size_t total{};
};

class QuantumState {
   public:
    static constexpr std::size_t piece_count = 4;
    using Assignment = std::array<Animal, piece_count>;

    /// @brief Constructs an unconstrained four-piece quantum identity state.
    QuantumState();

    /// @brief Returns the currently possible animal identities for a piece.
    /// @param piece Zero-based physical piece index.
    /// @return Candidate-form mask for the piece.
    /// @throws std::out_of_range If the piece index is invalid.
    Mask possible(std::size_t piece) const;

    /// @brief Constrains a piece using an observed displacement.
    /// @param piece Zero-based physical piece index.
    /// @param dx Horizontal displacement.
    /// @param dy Vertical displacement.
    /// @param side Side that made the move.
    /// @throws std::out_of_range If the piece index is invalid.
    /// @throws Contradiction If no complete identity assignment remains.
    void observe_move(std::size_t piece, int dx, int dy, Side side);

    /// @brief Requires a piece to have one specific identity.
    /// @param piece Zero-based physical piece index.
    /// @param animal Required animal identity.
    /// @throws std::out_of_range If the piece index is invalid.
    /// @throws Contradiction If the requirement is globally inconsistent.
    void require(std::size_t piece, Animal animal);

    /// @brief Eliminates one identity from a piece.
    /// @param piece Zero-based physical piece index.
    /// @param animal Animal identity to eliminate.
    /// @throws std::out_of_range If the piece index is invalid.
    /// @throws Contradiction If the elimination removes every complete assignment.
    void eliminate(std::size_t piece, Animal animal);

    /// @brief Applies the identity measurement produced by a capture.
    /// @param piece Zero-based physical piece index.
    /// @param result Measured non-Lion identity.
    /// @throws std::invalid_argument If the result is Lion.
    /// @throws Contradiction If the measurement is globally inconsistent.
    void measure_capture(std::size_t piece, Animal result);

    /// @brief Enumerates all complete identity assignments allowed by the masks.
    /// @return Valid assignments in deterministic permutation order.
    std::vector<Assignment> assignments() const;

    /// @brief Computes the exact identity probability over valid assignments.
    /// @param piece Zero-based physical piece index.
    /// @param animal Animal identity to count.
    /// @return Matching and total assignment counts.
    /// @throws std::out_of_range If the piece index is invalid.
    Probability probability(std::size_t piece, Animal animal) const;

   private:
    std::array<Mask, piece_count> masks_{};

    /// @brief Validates a physical piece index.
    /// @param piece Zero-based physical piece index.
    /// @throws std::out_of_range If the index is outside the state.
    static void validate_piece(std::size_t piece);

    /// @brief Intersects one piece mask with an allowed set and normalizes globally.
    /// @param piece Zero-based physical piece index.
    /// @param allowed Forms allowed by the latest observation.
    /// @throws std::out_of_range If the piece index is invalid.
    /// @throws Contradiction If no complete assignment remains.
    void constrain(std::size_t piece, Mask allowed);

    /// @brief Removes mask values unsupported by any complete identity assignment.
    /// @throws Contradiction If the current constraints admit no complete assignment.
    void normalize();
};

}  // namespace qas

#pragma once

#include "core/animal.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

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

    QuantumState();

    Mask possible(std::size_t piece) const;
    void observe_move(std::size_t piece, int dx, int dy, Side side);
    void require(std::size_t piece, Animal animal);
    void eliminate(std::size_t piece, Animal animal);
    void measure_capture(std::size_t piece, Animal result);

    std::vector<Assignment> assignments() const;
    Probability probability(std::size_t piece, Animal animal) const;

private:
    std::array<Mask, piece_count> masks_{};

    static void validate_piece(std::size_t piece);
    void constrain(std::size_t piece, Mask allowed);
    void normalize();
};

}  // namespace qas

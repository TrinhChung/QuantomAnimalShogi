#include "core/quantum_state.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>

namespace qas {

const char* animal_name(Animal animal) {
    switch (animal) {
        case Animal::Chick:
            return "chick";
        case Animal::Elephant:
            return "elephant";
        case Animal::Giraffe:
            return "giraffe";
        case Animal::Lion:
            return "lion";
        case Animal::Hen:
            return "hen";
    }
    throw std::invalid_argument("unknown animal");
}

char animal_code(Animal animal) {
    switch (animal) {
        case Animal::Chick:
            return 'C';
        case Animal::Elephant:
            return 'E';
        case Animal::Giraffe:
            return 'G';
        case Animal::Lion:
            return 'L';
        case Animal::Hen:
            return 'H';
    }
    throw std::invalid_argument("unknown animal");
}

Animal parse_animal(const std::string& value) {
    if (value == "C" || value == "c" || value == "CHICK" || value == "chick") {
        return Animal::Chick;
    }
    if (value == "E" || value == "e" || value == "ELEPHANT" || value == "elephant") {
        return Animal::Elephant;
    }
    if (value == "G" || value == "g" || value == "GIRAFFE" || value == "giraffe") {
        return Animal::Giraffe;
    }
    if (value == "L" || value == "l" || value == "LION" || value == "lion") {
        return Animal::Lion;
    }
    if (value == "H" || value == "h" || value == "HEN" || value == "hen") {
        return Animal::Hen;
    }
    throw std::invalid_argument("unknown animal: " + value);
}

std::string mask_string(Mask mask) {
    std::string result;
    for (Animal animal : all_forms) {
        if (contains(mask, animal)) {
            result.push_back(animal_code(animal));
        }
    }
    return result.empty() ? "-" : result;
}

Mask animals_for_move(int dx, int dy, Side side) {
    if (dx == 0 && dy == 0) {
        return 0;
    }
    if (std::abs(dx) > 1 || std::abs(dy) > 1) {
        return 0;
    }

    Mask result = bit(Animal::Lion);
    if (std::abs(dx) == 1 && std::abs(dy) == 1) {
        result |= bit(Animal::Elephant);
    }
    if (std::abs(dx) + std::abs(dy) == 1) {
        result |= bit(Animal::Giraffe);
    }
    const int forward = side == Side::South ? -1 : 1;
    if (dx == 0 && dy == forward) {
        result |= bit(Animal::Chick);
    }
    return result;
}

QuantumState::QuantumState() {
    masks_.fill(all_mask);
}

void QuantumState::validate_piece(std::size_t piece) {
    if (piece >= piece_count) {
        throw std::out_of_range("piece index is out of range");
    }
}

Mask QuantumState::possible(std::size_t piece) const {
    validate_piece(piece);
    return masks_[piece];
}

void QuantumState::constrain(std::size_t piece, Mask allowed) {
    validate_piece(piece);
    const auto previous = masks_;
    masks_[piece] &= allowed;
    try {
        normalize();
    } catch (...) {
        masks_ = previous;
        throw;
    }
}

void QuantumState::observe_move(std::size_t piece, int dx, int dy, Side side) {
    const Mask movers = animals_for_move(dx, dy, side);
    if (movers == 0) {
        throw Contradiction("no animal can make that move");
    }
    constrain(piece, movers);
}

void QuantumState::require(std::size_t piece, Animal animal) {
    constrain(piece, bit(animal));
}

void QuantumState::eliminate(std::size_t piece, Animal animal) {
    constrain(piece, static_cast<Mask>(all_mask & ~bit(animal)));
}

void QuantumState::measure_capture(std::size_t piece, Animal result) {
    if (result == Animal::Lion) {
        throw std::invalid_argument("a captured unresolved piece cannot measure as lion");
    }
    require(piece, result);
}

std::vector<QuantumState::Assignment> QuantumState::assignments() const {
    Assignment permutation = all_animals;
    std::vector<Assignment> result;
    do {
        bool valid = true;
        for (std::size_t piece = 0; piece < piece_count; ++piece) {
            if (!contains(masks_[piece], permutation[piece])) {
                valid = false;
                break;
            }
        }
        if (valid) {
            result.push_back(permutation);
        }
    } while (std::next_permutation(permutation.begin(), permutation.end(), [](Animal left, Animal right) {
        return static_cast<unsigned>(left) < static_cast<unsigned>(right);
    }));
    return result;
}

void QuantumState::normalize() {
    const auto valid = assignments();
    if (valid.empty()) {
        throw Contradiction("constraints admit no complete identity assignment");
    }

    std::array<Mask, piece_count> supported{};
    for (const Assignment& assignment : valid) {
        for (std::size_t piece = 0; piece < piece_count; ++piece) {
            supported[piece] |= bit(assignment[piece]);
        }
    }
    masks_ = supported;
}

Probability QuantumState::probability(std::size_t piece, Animal animal) const {
    validate_piece(piece);
    const auto valid = assignments();
    const auto matches = std::count_if(valid.begin(), valid.end(), [piece, animal](const Assignment& item) {
        return item[piece] == animal;
    });
    return {static_cast<std::size_t>(matches), valid.size()};
}

}  // namespace qas

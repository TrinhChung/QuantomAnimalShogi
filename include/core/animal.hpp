#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace qas {

enum class Animal : std::uint8_t {
    Chick = 0,
    Giraffe = 1,
    Elephant = 2,
    Lion = 3,
    Hen = 4
};
enum class Side : std::uint8_t { South, North };
using Mask = std::uint8_t;

inline constexpr std::array<Animal, 4> all_animals{
    Animal::Chick, Animal::Giraffe, Animal::Elephant, Animal::Lion};
inline constexpr std::array<Animal, 5> all_forms{
    Animal::Chick, Animal::Giraffe, Animal::Elephant, Animal::Lion, Animal::Hen};

constexpr Mask bit(Animal animal) {
    return static_cast<Mask>(1U << static_cast<unsigned>(animal));
}

inline constexpr Mask all_mask = bit(Animal::Chick) | bit(Animal::Elephant) |
                                 bit(Animal::Giraffe) | bit(Animal::Lion);
inline constexpr Mask all_form_mask = all_mask | bit(Animal::Hen);

constexpr bool contains(Mask mask, Animal animal) {
    return (mask & bit(animal)) != 0;
}

const char* animal_name(Animal animal);
char animal_code(Animal animal);
Animal parse_animal(const std::string& value);
std::string mask_string(Mask mask);

// dx grows to the right. South moves toward dy=-1; North toward dy=+1.
Mask animals_for_move(int dx, int dy, Side side);

}  // namespace qas

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace qas {

enum class Animal : std::uint8_t { Chick = 0, Giraffe = 1, Elephant = 2, Lion = 3, Hen = 4 };
enum class Side : std::uint8_t { South, North };
using Mask = std::uint8_t;

inline constexpr std::size_t kSideCount = 2;

inline constexpr std::array<Animal, 4> all_animals{
    Animal::Chick, Animal::Giraffe, Animal::Elephant, Animal::Lion};
inline constexpr std::array<Animal, 5> all_forms{
    Animal::Chick, Animal::Giraffe, Animal::Elephant, Animal::Lion, Animal::Hen};
inline constexpr std::size_t kAnimalFormCount = all_forms.size();
inline constexpr std::size_t kAnimalFormMaskCount = std::size_t{1} << kAnimalFormCount;

static_assert(kAnimalFormCount <= std::numeric_limits<Mask>::digits,
              "animal forms must fit in Mask");

/// @brief Returns the mask bit assigned to an animal form.
/// @param animal Animal form to encode.
/// @return A mask containing only the requested form.
constexpr Mask bit(Animal animal) {
    return static_cast<Mask>(1U << static_cast<unsigned>(animal));
}

inline constexpr Mask all_mask =
    bit(Animal::Chick) | bit(Animal::Elephant) | bit(Animal::Giraffe) | bit(Animal::Lion);
inline constexpr Mask all_form_mask = all_mask | bit(Animal::Hen);

/// @brief Tests whether an animal form is present in a mask.
/// @param mask Candidate-form mask to inspect.
/// @param animal Animal form to query.
/// @return `true` when the form bit is set.
constexpr bool contains(Mask mask, Animal animal) {
    return (mask & bit(animal)) != 0;
}

/// @brief Returns the single-character code of an animal form.
/// @param animal Animal form to encode.
/// @return Stable character code used by text formats.
/// @throws std::invalid_argument If the enum value is unknown.
char animal_code(Animal animal);

/// @brief Parses an animal name or single-character code.
/// @param value Textual animal representation.
/// @return Parsed animal form.
/// @throws std::invalid_argument If the text does not identify a supported form.
Animal parse_animal(const std::string& value);

/// @brief Formats every animal form present in a mask.
/// @param mask Candidate-form mask to format.
/// @return Concatenated form codes in stable order.
std::string mask_string(Mask mask);

/// @brief Returns all animal forms capable of a displacement.
/// @param dx Horizontal displacement, positive to the right.
/// @param dy Vertical displacement; South advances toward -1 and North toward +1.
/// @param side Side whose forward direction defines Chick and Hen movement.
/// @return Mask of forms capable of the displacement.
Mask animals_for_move(int dx, int dy, Side side);

}  // namespace qas

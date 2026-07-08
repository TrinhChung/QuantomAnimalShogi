#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rules/game.hpp"

namespace qas::benchmark {

struct Fixture {
    std::string name;
    std::string category;
    std::uint32_t seed{0};
    State state{};
    std::vector<int> action_sequence;
    std::array<std::uint8_t, external_action_count> legal_action_mask{};
    std::size_t expected_legal_move_count{0};
};

std::vector<Fixture> generate_stage35_fixtures();
void save_stage35_fixtures(const std::vector<Fixture>& fixtures, const std::string& directory);
std::vector<Fixture> load_stage35_fixtures(const std::string& directory);

}  // namespace qas::benchmark

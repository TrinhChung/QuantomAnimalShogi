#include "core/quantum_state.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void check_contradiction(Function function, const std::string& message) {
    try {
        function();
        check(false, message);
    } catch (const qas::Contradiction&) {
    }
}

void test_initial_state() {
    qas::QuantumState state;
    check(state.assignments().size() == 24, "initial state has 4! assignments");
    for (qas::Animal animal : qas::all_animals) {
        const auto probability = state.probability(0, animal);
        check(probability.matching == 6 && probability.total == 24,
              "initial probability is 6/24");
    }
}

void test_move_filtering() {
    qas::QuantumState state;
    state.observe_move(0, 1, 0, qas::Side::South);
    check(state.possible(0) == (qas::bit(qas::Animal::Giraffe) | qas::bit(qas::Animal::Lion)),
          "horizontal move leaves giraffe and lion");
    check(state.assignments().size() == 12, "horizontal move leaves 12 assignments");

    state.observe_move(0, 1, 1, qas::Side::South);
    check(state.possible(0) == qas::bit(qas::Animal::Lion),
          "diagonal after horizontal fixes lion");
    check(state.assignments().size() == 6, "fixed piece leaves 3! assignments");
}

void test_global_propagation() {
    qas::QuantumState state;
    state.require(0, qas::Animal::Lion);
    for (std::size_t piece = 1; piece < qas::QuantumState::piece_count; ++piece) {
        check(!qas::contains(state.possible(piece), qas::Animal::Lion),
              "fixed identity is removed from every other piece");
    }
}

void test_orientation() {
    const auto south = qas::animals_for_move(0, -1, qas::Side::South);
    const auto north = qas::animals_for_move(0, 1, qas::Side::North);
    check(qas::contains(south, qas::Animal::Chick), "south chick moves toward -y");
    check(qas::contains(north, qas::Animal::Chick), "north chick moves toward +y");
    check(!qas::contains(qas::animals_for_move(0, 1, qas::Side::South), qas::Animal::Chick),
          "south chick cannot move backward");
}

void test_contradiction_is_transactional() {
    qas::QuantumState state;
    state.require(0, qas::Animal::Lion);
    const auto before = state.possible(1);
    check_contradiction([&state] { state.require(1, qas::Animal::Lion); },
                        "two lions contradict all-different constraint");
    check(state.possible(1) == before, "failed constraint does not mutate state");
}

void test_capture() {
    qas::QuantumState state;
    state.measure_capture(2, qas::Animal::Elephant);
    check(state.possible(2) == qas::bit(qas::Animal::Elephant),
          "capture measurement fixes non-lion identity");
    bool invalid = false;
    try {
        state.measure_capture(1, qas::Animal::Lion);
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    check(invalid, "capture measurement rejects lion");
}

}  // namespace

int main() {
    test_initial_state();
    test_move_filtering();
    test_global_propagation();
    test_orientation();
    test_contradiction_is_transactional();
    test_capture();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All quantum-state tests passed\n";
    return EXIT_SUCCESS;
}

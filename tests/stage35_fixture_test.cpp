#include "stage35_fixture.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "io/protocol.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    std::vector<qas::benchmark::Fixture> fixtures;
    try {
        fixtures = qas::benchmark::load_stage35_fixtures("benchmarks/fixtures/stage35");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    check(fixtures.size() == 110, "Stage 3.5 fixture set has 10 required plus 100 random states");
    std::map<std::string, int> category_counts;
    for (const auto& fixture : fixtures) {
        ++category_counts[fixture.category];
        check(fixture.state.hash == qas::zobrist_hash(fixture.state),
              fixture.name + " preserves its Zobrist hash");
        const auto legal = qas::generate_legal_moves(fixture.state);
        check(legal.size() == fixture.expected_legal_move_count,
              fixture.name + " preserves legal move count");
        for (const qas::Move& move : legal) {
            const int action = qas::encode_action_move(move);
            check(fixture.legal_action_mask[static_cast<std::size_t>(action)] == 1,
                  fixture.name + " contains every generated legal action");
            check(qas::decode_action_move(fixture.state, action) == move,
                  fixture.name + " legal moves round-trip through the action codec");
        }
    }
    check(category_counts["required"] == 10, "required fixture count is 10");
    check(category_counts["random_ply4"] == 20, "ply-4 fixture count is 20");
    check(category_counts["random_ply8"] == 20, "ply-8 fixture count is 20");
    check(category_counts["random_ply16"] == 20, "ply-16 fixture count is 20");
    check(category_counts["random_hands"] == 20, "hand fixture count is 20");
    check(category_counts["random_low_uncertainty"] == 20, "low-uncertainty fixture count is 20");
    if (failures != 0) {
        std::cerr << failures << " Stage 3.5 fixture test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Stage 3.5 fixture tests passed\n";
    return EXIT_SUCCESS;
}

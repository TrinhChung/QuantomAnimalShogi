#include "search/nnue.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "search/alpha_beta.hpp"
#include "search/nnue_features.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Writes a network whose only non-zero parameter is the output bias.
std::string write_bias_network(const std::string& path, int features, double bias) {
    std::ofstream out(path);
    const int h1 = 4, h2 = 2;
    out << "nnue v1 " << features << ' ' << h1 << ' ' << h2 << " 100\n";
    for (long long i = 0; i < static_cast<long long>(features) * h1; ++i)
        out << "0 ";
    out << '\n';
    for (int i = 0; i < h1; ++i)
        out << "0 ";
    out << '\n';
    for (int i = 0; i < h1 * h2; ++i)
        out << "0 ";
    out << '\n';
    for (int i = 0; i < h2; ++i)
        out << "0 ";
    out << '\n';
    for (int i = 0; i < h2; ++i)
        out << "0 ";
    out << '\n' << bias << '\n';
    return path;
}

void test_features_are_in_range_and_side_symmetric() {
    qas::State south = qas::initial_state();
    auto a = qas::nnue_active_features(south);
    for (int feature : a)
        check(feature >= 0 && feature < qas::kNnueFeatureCount, "feature index in range");
    // The initial position is point-symmetric, so seen from North it must encode identically.
    qas::State north = south;
    north.side_to_move = qas::Side::North;
    qas::recompute_hash(north);
    auto b = qas::nnue_active_features(north);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    check(a == b, "initial position encodes the same from both sides");
}

void test_loader_and_bias_network() {
    const std::string path = "qas_nnue_test.txt";
    qas::NnueNetwork network;
    check(!network.loaded() && network.correction(qas::initial_state()) == 0,
          "unloaded network adds nothing");
    std::string error;
    check(network.load(write_bias_network(path, qas::kNnueFeatureCount, 0.5), &error) &&
              error.empty(),
          "well-formed network loads");
    check(network.correction(qas::initial_state()) == 50, "bias-only network adds scale * bias");
    qas::NnueNetwork wrong;
    check(!wrong.load(write_bias_network(path, qas::kNnueFeatureCount - 1, 0.5)) && !wrong.loaded(),
          "a network for another feature layout is rejected");
    {
        std::ofstream truncated(path);
        truncated << "nnue v1 " << qas::kNnueFeatureCount << " 4 2 100\n0 0 0\n";
    }
    check(!wrong.load(path), "a truncated network is rejected");
    std::remove(path.c_str());
}

void test_zero_network_matches_v2_search() {
    const std::string path = "qas_nnue_zero.txt";
    qas::NnueNetwork zero;
    check(zero.load(write_bias_network(path, qas::kNnueFeatureCount, 0.0)), "zero network loads");
    std::remove(path.c_str());
    qas::SearchOptions options;
    options.max_depth = 4;
    options.qsearch_depth = 4;
    options.eval_v2_enabled = true;
    options.time_limit_ms = options.hard_time_limit_ms = options.soft_time_limit_ms = 60'000;
    qas::AlphaBetaEngine plain(1U << 16U);
    const auto expected = plain.find_best_move(qas::initial_state(), options);
    options.nnue = &zero;
    qas::AlphaBetaEngine with_nnue(1U << 16U);
    const auto actual = with_nnue.find_best_move(qas::initial_state(), options);
    check(expected.best_move == actual.best_move && expected.score == actual.score,
          "a zero NNUE correction leaves the v2 search unchanged");
}

}  // namespace

int main() {
    test_features_are_in_range_and_side_symmetric();
    test_loader_and_bias_network();
    test_zero_network_matches_v2_search();
    if (failures != 0) {
        std::cerr << failures << " NNUE test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All NNUE tests passed\n";
    return EXIT_SUCCESS;
}

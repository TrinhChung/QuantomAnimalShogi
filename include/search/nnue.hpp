#pragma once

#include <string>
#include <vector>

#include "rules/game.hpp"
#include "search/nnue_features.hpp"

namespace qas {

/// Residual NNUE correction added to the linear evaluation (CPU only, float inference).
/// correction = scale * (W3 . crelu(W2^T . crelu(b1 + sum_i W1[f_i]) + b2) + b3), in centipawns.
/// Weights come from offline training (`nnue v1 <features> <h1> <h2> <scale>` text format).
class NnueNetwork {
   public:
    /// @brief Loads weights; returns false (and keeps the network unloaded) on any format problem.
    bool load(const std::string& path, std::string* error = nullptr);

    /// @brief True after a successful load.
    bool loaded() const { return h1_ > 0; }

    /// @brief Correction for a state, from the side to move's perspective.
    int correction(const State& state) const;

    /// @brief Correction for already-encoded active features.
    double correction_from_features(const NnueActive& active) const;

   private:
    int h1_{0};
    int h2_{0};
    double scale_{0.0};
    std::vector<float> w1_, b1_, w2_, b2_, w3_;
    float b3_{0.0F};
};

}  // namespace qas

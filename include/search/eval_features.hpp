#pragma once

#include <array>
#include <vector>

#include "rules/game.hpp"

namespace qas {

/// Linear evaluation terms, all measured from the side to move ("us") against the opponent
/// ("them"). Paired `Us`/`Them` terms are kept separate because the side to move can react first.
enum EvalFeature : int {
    kFeatMaterial,    // expected material (mask average), us - them
    kFeatIdentities,  // extra candidate identities (popcount - 1), us - them
    kFeatMobility,    // pseudo destinations (+ empty squares per hand piece), us - them
    kFeatUsSureLionAttacked,
    kFeatThemSureLionAttacked,
    kFeatUsLionCandAttacked,
    kFeatThemLionCandAttacked,
    kFeatSureLionTryProgress,  // rows advanced by a certain Lion, us - them
    kFeatCandTryProgress,      // rows advanced by Lion candidates, us - them
    kFeatSureLionInHand,       // certain Lion stranded in hand, us - them
    kFeatLionCandidates,       // Lion-capable pieces, us - them
    kFeatUsImmediateWin,       // side to move can Catch/Try now
    kFeatThemImmediateWin,     // opponent would win if it were to move
    kFeatUsLionEscapes,        // fewest safe Lion steps over our on-board candidates
    kFeatThemLionEscapes,
    kFeatUsLionDefended,  // our on-board candidates defended by our pieces
    kFeatThemLionDefended,
    kFeatUsLionPressure,  // most enemy pieces bearing on one candidate's square or zone
    kFeatThemLionPressure,
    kFeatUsDropThreat,  // empty squares around our candidates while the opponent holds hand pieces
    kFeatThemDropThreat,
    kFeatUsHanging,  // our non-Lion pieces attacked and undefended
    kFeatThemHanging,
    kFeatHandPieces,  // pieces in hand, us - them
    kFeatTempo,       // constant 1 (side-to-move bonus)
    kEvalFeatureCount
};

/// Score = sum(weight * feature) / kEvalWeightScale.
inline constexpr int kEvalWeightScale = 8;
using EvalFeatures = std::array<int, kEvalFeatureCount>;
using EvalWeights = std::array<int, kEvalFeatureCount>;

/// @brief Returns weights that reproduce the legacy optimized evaluation exactly.
const EvalWeights& legacy_eval_weights();

/// @brief Extracts every linear evaluation term for a non-terminal state.
/// @param state Non-terminal state; not mutated.
/// @param buffer Reusable pseudo-move buffer.
/// @return Feature values from the side to move's perspective.
EvalFeatures extract_eval_features(const State& state, std::vector<Move>& buffer);

/// @brief Applies weights to extracted features.
int weighted_eval(const EvalFeatures& features, const EvalWeights& weights);

}  // namespace qas

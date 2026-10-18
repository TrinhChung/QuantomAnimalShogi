#include "search/nnue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>

namespace qas {
namespace {

bool read_floats(std::istream& input, std::vector<float>& out, std::size_t count) {
    out.resize(count);
    for (float& value : out) {
        if (!(input >> value))
            return false;
    }
    return true;
}

constexpr int kMaxHidden = 256;

float crelu(float value) {
    return std::min(1.0F, std::max(0.0F, value));
}

}  // namespace

bool NnueNetwork::load(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr)
            *error = message;
        h1_ = 0;
        return false;
    };
    std::ifstream input(path);
    if (!input)
        return fail("cannot open NNUE file: " + path);
    std::string magic, version;
    int features = 0, h1 = 0, h2 = 0;
    double scale = 0.0;
    if (!(input >> magic >> version >> features >> h1 >> h2 >> scale) || magic != "nnue" ||
        version != "v1")
        return fail("bad NNUE header");
    if (features != kNnueFeatureCount || h1 <= 0 || h2 <= 0 || h1 > kMaxHidden || h2 > kMaxHidden ||
        !(scale > 0.0) || !std::isfinite(scale))
        return fail("NNUE shape does not match this engine");
    std::vector<float> b3;
    if (!read_floats(input, w1_, static_cast<std::size_t>(features) * h1) ||
        !read_floats(input, b1_, h1) ||
        !read_floats(input, w2_, static_cast<std::size_t>(h1) * h2) ||
        !read_floats(input, b2_, h2) || !read_floats(input, w3_, h2) || !read_floats(input, b3, 1))
        return fail("truncated NNUE weights");
    float extra = 0.0F;
    if (input >> extra)
        return fail("trailing data in NNUE file");
    h2_ = h2;
    scale_ = scale;
    b3_ = b3[0];
    h1_ = h1;
    return true;
}

double NnueNetwork::correction_from_features(const NnueActive& active) const {
    if (!loaded())
        return 0.0;
    std::array<float, kMaxHidden> hidden1;
    std::copy(b1_.begin(), b1_.end(), hidden1.begin());
    for (const int feature : active) {
        const float* row = &w1_[static_cast<std::size_t>(feature) * h1_];
        for (int j = 0; j < h1_; ++j)
            hidden1[j] += row[j];
    }
    for (int j = 0; j < h1_; ++j)
        hidden1[j] = crelu(hidden1[j]);
    double output = b3_;
    for (int k = 0; k < h2_; ++k) {
        float sum = b2_[k];
        for (int j = 0; j < h1_; ++j)
            sum += hidden1[j] * w2_[static_cast<std::size_t>(j) * h2_ + k];
        output += crelu(sum) * w3_[k];
    }
    return scale_ * output;
}

int NnueNetwork::correction(const State& state) const {
    return static_cast<int>(std::lround(correction_from_features(nnue_active_features(state))));
}

}  // namespace qas

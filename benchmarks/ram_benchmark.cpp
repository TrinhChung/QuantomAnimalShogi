#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>

#include "core/config.hpp"
#include "search/alpha_beta.hpp"

int main(int argc, char** argv) {
    const int depth = argc >= 2 ? std::max(1, std::atoi(argv[1])) : 4;
    constexpr std::array<std::size_t, 7> sizes{64, 128, 256, 512, 1024, 2048, 4096};
    std::cout << "requested_mb,allocated_mb,entries,depth,nodes,time_ms,tt_hits,tt_probes,status\n";
    for (std::size_t requested : sizes) {
        qas::EngineConfig config = qas::profile_config("local_benchmark");
        config.tt.auto_size_enabled = false;
        config.tt.size_mb = requested;
        config.memory.max_total_memory_mb = 8192;
        config.memory.reserve_memory_mb = 512;
        const auto estimate = qas::estimate_resources(config, sizeof(qas::TTEntry), 16, 4096);
        if (requested > 256) {
            std::cout << requested << ',' << estimate.tt_size_mb << ',' << estimate.tt_entry_count
                      << ",0,0,0,0,0,calculated_only\n";
            continue;
        }
        qas::AlphaBetaEngine engine(estimate.tt_entry_count);
        qas::SearchOptions options;
        options.max_depth = depth;
        options.time_limit_ms = 60'000;
        options.soft_time_limit_ms = 60'000;
        options.hard_time_limit_ms = 60'000;
        options.pvs_enabled = true;
        options.aspiration_enabled = false;
        const auto result = engine.find_best_move(qas::initial_state(), options);
        std::cout << requested << ',' << (engine.tt_bytes() >> 20U) << ','
                  << engine.tt_entry_count() << ',' << result.stats.depth_reached << ','
                  << result.stats.searched_nodes << ',' << std::fixed << std::setprecision(3)
                  << result.stats.elapsed_ms << ',' << result.stats.tt_hits << ','
                  << result.stats.tt_probes << ",measured\n";
    }
}

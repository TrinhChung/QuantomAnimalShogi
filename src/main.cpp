#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "core/config.hpp"
#include "core/quantum_state.hpp"
#include "exact/equivalence.hpp"
#include "io/protocol.hpp"
#include "search/alpha_beta.hpp"

namespace {

using qas::Animal;
using qas::QuantumState;
using qas::Side;

/// @brief Converts a one-based demo piece number to a zero-based index.
/// @param one_based User-facing piece number in `[1, 4]`.
/// @return Zero-based piece index.
/// @throws std::invalid_argument If the number is outside the supported range.
std::size_t piece_index(int one_based) {
    if (one_based < 1 || one_based > 4) {
        throw std::invalid_argument("piece must be in [1, 4]");
    }
    return static_cast<std::size_t>(one_based - 1);
}

/// @brief Parses a demo side token.
/// @param value Short or long South/North token.
/// @return Parsed side.
/// @throws std::invalid_argument If the token is unsupported.
Side parse_side(const std::string& value) {
    if (value == "S" || value == "s" || value == "SOUTH" || value == "south") {
        return Side::South;
    }
    if (value == "N" || value == "n" || value == "NORTH" || value == "north") {
        return Side::North;
    }
    throw std::invalid_argument("side must be S or N");
}

/// @brief Verifies that an operation stream contains no trailing token.
/// @param input Operation stream positioned after expected arguments.
/// @throws std::invalid_argument If another token remains.
void ensure_end(std::istringstream& input) {
    std::string extra;
    if (input >> extra) {
        throw std::invalid_argument("unexpected token: " + extra);
    }
}

/// @brief Parses and applies one development-solver observation.
/// @param state Quantum identity state to mutate.
/// @param line Complete operation line.
/// @throws std::invalid_argument If syntax or command arguments are invalid.
/// @throws qas::Contradiction If the observation makes identities inconsistent.
void apply_operation(QuantumState& state, const std::string& line) {
    std::istringstream input(line);
    std::string command;
    input >> command;
    if (command.empty()) {
        throw std::invalid_argument("empty operation");
    }

    int piece = 0;
    if (command == "MOVE") {
        int dx = 0;
        int dy = 0;
        std::string side;
        if (!(input >> piece >> dx >> dy >> side)) {
            throw std::invalid_argument("usage: MOVE piece dx dy S|N");
        }
        ensure_end(input);
        state.observe_move(piece_index(piece), dx, dy, parse_side(side));
        return;
    }

    std::string animal;
    if (command == "FIX" || command == "FORBID" || command == "CAPTURE") {
        if (!(input >> piece >> animal)) {
            throw std::invalid_argument("usage: " + command + " piece animal");
        }
        ensure_end(input);
        const Animal parsed = qas::parse_animal(animal);
        if (command == "FIX") {
            state.require(piece_index(piece), parsed);
        } else if (command == "FORBID") {
            state.eliminate(piece_index(piece), parsed);
        } else {
            state.measure_capture(piece_index(piece), parsed);
        }
        return;
    }

    if (command == "PROMOTE" || command == "DECLINE") {
        if (!(input >> piece)) {
            throw std::invalid_argument("usage: " + command + " piece");
        }
        ensure_end(input);
        if (command == "PROMOTE") {
            state.require(piece_index(piece), Animal::Chick);
        } else {
            state.eliminate(piece_index(piece), Animal::Chick);
        }
        return;
    }

    throw std::invalid_argument("unknown operation: " + command);
}

/// @brief Writes quantum identity assignments and exact probabilities.
/// @param state Quantum identity state to report.
/// @param output Destination stream.
void print_state(const QuantumState& state, std::ostream& output) {
    const auto assignments = state.assignments();
    output << "assignments " << assignments.size() << '\n';
    for (std::size_t piece = 0; piece < QuantumState::piece_count; ++piece) {
        output << "piece " << (piece + 1) << ':';
        for (Animal animal : qas::all_animals) {
            const auto probability = state.probability(piece, animal);
            output << ' ' << qas::animal_code(animal) << '=' << probability.matching << '/'
                   << probability.total;
        }
        output << " possible=" << qas::mask_string(state.possible(piece)) << '\n';
    }
}

/// @brief Executes the line-oriented quantum-identity development solver.
/// @param input Operation-count and operation stream.
/// @param output Normal result and contradiction stream.
/// @param error Invalid-input diagnostic stream.
/// @return Zero on success, one on invalid input, or two on contradiction.
int solve(std::istream& input, std::ostream& output, std::ostream& error) {
    int operation_count = 0;
    if (!(input >> operation_count) || operation_count < 0) {
        error << "invalid operation count\n";
        return 1;
    }
    std::string line;
    std::getline(input, line);

    QuantumState state;
    for (int operation = 1; operation <= operation_count; ++operation) {
        if (!std::getline(input, line)) {
            error << "missing operation " << operation << '\n';
            return 1;
        }
        try {
            apply_operation(state, line);
        } catch (const qas::Contradiction& exception) {
            output << "IMPOSSIBLE at operation " << operation << ": " << exception.what() << '\n';
            return 2;
        } catch (const std::exception& exception) {
            error << "invalid operation " << operation << ": " << exception.what() << '\n';
            return 1;
        }
    }

    print_state(state, output);
    return 0;
}

/// @brief Prints a fixed quantum-identity demonstration.
/// @param output Destination stream.
void demo(std::ostream& output) {
    QuantumState state;
    output << "Initial state\n";
    print_state(state, output);
    state.observe_move(0, 1, 0, Side::South);
    output << "\nAfter piece 1 moves horizontally (giraffe or lion)\n";
    print_state(state, output);
    state.observe_move(1, 1, 1, Side::South);
    output << "\nAfter piece 2 moves diagonally (elephant or lion)\n";
    print_state(state, output);
}

/// @brief Prints supported command-line modes.
/// @param output Destination stream.
/// @param program Executable name displayed in the usage line.
void print_usage(std::ostream& output, const char* program) {
    output << "Usage: " << program
           << " <protocol|demo|solve|engine-demo|search|search-leq [milliseconds] "
              "[max-depth] [threshold]|legal>\n";
}

/// @brief Writes a development search result and instrumentation summary.
/// @param result Completed search result.
/// @param output Destination stream.
void print_search_result(const qas::SearchResult& result, std::ostream& output) {
    if (!result.has_move) {
        output << "NO_LEGAL_MOVE\n";
        return;
    }
    output << "action " << qas::encode_action_move(result.best_move) << '\n'
           << "move " << qas::move_string(result.best_move) << '\n'
           << "score " << result.score << '\n'
           << "depth " << result.stats.depth_reached << '\n'
           << "nodes " << result.stats.searched_nodes << '\n'
           << "expanded " << result.stats.expanded_nodes << '\n'
           << "cutoffs " << result.stats.cutoffs << '\n'
           << "average_L " << std::fixed << std::setprecision(3)
           << result.stats.average_legal_moves() << '\n'
           << "average_L_eq " << result.stats.average_equivalent_moves() << '\n'
           << "duplicate_ratio " << result.stats.duplicate_ratio() << '\n'
           << "leq_grouping_ms " << result.stats.leq_grouping_ms << '\n'
           << "tt " << result.stats.tt_hits << '/' << result.stats.tt_probes << '\n'
           << "elapsed_ms " << std::setprecision(3) << result.stats.elapsed_ms << '\n';
    for (const auto& depth : result.stats.completed_depths) {
        output << "completed depth=" << depth.depth << " score=" << depth.score
               << " move=" << qas::move_string(depth.best_move) << " nodes=" << depth.nodes
               << " ms=" << depth.elapsed_ms << '\n';
    }
}

/// @brief Maps runtime engine configuration to one search invocation.
/// @param config Loaded engine configuration.
/// @return Search options with optional successor equivalence installed.
qas::SearchOptions search_options(const qas::EngineConfig& config) {
    qas::SearchOptions options;
    options.max_depth = config.search.max_depth;
    options.soft_time_limit_ms = config.search.soft_time_limit_ms;
    options.hard_time_limit_ms = config.search.hard_time_limit_ms;
    options.time_limit_ms = config.search.hard_time_limit_ms;
    options.iterative_deepening_enabled = config.search.iterative_deepening_enabled;
    options.pvs_enabled = config.search.pvs_enabled;
    options.aspiration_enabled = config.search.aspiration_enabled;
    options.aspiration_initial_window = config.search.aspiration_initial_window;
    options.aspiration_max_retries = config.search.aspiration_max_retries;
    options.use_tt = config.tt.enabled;
    options.tt_clear_each_move = config.tt.clear_each_move;
    options.tt_age_enabled = config.tt.age_enabled;
    options.killer_enabled = config.move_ordering.killer_enabled;
    options.history_enabled = config.move_ordering.history_enabled;
    options.history_decay_interval = config.move_ordering.history_decay_interval;
    options.tt_move_ordering_enabled = config.move_ordering.tt_move_ordering_enabled;
    options.immediate_win_ordering_enabled = config.move_ordering.immediate_win_ordering_enabled;
    options.prevent_loss_ordering_enabled = config.move_ordering.prevent_loss_ordering_enabled;
    options.capture_ordering_enabled = config.move_ordering.capture_ordering_enabled;
    options.lion_reduction_ordering_enabled = config.move_ordering.lion_reduction_ordering_enabled;
    options.try_threat_ordering_enabled = config.move_ordering.try_threat_ordering_enabled;
    options.mask_collapse_ordering_enabled = config.move_ordering.mask_collapse_ordering_enabled;
    if (config.search.l_eq_enabled && config.search.l_eq_trigger_enabled) {
        qas::enable_successor_equivalence(options,
                                          config.search.l_eq_min_legal_count,
                                          config.search.l_eq_require_duplicate_hand_hint);
        options.reducer_min_depth = config.search.l_eq_min_depth_remaining;
        options.reducer_min_duplicate_ratio = config.search.l_eq_min_duplicate_ratio;
        options.reducer_disable_low_time = config.safety.disable_l_eq_when_low_time;
    }
    return options;
}

/// @brief Runs the blocking JSON-line contest protocol loop.
/// @param config Runtime engine configuration.
/// @return Zero at end-of-input or one after a protocol error.
int run_protocol(const qas::EngineConfig& config) {
    const auto resources =
        qas::estimate_resources(config, sizeof(qas::TTEntry), 16, config.tt.max_size_mb);
    const std::size_t entries = std::max<std::size_t>(1, resources.tt_entry_count);
    qas::AlphaBetaEngine engine(entries);
    qas::SearchOptions options = search_options(config);
    if (config.instrumentation.stderr_log_enabled) {
        std::cerr << "profile=" << config.profile << " tt_mb=" << resources.tt_size_mb
                  << " tt_entries=" << entries << '\n';
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const qas::ProtocolMessage message = qas::parse_protocol_message(line);
            if (message.command == qas::ProtocolCommand::EndGame) {
                std::cout << "\"OK\"" << std::endl;
                continue;
            }
            const auto result = engine.find_best_move(message.state, options);
            int action = result.has_move ? qas::encode_action_move(result.best_move) : -1;
            if (!qas::action_is_allowed(message, action)) {
                action = -1;
                for (const qas::Move& move : qas::generate_legal_moves(message.state)) {
                    const int candidate = qas::encode_action_move(move);
                    if (qas::action_is_allowed(message, candidate)) {
                        action = candidate;
                        break;
                    }
                }
            }
            if (!qas::action_is_allowed(message, action)) {
                for (int candidate = 0; candidate < qas::external_action_count; ++candidate) {
                    if (qas::action_is_allowed(message, candidate)) {
                        action = candidate;
                        break;
                    }
                }
            }
            if (!qas::action_is_allowed(message, action)) {
                throw std::runtime_error("get_action has no allowed action");
            }
            std::cout << action << std::endl;
        } catch (const std::exception& exception) {
            std::cerr << "protocol error: " << exception.what() << '\n';
            return 1;
        }
    }
    return 0;
}

}  // namespace

/// @brief Dispatches contest-protocol and development command-line modes.
/// @param argc Number of command-line arguments.
/// @param argv Command-line argument vector.
/// @return Process exit status.
int main(int argc, char** argv) {
    std::string profile_override;
    std::string config_path = "engine_config.json";
    std::size_t tt_override = 0;
    bool benchmark_override = false;
    bool config_override = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--profile" && index + 1 < argc)
            profile_override = argv[++index];
        else if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
            config_override = true;
        } else if (argument == "--tt-size-mb" && index + 1 < argc)
            tt_override = std::stoull(argv[++index]);
        else if (argument == "--benchmark-mode" && index + 1 < argc)
            benchmark_override = std::stoi(argv[++index]) != 0;
    }
    qas::EngineConfig runtime_config;
    try {
        runtime_config = qas::load_engine_config(config_path, profile_override);
    } catch (const std::exception& exception) {
        std::cerr << "config error: " << exception.what() << '\n';
        return 1;
    }
    if (tt_override != 0) {
        runtime_config.tt.size_mb = tt_override;
        runtime_config.tt.auto_size_enabled = false;
    }
    if (benchmark_override)
        runtime_config.instrumentation.benchmark_mode_enabled = true;
    if (argc == 1 || !profile_override.empty() || config_override || tt_override != 0 ||
        benchmark_override) {
        return run_protocol(runtime_config);
    }
    const std::string mode = argv[1];
    if (mode == "protocol") {
        if (argc >= 3 && argv[2][0] != '-') {
            runtime_config.search.soft_time_limit_ms = std::stoi(argv[2]);
            runtime_config.search.hard_time_limit_ms =
                runtime_config.search.soft_time_limit_ms + 1000;
        }
        if (argc >= 4 && argv[3][0] != '-')
            runtime_config.search.max_depth = std::stoi(argv[3]);
        return run_protocol(runtime_config);
    }
    if (mode == "demo") {
        demo(std::cout);
        return 0;
    }
    if (mode == "solve") {
        return solve(std::cin, std::cout, std::cerr);
    }
    if (mode == "engine-demo") {
        const qas::State state = qas::initial_state();
        qas::write_state(state, std::cout);
        std::cout << "legal " << qas::generate_legal_moves(state).size() << '\n';
        qas::AlphaBetaEngine engine;
        qas::SearchOptions options;
        options.max_depth = 5;
        options.time_limit_ms = 1000;
        print_search_result(engine.find_best_move(state, options), std::cout);
        return 0;
    }
    if (mode == "search" || mode == "search-leq") {
        qas::SearchOptions options;
        if (argc >= 3)
            options.time_limit_ms = std::stoi(argv[2]);
        if (argc >= 4)
            options.max_depth = std::stoi(argv[3]);
        if (mode == "search-leq") {
            const std::size_t threshold =
                argc >= 5 ? static_cast<std::size_t>(std::stoul(argv[4])) : 12;
            qas::enable_successor_equivalence(options, threshold);
        }
        try {
            const qas::State state = qas::parse_state(std::cin);
            qas::AlphaBetaEngine engine;
            print_search_result(engine.find_best_move(state, options), std::cout);
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "invalid state: " << exception.what() << '\n';
            return 1;
        }
    }
    if (mode == "legal") {
        try {
            const qas::State state = qas::parse_state(std::cin);
            for (const qas::Move& move : qas::generate_legal_moves(state)) {
                std::cout << qas::encode_action_move(move) << ' ' << qas::move_string(move) << '\n';
            }
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "invalid state: " << exception.what() << '\n';
            return 1;
        }
    }
    print_usage(std::cerr, argv[0]);
    return 1;
}

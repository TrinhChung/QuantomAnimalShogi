#include "core/quantum_state.hpp"
#include "search/alpha_beta.hpp"
#include "io/protocol.hpp"
#include "exact/equivalence.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using qas::Animal;
using qas::QuantumState;
using qas::Side;

std::size_t piece_index(int one_based) {
    if (one_based < 1 || one_based > 4) {
        throw std::invalid_argument("piece must be in [1, 4]");
    }
    return static_cast<std::size_t>(one_based - 1);
}

Side parse_side(const std::string& value) {
    if (value == "S" || value == "s" || value == "SOUTH" || value == "south") {
        return Side::South;
    }
    if (value == "N" || value == "n" || value == "NORTH" || value == "north") {
        return Side::North;
    }
    throw std::invalid_argument("side must be S or N");
}

void ensure_end(std::istringstream& input) {
    std::string extra;
    if (input >> extra) {
        throw std::invalid_argument("unexpected token: " + extra);
    }
}

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
            output << "IMPOSSIBLE at operation " << operation << ": " << exception.what()
                   << '\n';
            return 2;
        } catch (const std::exception& exception) {
            error << "invalid operation " << operation << ": " << exception.what() << '\n';
            return 1;
        }
    }

    print_state(state, output);
    return 0;
}

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

void print_usage(std::ostream& output, const char* program) {
    output << "Usage: " << program
           << " <protocol|demo|solve|engine-demo|search|search-leq [milliseconds] "
              "[max-depth] [threshold]|legal>\n";
}

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

int run_protocol(int time_limit_ms, int max_depth, bool use_leq) {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const qas::ProtocolMessage message = qas::parse_protocol_message(line);
            if (message.command == qas::ProtocolCommand::EndGame) {
                std::cout << "\"OK\"" << std::endl;
                continue;
            }
            qas::AlphaBetaEngine engine;
            qas::SearchOptions options;
            options.time_limit_ms = time_limit_ms;
            options.max_depth = max_depth;
            if (use_leq) qas::enable_successor_equivalence(options);
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

int main(int argc, char** argv) {
    if (argc == 1) return run_protocol(1000, 64, true);
    const std::string mode = argv[1];
    if (mode == "protocol") {
        const int milliseconds = argc >= 3 ? std::stoi(argv[2]) : 1000;
        const int depth = argc >= 4 ? std::stoi(argv[3]) : 64;
        return run_protocol(milliseconds, depth, true);
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
        if (argc >= 3) options.time_limit_ms = std::stoi(argv[2]);
        if (argc >= 4) options.max_depth = std::stoi(argv[3]);
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
                std::cout << qas::encode_action_move(move) << ' ' << qas::move_string(move)
                          << '\n';
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

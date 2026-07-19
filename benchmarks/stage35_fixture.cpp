#include "stage35_fixture.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

#include "io/protocol.hpp"

namespace qas::benchmark {
namespace {

constexpr std::uint32_t kFixtureSeed = 0x35A5'2026U;

int uncertainty(const State& state) {
    int total = 0;
    for (Mask mask : state.mask) {
        for (; mask != 0; mask = static_cast<Mask>(mask & (mask - 1)))
            ++total;
    }
    return total;
}

int hand_piece_count(const State& state) {
    return static_cast<int>(
        std::count_if(state.pos.begin(), state.pos.end(), [](std::uint8_t position) {
            return is_hand_position(position);
        }));
}

bool has_immediate_win(const State& state, Terminal reason = Terminal::None) {
    for (const Move& move : generate_legal_moves(state)) {
        State successor = state;
        Undo undo;
        if (!apply_move(successor, move, undo))
            continue;
        if ((successor.terminal == Terminal::Catch || successor.terminal == Terminal::Try) &&
            successor.winner == side_index(state.side_to_move) &&
            (reason == Terminal::None || successor.terminal == reason)) {
            return true;
        }
    }
    return false;
}

bool play_random_nonterminal(State& state,
                             std::vector<int>& sequence,
                             std::mt19937& random,
                             int plies) {
    for (int ply = 0; ply < plies; ++ply) {
        auto moves = generate_legal_moves(state);
        std::shuffle(moves.begin(), moves.end(), random);
        bool moved = false;
        for (const Move& move : moves) {
            State successor = state;
            Undo undo;
            if (apply_move(successor, move, undo) && successor.terminal == Terminal::None) {
                sequence.push_back(encode_action_move(move));
                state = successor;
                moved = true;
                break;
            }
        }
        if (!moved)
            return false;
    }
    return true;
}

Fixture reachable_fixture(const std::string& name,
                          const std::string& category,
                          std::uint32_t seed,
                          int plies) {
    std::mt19937 random(seed);
    for (int attempt = 0; attempt < 1000; ++attempt) {
        Fixture fixture;
        fixture.name = name;
        fixture.category = category;
        fixture.seed = seed;
        fixture.state = initial_state();
        if (play_random_nonterminal(fixture.state, fixture.action_sequence, random, plies)) {
            return fixture;
        }
    }
    throw std::runtime_error("could not generate reachable fixture: " + name);
}

Fixture selected_fixture(const std::string& name,
                         const std::string& category,
                         std::uint32_t seed,
                         int minimum_ply,
                         int maximum_ply,
                         const std::function<int(const State&)>& quality,
                         int minimum_quality) {
    std::mt19937 random(seed);
    Fixture best;
    int best_quality = std::numeric_limits<int>::min();
    for (int attempt = 0; attempt < 4000; ++attempt) {
        const int plies =
            minimum_ply +
            static_cast<int>(random() % static_cast<unsigned>(maximum_ply - minimum_ply + 1));
        Fixture candidate;
        candidate.name = name;
        candidate.category = category;
        candidate.seed = seed;
        candidate.state = initial_state();
        if (!play_random_nonterminal(candidate.state, candidate.action_sequence, random, plies)) {
            continue;
        }
        const int candidate_quality = quality(candidate.state);
        if (candidate_quality > best_quality) {
            best = candidate;
            best_quality = candidate_quality;
        }
        if (candidate_quality >= minimum_quality)
            return candidate;
    }
    if (best_quality == std::numeric_limits<int>::min())
        throw std::runtime_error("could not select fixture: " + name);
    return best;
}

State safe_position(int plies, int salt) {
    State state = initial_state();
    for (int ply = 0; ply < plies; ++ply) {
        auto moves = generate_legal_moves(state);
        std::sort(moves.begin(), moves.end(), [](const Move& left, const Move& right) {
            return encode_action_move(left) < encode_action_move(right);
        });
        bool selected = false;
        for (std::size_t offset = 0; offset < moves.size(); ++offset) {
            const std::size_t index =
                (static_cast<std::size_t>(salt + ply * 7) + offset) % moves.size();
            State candidate = state;
            Undo undo;
            if (apply_move(candidate, moves[index], undo) && candidate.terminal == Terminal::None &&
                !has_immediate_win(candidate)) {
                state = candidate;
                selected = true;
                break;
            }
        }
        if (!selected)
            break;
    }
    return state;
}

State duplicate_hands_position() {
    State state = initial_state();
    for (int piece : {0, 1}) {
        state.board[state.pos[piece]] = -1;
        state.pos[piece] = static_cast<std::uint8_t>(first_hand_slot + piece);
    }
    recompute_hash(state);
    return state;
}

State near_catch_position() {
    constexpr std::uint8_t kSouthGiraffeSquare = 4;
    constexpr std::uint8_t kSouthLionSquare = 11;
    constexpr std::uint8_t kNorthLionSquare = 1;
    State state = initial_state();
    state.board.fill(-1);
    state.mask[0] = bit(Animal::Giraffe);
    state.mask[1] = bit(Animal::Chick);
    state.mask[2] = bit(Animal::Elephant);
    state.mask[3] = bit(Animal::Lion);
    state.mask[4] = bit(Animal::Lion);
    state.mask[5] = bit(Animal::Chick);
    state.mask[6] = bit(Animal::Giraffe);
    state.mask[7] = bit(Animal::Elephant);
    state.pos = {kSouthGiraffeSquare,
                 first_hand_slot,
                 first_hand_slot + 1,
                 kSouthLionSquare,
                 kNorthLionSquare,
                 external_source_count - 3,
                 external_source_count - 2,
                 external_source_count - 1};
    state.board[kSouthGiraffeSquare] = 0;
    state.board[kSouthLionSquare] = 3;
    state.board[kNorthLionSquare] = 4;
    state.side_to_move = Side::South;
    state.terminal = Terminal::None;
    state.winner = -1;
    recompute_hash(state);
    return state;
}

Fixture make_fixture(const std::string& name, const std::string& category, const State& state) {
    Fixture fixture;
    fixture.name = name;
    fixture.category = category;
    fixture.seed = kFixtureSeed;
    fixture.state = state;
    return fixture;
}

void complete_metadata(Fixture& fixture) {
    std::string error;
    if (!validate_state(fixture.state, &error)) {
        throw std::runtime_error("invalid fixture " + fixture.name + ": " + error);
    }
    if (fixture.state.hash != zobrist_hash(fixture.state)) {
        throw std::runtime_error("fixture hash mismatch: " + fixture.name);
    }
    fixture.legal_action_mask.fill(0);
    const auto legal = generate_legal_moves(fixture.state);
    fixture.expected_legal_move_count = legal.size();
    for (const Move& move : legal) {
        fixture.legal_action_mask[static_cast<std::size_t>(encode_action_move(move))] = 1;
    }
}

template <typename Container>
std::string join_numbers(const Container& values) {
    std::ostringstream output;
    bool first = true;
    for (const auto value : values) {
        if (!first)
            output << ',';
        output << +value;
        first = false;
    }
    return output.str();
}

Fixture load_fixture(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open fixture: " + path.string());
    Fixture fixture;
    std::uint64_t saved_hash = 0;
    std::size_t saved_legal_count = 0;
    std::string saved_action_mask;
    std::ostringstream state_text;
    std::string line;
    bool reading_state = false;
    while (std::getline(input, line)) {
        if (line == "state_begin") {
            reading_state = true;
            continue;
        }
        if (line == "state_end")
            break;
        if (reading_state) {
            state_text << line << '\n';
            continue;
        }
        const auto separator = line.find(' ');
        const std::string key = line.substr(0, separator);
        const std::string value = separator == std::string::npos ? "" : line.substr(separator + 1);
        if (key == "name")
            fixture.name = value;
        else if (key == "category")
            fixture.category = value;
        else if (key == "seed")
            fixture.seed = static_cast<std::uint32_t>(std::stoul(value));
        else if (key == "zobrist_hash")
            saved_hash = std::stoull(value);
        else if (key == "expected_legal_move_count")
            saved_legal_count = std::stoull(value);
        else if (key == "legal_action_mask")
            saved_action_mask = value;
        else if (key == "move_sequence" && !value.empty()) {
            std::istringstream actions(value);
            std::string action;
            while (std::getline(actions, action, ',')) {
                fixture.action_sequence.push_back(std::stoi(action));
            }
        }
    }
    std::istringstream state_input(state_text.str());
    fixture.state = parse_state(state_input);
    complete_metadata(fixture);
    if (fixture.state.hash != saved_hash ||
        fixture.expected_legal_move_count != saved_legal_count) {
        throw std::runtime_error("fixture metadata mismatch: " + fixture.name);
    }
    std::string actual_mask;
    actual_mask.reserve(external_action_count);
    for (std::uint8_t enabled : fixture.legal_action_mask)
        actual_mask += enabled ? '1' : '0';
    if (actual_mask != saved_action_mask) {
        throw std::runtime_error("fixture action mask mismatch: " + fixture.name);
    }
    return fixture;
}

}  // namespace

std::vector<Fixture> generate_stage35_fixtures() {
    std::vector<Fixture> fixtures;
    fixtures.push_back(make_fixture("initial", "required", initial_state()));
    fixtures.push_back(make_fixture("safe_4ply", "required", safe_position(4, 1)));
    fixtures.push_back(make_fixture("safe_8ply", "required", safe_position(8, 3)));
    fixtures.push_back(make_fixture("duplicate_hands", "required", duplicate_hands_position()));
    State near_draw = initial_state();
    near_draw.turn = 252;
    recompute_hash(near_draw);
    fixtures.push_back(make_fixture("near_draw", "required", near_draw));
    fixtures.push_back(make_fixture("near_catch", "required", near_catch_position()));

    Fixture near_try = selected_fixture(
        "near_try",
        "required",
        kFixtureSeed + 1U,
        4,
        32,
        [](const State& state) { return has_immediate_win(state, Terminal::Try) ? 1 : 0; },
        1);
    fixtures.push_back(near_try);
    Fixture many_hands = selected_fixture(
        "many_hands",
        "required",
        kFixtureSeed + 2U,
        8,
        48,
        [](const State& state) { return has_immediate_win(state) ? -1 : hand_piece_count(state); },
        3);
    fixtures.push_back(many_hands);
    Fixture high_uncertainty = selected_fixture(
        "high_uncertainty_midgame", "required", kFixtureSeed + 3U, 8, 20, uncertainty, 24);
    fixtures.push_back(high_uncertainty);
    Fixture low_uncertainty = selected_fixture(
        "low_uncertainty_midgame",
        "required",
        kFixtureSeed + 4U,
        12,
        48,
        [](const State& state) { return has_immediate_win(state) ? -1 : 40 - uncertainty(state); },
        28);
    fixtures.push_back(low_uncertainty);

    const std::array<std::pair<const char*, int>, 3> ply_groups{
        {{"random_ply4", 4}, {"random_ply8", 8}, {"random_ply16", 16}}};
    for (const auto& [category, plies] : ply_groups) {
        for (int index = 0; index < 20; ++index) {
            std::ostringstream name;
            name << category << '_' << std::setw(2) << std::setfill('0') << index;
            fixtures.push_back(reachable_fixture(
                name.str(), category, kFixtureSeed + 100U * plies + index, plies));
        }
    }
    for (int index = 0; index < 20; ++index) {
        std::ostringstream name;
        name << "random_hands_" << std::setw(2) << std::setfill('0') << index;
        fixtures.push_back(selected_fixture(
            name.str(),
            "random_hands",
            kFixtureSeed + 4000U + index,
            6,
            48,
            [](const State& state) {
                return has_immediate_win(state) ? -1 : hand_piece_count(state);
            },
            1));
    }
    for (int index = 0; index < 20; ++index) {
        std::ostringstream name;
        name << "random_low_uncertainty_" << std::setw(2) << std::setfill('0') << index;
        fixtures.push_back(selected_fixture(
            name.str(),
            "random_low_uncertainty",
            kFixtureSeed + 5000U + index,
            12,
            48,
            [](const State& state) {
                return has_immediate_win(state) ? -1 : 40 - uncertainty(state);
            },
            24));
    }

    std::set<std::string> names;
    for (Fixture& fixture : fixtures) {
        if (!names.insert(fixture.name).second) {
            throw std::runtime_error("duplicate fixture name: " + fixture.name);
        }
        complete_metadata(fixture);
    }
    return fixtures;
}

void save_stage35_fixtures(const std::vector<Fixture>& fixtures, const std::string& directory) {
    const std::filesystem::path root(directory);
    std::filesystem::create_directories(root);
    std::ofstream manifest(root / "manifest.txt");
    if (!manifest)
        throw std::runtime_error("cannot write fixture manifest");
    for (const Fixture& fixture : fixtures) {
        manifest << fixture.name << ".fixture\n";
        std::ofstream output(root / (fixture.name + ".fixture"));
        if (!output)
            throw std::runtime_error("cannot write fixture: " + fixture.name);
        output << "name " << fixture.name << '\n'
               << "category " << fixture.category << '\n'
               << "seed " << fixture.seed << '\n'
               << "side_to_move " << (fixture.state.side_to_move == Side::South ? 'S' : 'N') << '\n'
               << "turn_count " << fixture.state.turn << '\n'
               << "board " << join_numbers(fixture.state.board) << '\n'
               << "pieces " << join_numbers(fixture.state.pos) << '\n'
               << "masks " << join_numbers(fixture.state.mask) << '\n'
               << "owner_bits " << static_cast<unsigned>(fixture.state.owner_bits) << '\n'
               << "origin_bits " << static_cast<unsigned>(fixture.state.origin_bits) << '\n'
               << "hands ";
        bool first_hand = true;
        for (int piece = 0; piece < physical_piece_count; ++piece) {
            if (!is_hand_position(fixture.state.pos[piece]))
                continue;
            if (!first_hand)
                output << ',';
            output << piece << ':' << static_cast<int>(fixture.state.pos[piece] - first_hand_slot);
            first_hand = false;
        }
        output << '\n'
               << "zobrist_hash " << fixture.state.hash << '\n'
               << "legal_action_mask_source generated_internal\n"
               << "legal_action_mask ";
        for (std::uint8_t enabled : fixture.legal_action_mask)
            output << (enabled ? '1' : '0');
        output << '\n'
               << "expected_legal_move_count " << fixture.expected_legal_move_count << '\n'
               << "move_sequence ";
        for (std::size_t index = 0; index < fixture.action_sequence.size(); ++index) {
            if (index != 0)
                output << ',';
            output << fixture.action_sequence[index];
        }
        output << "\nstate_begin\n";
        write_state(fixture.state, output);
        output << "state_end\n";
    }
}

std::vector<Fixture> load_stage35_fixtures(const std::string& directory) {
    const std::filesystem::path root(directory);
    std::ifstream manifest(root / "manifest.txt");
    if (!manifest)
        throw std::runtime_error("cannot open fixture manifest: " + directory);
    std::vector<Fixture> fixtures;
    std::string file;
    while (std::getline(manifest, file)) {
        if (!file.empty())
            fixtures.push_back(load_fixture(root / file));
    }
    if (fixtures.empty())
        throw std::runtime_error("fixture manifest is empty");
    return fixtures;
}

}  // namespace qas::benchmark

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "book/forbidden_moves.hpp"
#include "book/position_book.hpp"
#include "io/protocol.hpp"
#include "search/guarded_search.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

qas::State parse(const std::string& text) {
    std::istringstream input(text);
    return qas::parse_state(input);
}

// The same position written with two different physical-piece numberings and hand slots.
const char* kNumberingA =
    "S 20\n0 N S 10 E\n1 S S 6 L\n2 S S H2 CG\n3 S S H0 CG\n4 N N 5 L\n5 S N H3 G\n6 N N 2 CE\n7 N "
    "N 4 CE\n";
const char* kNumberingB =
    "S 20\n0 S S H5 CG\n1 S S 6 L\n2 N S 10 E\n3 S S H1 CG\n4 N N 4 CE\n5 N N 2 CE\n6 S N H0 G\n7 "
    "N N 5 L\n";

// Serializes a state as the official observation the environment would send (protocol view:
// the side to move is "mine"; origin flags are absolute).
std::string observation_json(const qas::State& state) {
    std::array<std::array<int, qas::kObservationFieldCount>, qas::external_source_count> entries{};
    for (int piece = 0; piece < qas::physical_piece_count; ++piece) {
        auto& entry = entries[state.pos[piece]];
        for (qas::Animal form : qas::all_forms)
            entry[static_cast<std::size_t>(form)] = qas::contains(state.mask[piece], form) ? 1 : 0;
        const bool north_origin = qas::originated_from(state, piece, qas::Side::North);
        entry[5] = north_origin ? 0 : 1;
        entry[6] = north_origin ? 1 : 0;
        const bool mine = qas::owned_by(state, piece, qas::Side::South);
        entry[7] = mine ? 1 : 0;
        entry[8] = mine ? 0 : 1;
    }
    std::ostringstream output;
    output << "{\"command\":\"get_action\",\"observation\":{\"observation\":[";
    for (int source = 0; source < qas::external_source_count; ++source) {
        output << (source ? "," : "") << '[';
        for (std::size_t field = 0; field < qas::kObservationFieldCount; ++field)
            output << (field ? "," : "") << entries[source][field];
        output << ']';
    }
    output << "],\"action_mask\":[";
    for (int action = 0; action < qas::external_action_count; ++action)
        output << (action ? "," : "") << 1;
    output << "],\"turn\":" << state.turn << "}}";
    return output.str();
}

void test_key_ignores_piece_numbering() {
    const qas::State a = parse(kNumberingA);
    const qas::State b = parse(kNumberingB);
    check(qas::PositionBook::key(a) == qas::PositionBook::key(b),
          "book key is independent of physical piece numbering and hand slots");
    qas::State later = a;
    later.turn = 21;
    check(qas::PositionBook::key(later) != qas::PositionBook::key(a), "book key includes the turn");
}

void test_key_matches_protocol_built_state() {
    const qas::State a = parse(kNumberingA);
    const qas::State from_protocol = qas::parse_protocol_message(observation_json(a)).state;
    check(qas::PositionBook::key(from_protocol) == qas::PositionBook::key(a),
          "book key of a parsed state equals the key of the protocol-built state");
}

void test_drop_entry_resolves_for_any_numbering() {
    const qas::State a = parse(kNumberingA);
    const qas::State b = parse(kNumberingB);
    qas::Move drop{};
    for (const qas::Move& move : qas::generate_legal_moves(a)) {
        if (qas::is_hand_position(move.from) &&
            a.mask[move.piece] == (qas::bit(qas::Animal::Chick) | qas::bit(qas::Animal::Giraffe))) {
            drop = move;
            break;
        }
    }
    check(drop.valid(), "fixture has a duplicate-hand drop");
    qas::PositionBook book;
    check(book.add(qas::PositionBook::key(a), qas::PositionBook::encode_move(a, drop)),
          "entry is accepted");
    const auto found = book.lookup(b);
    check(found.has_value() && found->to == drop.to && qas::is_hand_position(found->from) &&
              b.mask[found->piece] == a.mask[drop.piece],
          "drop entry resolves to an equivalent legal drop under another numbering");
}

void test_illegal_or_unknown_entries_are_ignored() {
    const qas::State a = parse(kNumberingA);
    qas::PositionBook book;
    check(book.add(qas::PositionBook::key(a), "b0-11"), "syntactically valid entry is accepted");
    check(!book.lookup(a).has_value(), "an illegal stored move is never returned");
    check(!book.add(qas::PositionBook::key(a), "x1-2"), "malformed move text is rejected");
    check(!book.lookup(qas::initial_state()).has_value(), "unknown positions miss the book");
}

void test_load_file_with_comments() {
    const qas::State state = qas::initial_state();
    const auto legal = qas::generate_legal_moves(state);
    const std::string path = "qas_book_test.txt";
    {
        std::ofstream output(path);
        output << "# comment line\n\n"
               << qas::PositionBook::key(state) << ' '
               << qas::PositionBook::encode_move(state, legal.back()) << "  # trailing comment\n";
    }
    qas::PositionBook book;
    std::string error;
    check(book.load(path, &error) && error.empty() && book.size() == 1, "book file loads");
    std::remove(path.c_str());
    const auto found = book.lookup(state);
    check(found.has_value() && *found == legal.back(),
          "loaded entry returns the stored legal move");
    qas::PositionBook missing;
    check(!missing.load("does_not_exist_book.txt"), "a missing file reports failure");
}

void test_forbidden_moves_are_excluded() {
    const qas::State state = qas::initial_state();
    const auto legal = qas::generate_legal_moves(state);
    const std::string path = "qas_forbidden_test.txt";
    {
        std::ofstream output(path);
        output << "# forbidden list\n";
        for (std::size_t i = 0; i + 1 < legal.size(); ++i)
            output << qas::PositionBook::key(state) << ' '
                   << qas::PositionBook::encode_move(state, legal[i]) << '\n';
    }
    qas::ForbiddenMoves forbidden;
    std::string error;
    check(forbidden.load(path, &error) && error.empty() && forbidden.size() == 1,
          "forbidden list loads");
    std::remove(path.c_str());
    const auto excluded = forbidden.lookup(state);
    check(excluded.size() + 1 == legal.size(), "every listed legal move is returned");
    check(forbidden.lookup(parse(kNumberingA)).empty(),
          "unlisted positions have no forbidden moves");

    qas::SearchOptions options;
    options.max_depth = 4;
    options.time_limit_ms = options.hard_time_limit_ms = options.soft_time_limit_ms = 1'500;
    options.root_excluded = excluded;
    qas::AlphaBetaEngine engine(1U << 16U);
    const auto result = qas::guarded_find_best_move(engine, state, options, qas::GuardOptions{});
    check(result.has_move && result.best_move == legal.back(),
          "the guarded search plays the only non-forbidden move");
}

}  // namespace

int main() {
    test_key_ignores_piece_numbering();
    test_key_matches_protocol_built_state();
    test_drop_entry_resolves_for_any_numbering();
    test_illegal_or_unknown_entries_are_ignored();
    test_load_file_with_comments();
    test_forbidden_moves_are_excluded();
    if (failures != 0) {
        std::cerr << failures << " position-book test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All position-book tests passed\n";
    return EXIT_SUCCESS;
}

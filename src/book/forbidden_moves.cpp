#include "book/forbidden_moves.hpp"

#include <fstream>
#include <sstream>

#include "book/position_book.hpp"

namespace qas {

void ForbiddenMoves::add(const std::string& key, const std::string& move) {
    entries_[key].insert(move);
}

bool ForbiddenMoves::load(const std::string& path, std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr)
            *error = "cannot open forbidden-moves file: " + path;
        return false;
    }
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        std::istringstream fields(line);
        std::string key;
        std::string move;
        if (!(fields >> key))
            continue;
        if (!(fields >> move)) {
            if (error != nullptr && error->empty())
                *error = "line " + std::to_string(line_number) + ": missing move";
            continue;
        }
        add(key, move);
    }
    return true;
}

std::vector<Move> ForbiddenMoves::lookup(const State& state) const {
    std::vector<Move> result;
    const auto found = entries_.find(PositionBook::key(state));
    if (found == entries_.end())
        return result;
    for (const Move& move : generate_legal_moves(state)) {
        if (found->second.count(PositionBook::encode_move(state, move)) != 0)
            result.push_back(move);
    }
    return result;
}

}  // namespace qas

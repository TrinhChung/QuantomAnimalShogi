#include "search/dfpn.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace qas {
namespace {

/// @brief Adds proof numbers without overflowing past a cap.
std::uint32_t saturating_add(std::uint32_t left, std::uint32_t right, std::uint32_t cap) {
    const std::uint64_t sum = static_cast<std::uint64_t>(left) + right;
    return sum >= cap ? cap : static_cast<std::uint32_t>(sum);
}

/// @brief Tests whether a node is proven or disproven.
bool solved(std::uint32_t pn, std::uint32_t dn) {
    return pn == 0 || dn == 0;
}

constexpr std::uint64_t kDeadlineCheckMask = 1023;

}  // namespace

DfpnSolver::DfpnSolver(std::size_t table_entries) {
    if (table_entries < 2 || (table_entries & (table_entries - 1)) != 0)
        throw std::invalid_argument("df-pn table size must be a power of two");
    table_.resize(table_entries);
    mask_ = table_entries - 1;
}

void DfpnSolver::reset(Side attacker, int max_ply) {
    std::fill(table_.begin(), table_.end(), Slot{});
    attacker_ = attacker;
    max_ply_ = max_ply;
}

bool DfpnSolver::win_for_attacker(const State& state) const {
    return (state.terminal == Terminal::Catch || state.terminal == Terminal::Try) &&
           state.winner == side_index(attacker_);
}

void DfpnSolver::get(std::uint64_t key, std::uint32_t& pn, std::uint32_t& dn) const {
    const std::size_t first = key & mask_;
    for (const std::size_t index : {first, first ^ 1U}) {
        const Slot& slot = table_[index];
        if (slot.key == key) {
            pn = slot.pn;
            dn = slot.dn;
            return;
        }
    }
    pn = 1;
    dn = 1;
}

void DfpnSolver::put(std::uint64_t key, std::uint32_t pn, std::uint32_t dn, std::uint32_t work) {
    const std::size_t first = key & mask_;
    const std::size_t second = first ^ 1U;
    Slot* target = nullptr;
    for (const std::size_t index : {first, second}) {
        if (table_[index].key == key) {
            target = &table_[index];
            work = std::max(work, target->work);
            break;
        }
    }
    if (target == nullptr) {
        Slot& a = table_[first];
        Slot& b = table_[second];
        if (a.key == 0) {
            target = &a;
        } else if (b.key == 0) {
            target = &b;
        } else {
            // Proven entries (proof trees) outrank disproven ones, which outrank unsolved; then
            // work.
            const auto value = [](const Slot& slot) {
                const std::uint64_t tier = slot.pn == 0 ? 2U : slot.dn == 0 ? 1U : 0U;
                return (tier << 40U) + slot.work;
            };
            target = value(a) <= value(b) ? &a : &b;
        }
    }
    target->key = key;
    target->pn = pn;
    target->dn = dn;
    target->work = work;
}

void DfpnSolver::mid(const State& state,
                     std::uint32_t threshold_pn,
                     std::uint32_t threshold_dn,
                     int ply) {
    const std::uint64_t start_nodes = nodes_;
    ++nodes_;
    if ((nodes_ & kDeadlineCheckMask) == 0 &&
        (Clock::now() >= deadline_ ||
         (stop_ != nullptr && stop_->load(std::memory_order_relaxed)))) {
        aborted_ = true;
    }
    if (aborted_ || nodes_ > node_cap_) {
        aborted_ = true;
        return;
    }
    if (state.terminal != Terminal::None) {
        win_for_attacker(state) ? put(state.hash, 0, kInfinity, 1)
                                : put(state.hash, kInfinity, 0, 1);
        return;
    }
    if (ply >= max_ply_) {
        put(state.hash, kInfinity, 0, 1);  // not provable inside the horizon
        return;
    }

    const bool or_node = state.side_to_move == attacker_;
    const auto work = [&]() { return static_cast<std::uint32_t>(nodes_ - start_nodes); };
    std::vector<Move> pseudo;
    generate_pseudo_legal_moves(state, pseudo);
    std::vector<Child> kids;
    kids.reserve(pseudo.size());
    for (const Move& move : pseudo) {
        Child kid;
        kid.move = move;
        kid.state = state;
        Undo undo;
        if (!apply_move(kid.state, move, undo))
            continue;
        if (kid.state.terminal != Terminal::None) {
            const bool win = win_for_attacker(kid.state);
            if (or_node && win) {  // the attacker wins at once
                put(state.hash, 0, kInfinity, work());
                return;
            }
            if (!or_node && !win) {  // the defender escapes at once (draw or its own win)
                put(state.hash, kInfinity, 0, work());
                return;
            }
            if (or_node)
                continue;  // a non-winning terminal is a dead attacker option
            put(kid.state.hash, 0, kInfinity, 1);
        }
        kids.push_back(std::move(kid));
    }
    if (kids.empty()) {
        // An attacker without useful moves cannot prove; a defender without legal moves loses.
        or_node ? put(state.hash, kInfinity, 0, 1) : put(state.hash, 0, kInfinity, 1);
        return;
    }

    while (true) {
        std::uint32_t pn = or_node ? kInfinity : 0;
        std::uint32_t dn = or_node ? 0 : kInfinity;
        std::size_t best = 0;
        std::uint32_t best_value = kInfinity + 1;
        std::uint32_t second_value = kInfinity;
        std::uint32_t best_pn = 1;
        std::uint32_t best_dn = 1;
        for (std::size_t index = 0; index < kids.size(); ++index) {
            std::uint32_t child_pn = 1;
            std::uint32_t child_dn = 1;
            get(kids[index].state.hash, child_pn, child_dn);
            if (or_node) {
                pn = std::min(pn, child_pn);
                dn = saturating_add(dn, child_dn, kInfinity);
            } else {
                pn = saturating_add(pn, child_pn, kInfinity);
                dn = std::min(dn, child_dn);
            }
            const std::uint32_t key_value = or_node ? child_pn : child_dn;
            if (key_value < best_value) {
                second_value = best_value;
                best_value = key_value;
                best = index;
                best_pn = child_pn;
                best_dn = child_dn;
            } else if (key_value < second_value) {
                second_value = key_value;
            }
        }
        put(state.hash, pn, dn, work());
        if (pn >= threshold_pn || dn >= threshold_dn || solved(pn, dn) || aborted_)
            return;
        // 1+epsilon trick: the selected child may run slightly past the second-best value.
        const std::uint32_t second = std::min<std::uint32_t>(second_value, kInfinity - 1);
        const auto widened = static_cast<std::uint32_t>(std::min<double>(
            kInfinity - 1, std::ceil(static_cast<double>(second) * (1.0 + kEpsilon))));
        std::uint32_t child_pn = 0;
        std::uint32_t child_dn = 0;
        if (or_node) {
            child_pn = std::min(threshold_pn, std::max(widened, second + 1));
            child_dn = threshold_dn >= kInfinity ? kInfinity : threshold_dn - dn + best_dn;
        } else {
            child_dn = std::min(threshold_dn, std::max(widened, second + 1));
            child_pn = threshold_pn >= kInfinity ? kInfinity : threshold_pn - pn + best_pn;
        }
        mid(kids[best].state, child_pn, child_dn, ply + 1);
    }
}

DfpnResult DfpnSolver::prove(const State& root,
                             Clock::time_point deadline,
                             std::uint64_t node_cap,
                             const std::atomic<bool>* stop) {
    nodes_ = 0;
    node_cap_ = node_cap;
    deadline_ = deadline;
    stop_ = stop;
    aborted_ = false;
    DfpnResult result;
    if (root.terminal != Terminal::None) {
        result.status = win_for_attacker(root) ? ProofStatus::Proven : ProofStatus::Disproven;
        return result;
    }
    std::uint32_t pn = 1;
    std::uint32_t dn = 1;
    get(root.hash, pn, dn);
    while (!aborted_ && !solved(pn, dn)) {
        mid(root, kInfinity, kInfinity, 0);
        get(root.hash, pn, dn);
    }
    result.nodes = nodes_;
    if (pn == 0) {
        result.status = ProofStatus::Proven;
        if (root.side_to_move == attacker_) {
            for (const Move& move : generate_legal_moves(root)) {
                State child = root;
                Undo undo;
                apply_move(child, move, undo);
                std::uint32_t child_pn = 1;
                std::uint32_t child_dn = 1;
                get(child.hash, child_pn, child_dn);
                if (win_for_attacker(child) ||
                    (child.terminal == Terminal::None && child_pn == 0)) {
                    result.winning_move = move;
                    break;
                }
            }
        }
    } else if (dn == 0) {
        result.status = ProofStatus::Disproven;
    }
    return result;
}

long long DfpnSolver::verify_proof(const State& root) {
    std::unordered_map<std::uint64_t, bool> checked;
    long long count = 0;
    std::function<bool(const State&, int)> verify = [&](const State& state, int ply) -> bool {
        if (state.terminal != Terminal::None)
            return win_for_attacker(state);
        if (ply >= max_ply_)
            return false;
        const auto memo = checked.find(state.hash);
        if (memo != checked.end())
            return memo->second;
        ++count;
        bool ok = false;
        const auto legal = generate_legal_moves(state);
        if (state.side_to_move == attacker_) {
            for (const Move& move : legal) {
                State child = state;
                Undo undo;
                if (!apply_move(child, move, undo))
                    continue;
                std::uint32_t child_pn = 1;
                std::uint32_t child_dn = 1;
                get(child.hash, child_pn, child_dn);
                const bool marked =
                    child.terminal != Terminal::None ? win_for_attacker(child) : child_pn == 0;
                if (marked && verify(child, ply + 1)) {
                    ok = true;
                    break;
                }
            }
        } else {
            ok = true;  // a defender without legal moves loses
            for (const Move& move : legal) {
                State child = state;
                Undo undo;
                if (!apply_move(child, move, undo) || !verify(child, ply + 1)) {
                    ok = false;
                    break;
                }
            }
        }
        checked[state.hash] = ok;
        return ok;
    };
    return verify(root, 0) ? count : -1;
}

}  // namespace qas

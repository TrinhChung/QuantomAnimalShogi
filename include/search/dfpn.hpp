#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rules/game.hpp"

namespace qas {

/// @brief Proof outcome from the attacker's point of view.
enum class ProofStatus : std::int8_t { Disproven = -1, Unknown = 0, Proven = 1 };

/// @brief Result of one proof-number search call.
struct DfpnResult {
    ProofStatus status{ProofStatus::Unknown};
    std::uint64_t nodes{0};
    Move winning_move{};  ///< First attacker move of a proof when the attacker is to move.
};

/// @brief Full-width depth-first proof-number search for forced Catch/Try wins.
/// @note Uses a fixed two-way transposition table that prefers proven, then disproven, then
///       expensive entries; the 1+epsilon threshold trick; one transition per child expansion with
///       immediate win/escape short-circuits; and a wall-clock deadline plus an external stop flag,
///       so it can run on a worker thread. The turn is hashed, so the expanded graph is a DAG and
///       positions with equal hashes share the same distance from the root.
class DfpnSolver {
   public:
    using Clock = std::chrono::steady_clock;

    /// @brief Allocates the transposition table.
    /// @param table_entries Power-of-two entry count.
    /// @throws std::invalid_argument If the entry count is not a power of two of at least two.
    explicit DfpnSolver(std::size_t table_entries = std::size_t{1} << 21U);

    /// @brief Clears the table and fixes the attacker and proof horizon for subsequent calls.
    /// @param attacker Side trying to force a win.
    /// @param max_ply Maximum plies from each searched root.
    void reset(Side attacker, int max_ply);

    /// @brief Searches until solved, the deadline, the node cap or the stop flag; keeps the table.
    /// @param root Root position; every root of one reset must share the same turn.
    /// @param deadline Wall-clock limit.
    /// @param node_cap Maximum nodes for this call.
    /// @param stop Optional external stop flag.
    /// @return Proof status, node count and, for an attacker-to-move proof, a winning move.
    DfpnResult prove(const State& root,
                     Clock::time_point deadline,
                     std::uint64_t node_cap = ~std::uint64_t{0},
                     const std::atomic<bool>* stop = nullptr);

    /// @brief Re-checks a proof using only rule functions.
    /// @param root Root of a previously proven search.
    /// @return Distinct proof-tree positions checked, or -1 when the stored proof is incomplete
    ///         (for example after table eviction); -1 never means the proof was unsound.
    long long verify_proof(const State& root);

   private:
    struct Slot {
        std::uint64_t key{0};
        std::uint32_t pn{1};
        std::uint32_t dn{1};
        std::uint32_t work{0};
    };
    struct Child {
        Move move{};
        State state{};
    };
    static constexpr std::uint32_t kInfinity = 100'000'000;
    static constexpr double kEpsilon = 0.25;

    std::vector<Slot> table_;
    std::size_t mask_{0};
    Side attacker_{Side::South};
    int max_ply_{23};
    std::uint64_t nodes_{0};
    std::uint64_t node_cap_{0};
    Clock::time_point deadline_{};
    const std::atomic<bool>* stop_{nullptr};
    bool aborted_{false};

    /// @brief Reads proof and disproof numbers, defaulting to (1, 1) for unknown keys.
    void get(std::uint64_t key, std::uint32_t& pn, std::uint32_t& dn) const;

    /// @brief Stores numbers, evicting the less valuable way when both ways hold other keys.
    void put(std::uint64_t key, std::uint32_t pn, std::uint32_t dn, std::uint32_t work);

    /// @brief Multiple-iterative-deepening expansion of one node under thresholds.
    void mid(const State& state, std::uint32_t threshold_pn, std::uint32_t threshold_dn, int ply);

    /// @brief Tests whether a terminal state is a Catch or Try win for the attacker.
    bool win_for_attacker(const State& state) const;
};

}  // namespace qas

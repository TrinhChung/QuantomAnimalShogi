# Function Design Rules

## Size and Responsibility

- One function has one responsibility and one abstraction level.
- Prefer functions under 40 nonblank lines. At more than 40, review for extraction; more than 80 requires splitting or a written review justification.
- A function must not both generate moves and evaluate a position, nor both apply a move and choose the best move.
- Keep branching rule-specific: extract helpers when a function has more than roughly five independent decision branches.

Bad: `find_move(State&)` mutates state, generates moves, evaluates them, and prints an action.

Good: `generate_legal_moves`, `apply_move`, `evaluate`, and `encode_action_index` are separate operations.

## Mutation and Interfaces

- Queries accept `const` references and have no observable mutation.
- Mutating names state the action: `apply_move`, `undo_move`, `clear`, `update_hash`.
- `apply_move` must have a matching `undo_move`; the undo record explicitly contains all overwritten information.
- Mutation-heavy functions document preconditions, postconditions, and preserved invariants next to the declaration.
- Avoid boolean parameter traps. Use `enum class ValidationMode { Checked, Unchecked };` rather than `apply(move, true)`.
- Prefer at most four logical parameters. Group cohesive parameters into a small named struct; do not hide unrelated dependencies in a context bag.
- Use an explicit result type (`std::optional`, expected-like project type, or named `Result`) when failure is expected. Assertions are for programmer-invariant violations, not input errors.
- Mark important results `[[nodiscard]]`; use `noexcept` only when the contract is true.

## Doxygen Documentation

- Every named production function, constructor, destructor, and operator has one canonical
  Doxygen comment written in English.
- Document public and class-member functions at their declaration in the owning header. Document
  internal-only helpers at their definition. Do not duplicate the same contract at an out-of-line
  definition.
- Use `/// @brief` for the purpose, `@param` for every parameter, `@tparam` for template
  parameters, `@return` for non-void results, and `@throws` for exceptions that are part of the
  contract.
- Use `@pre`, `@post`, `@note`, or `@warning` only for meaningful invariants, ordering constraints,
  side effects, or hazards. Do not narrate individual implementation statements.
- Local lambdas do not require separate Doxygen blocks unless they represent an independently
  reusable contract; document their behavior through the owning function instead.
- Update the Doxygen contract in the same patch whenever parameters, return semantics, mutation,
  failure behavior, or invariants change.

Good:

```cpp
/// @brief Applies a legal move and records the complete prior state.
/// @param state State to mutate.
/// @param move Move to apply.
/// @param undo Receives the prior state.
/// @return `true` when the transition succeeds.
bool apply_move(State& state, const Move& move, Undo& undo);
```

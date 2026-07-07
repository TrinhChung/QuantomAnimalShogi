# Project Principles

## Priority Order

1. Correct official behavior.
2. Deterministic, testable state transitions.
3. Clear module ownership and data flow.
4. Measured performance.

Never trade a higher priority for a lower one without an explicit design decision and tests.

## Enforceable Rules

- Use C++17-compatible code unless the build configuration explicitly moves the whole project to C++20.
- Keep mutable state owned by an object or stack frame; no hidden mutable globals or function-local mutable statics.
- Represent domain concepts with domain types, not unexplained integers.
- Make invalid states difficult to construct and impossible to ignore silently.
- Complex rules require focused unit tests plus at least one integration or invariant test.
- New features must extend the module structure rather than bypass it.
- Warnings introduced by a patch are defects. Formatting and static-analysis policy, once configured, applies to new code.

Bad: a global `current_position` read by rules and search.

Good: pass `const State&` to queries and `State&` only to named mutation operations.

## Scope Control

Implement the simplest correct baseline before adding caches, compact encodings, or specialized search. Record deferred ideas in an issue or a dated, owned TODO; do not leave speculative infrastructure in production code.

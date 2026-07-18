# Development Guide for Agents and Contributors

This file is the entry point for all changes to Quantum Animal Shogi.

## Before Coding

1. Read every file in `docs/00_PROJECT_PRINCIPLES.md` through `docs/10_BUILD_RULES.md`.
2. Identify the owning module and its permitted dependencies.
3. Find or add tests that define the behavior being changed.
4. Keep the patch limited to one coherent purpose.

Do not implement a feature before understanding its module boundary. Do not mix rule logic, search logic, and protocol/IO logic. Do not add machine-learning or neural-network code during the classical-engine phase.

## Required Working Style

- Use English identifiers and the naming rules in `docs/01_NAMING_RULES.md`.
- Format C++ files with the repository `.clang-format`; editor defaults are defined in `.editorconfig`.
- Prefer small patches, explicit data flow, and deterministic behavior.
- Do not change public behavior without tests that state the intended change.
- Preserve existing tests during refactors; add characterization tests first when behavior is unclear.
- Update these documents in the same patch when architecture or an invariant changes.
- Do not write official protocol output anywhere except the IO boundary. Diagnostics go to `stderr` or a controlled logger.
- When unsure, choose correctness and clarity over cleverness.

## Definition of Done

A change is done only when it builds, relevant tests pass, new behavior is tested, invariants and hashing remain valid, and the checklist in `docs/09_CODE_REVIEW_CHECKLIST.md` is completed.

Before review, format changed C++ files from the repository root:

```powershell
clang-format -i <changed-file.cpp> <changed-file.hpp>
```

To check a file without modifying it:

```powershell
clang-format --dry-run --Werror <changed-file.cpp>
```

Create or update builds only through the versioned build layout:

```powershell
scripts\build_version.ps1 -Version current -Configuration Release
```

Bad: adding move generation, evaluation, and protocol formatting in one patch.

Good: adding a tested core move type, then adding its IO conversion in a separate patch.

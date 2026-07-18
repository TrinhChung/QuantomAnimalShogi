# Build Layout Rules

All generated CMake build trees belong under one repository-level `build/` directory.
Every build tree must have a stable version name:

```text
build/
|-- current/          Reusable build for the active working tree
|-- <version>/        Explicit candidate or historical build
`-- legacy-default/   Preserved pre-versioned build artifacts
```

## Required Commands

Use the repository entry point for normal configure, build, and test work:

```powershell
scripts\build_version.ps1 -Version current -Configuration Release
scripts\build_version.ps1 -Version stage5-1 -Configuration Release
```

The default `current` directory is reused on subsequent builds. Creating `build_<name>`,
`cmake-build-*`, or another root-level build directory is not allowed. Direct CMake usage must
still name a version directory, for example `cmake -S . -B build/current`.

Version names use lowercase letters, digits, dots, underscores, and hyphens, start with a letter
or digit, and remain stable for the lifetime of that build tree.

## Historical Builds

Historical build trees are preserved under `build/<version>` and are never deleted merely to
adopt this layout. A moved CMake tree can contain absolute paths from its original location, so it
is an artifact archive rather than a supported incremental build. Create a fresh version directory
when a reproducible rebuild is required.

Frozen accepted engine binaries remain owned by `evaluation/versions/`; they are not CMake build
trees and must not be moved into `build/`.

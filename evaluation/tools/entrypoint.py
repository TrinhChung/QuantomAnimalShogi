from __future__ import annotations

import sys

from .run_pipeline import main


def dispatch() -> int:
    if len(sys.argv) < 3:
        print("entrypoint requires 'resume RUN_DIRECTORY' or 'registered VERSION_ID'", file=sys.stderr)
        return 2
    mode, value, *remaining = sys.argv[1:]
    if mode == "resume":
        return main(["--resume", value, *remaining])
    if mode == "registered":
        return main(["--registered-candidate", value, *remaining])
    print(f"unknown entrypoint mode: {mode}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(dispatch())

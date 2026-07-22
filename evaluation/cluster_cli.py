from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import UTC, datetime
from pathlib import Path
from typing import Any


STAGE5_CLEAN_COMMIT = "5e096227947cf53c760a027318e26183863483f3"


def _git_commit() -> str:
    root = Path(__file__).resolve().parents[1]
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    return completed.stdout.strip()


def _request(arguments: argparse.Namespace, method: str, route: str, value: Any = None) -> Any:
    body = json.dumps(value).encode("utf-8") if value is not None else None
    request = urllib.request.Request(
        f"{arguments.master_url.rstrip('/')}{route}",
        data=body,
        method=method,
        headers={
            "authorization": f"Bearer {arguments.token}",
            "content-type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        print(error.read().decode("utf-8", errors="replace"), file=sys.stderr)
        raise


def _idempotency_key(kind: str, commit: str, profile: str) -> str:
    timestamp = datetime.now(UTC).strftime("%Y%m%dT%H%M%S%fZ")
    return f"cli:{kind}:{commit[:12]}:{profile}:{timestamp}"


def _enqueue(arguments: argparse.Namespace) -> int:
    commit = arguments.commit or _git_commit()
    payload: dict[str, Any] = {"profile": arguments.profile}
    if arguments.kind != "git_update":
        payload.update(
            {
                "candidateName": arguments.candidate_name,
                "candidateVersionId": arguments.candidate_version_id,
                "changeCategory": arguments.change_category,
                "opponents": arguments.opponents.split(",") if arguments.opponents else [],
                "seed": arguments.seed,
            }
        )
    if arguments.kind == "tournament":
        payload.update(
            {
                "opponentGitCommit": arguments.opponent_commit,
                "opponentName": arguments.opponent_name,
            }
        )
    result = _request(
        arguments,
        "POST",
        "/api/cluster/jobs",
        {
            "idempotencyKey": arguments.idempotency_key
            or _idempotency_key(arguments.kind, commit, arguments.profile),
            "kind": arguments.kind,
            "priority": arguments.priority,
            "gitCommit": commit,
            "gitRemote": arguments.git_remote,
            "payload": payload,
            "requirements": {"labels": dict(arguments.label)},
            "requestedBy": arguments.requested_by,
            "maxAttempts": arguments.max_attempts,
        },
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def _state(arguments: argparse.Namespace) -> int:
    result = _request(arguments, "GET", f"/api/cluster/state?limit={arguments.limit}")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def _cancel(arguments: argparse.Namespace) -> int:
    result = _request(arguments, "POST", f"/api/cluster/jobs/{arguments.job_id}/cancel", {})
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def _label(value: str) -> tuple[str, str]:
    key, separator, item = value.partition("=")
    if not separator or not key or not item:
        raise argparse.ArgumentTypeError("label must use KEY=VALUE")
    return key, item


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Control the distributed QAS evaluation cluster")
    parser.add_argument("--master-url", default=os.environ.get("QAS_CLUSTER_URL"))
    parser.add_argument("--token", default=os.environ.get("QAS_CLUSTER_TOKEN"))
    subparsers = parser.add_subparsers(dest="command", required=True)

    enqueue = subparsers.add_parser("enqueue")
    enqueue.add_argument("kind", choices=["git_update", "benchmark", "tournament", "evaluation"])
    enqueue.add_argument("--commit")
    enqueue.add_argument("--git-remote", default="origin")
    enqueue.add_argument("--profile", default="none")
    enqueue.add_argument("--candidate-name", default="Cluster candidate")
    enqueue.add_argument("--candidate-version-id")
    enqueue.add_argument("--change-category", default="performance_only")
    enqueue.add_argument("--opponents", default="")
    enqueue.add_argument("--opponent-commit", default=STAGE5_CLEAN_COMMIT)
    enqueue.add_argument("--opponent-name", default="stage5-clean")
    enqueue.add_argument("--priority", type=int)
    enqueue.add_argument("--seed", type=int, default=0x51415335)
    enqueue.add_argument("--label", type=_label, action="append", default=[])
    enqueue.add_argument("--requested-by", default="chungtrinh2k2@gmail.com")
    enqueue.add_argument("--max-attempts", type=int, default=2)
    enqueue.add_argument("--idempotency-key")
    enqueue.set_defaults(handler=_enqueue)

    state = subparsers.add_parser("state")
    state.add_argument("--limit", type=int, default=100)
    state.set_defaults(handler=_state)

    cancel = subparsers.add_parser("cancel")
    cancel.add_argument("job_id")
    cancel.set_defaults(handler=_cancel)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if not arguments.master_url or not arguments.token:
        parser.error("QAS_CLUSTER_URL and QAS_CLUSTER_TOKEN (or explicit options) are required")
    if arguments.command == "enqueue" and arguments.priority is None:
        arguments.priority = {
            "git_update": 50,
            "benchmark": 100,
            "evaluation": 150,
            "tournament": 200,
        }[arguments.kind]
    return arguments.handler(arguments)


if __name__ == "__main__":
    raise SystemExit(main())

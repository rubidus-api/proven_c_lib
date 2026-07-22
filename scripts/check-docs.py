#!/usr/bin/env python3
"""Mechanical documentation checks for proven_c_lib."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REQUIRED = [
    "docs/operations/README.md",
    "docs/tests/README.md",
    "docs/tests/test-index.md",
    "docs/tests/cases/T001-project-operations.md",
    "docs/tests/cases/T002-operating-file-roles.md",
    "scripts/project-check.sh",
]

REQUIRED_TEXT = {
    "docs/operations/README.md": [
        "`SPEC.md` as the local current behavior, architecture, and layout contract",
        "`REQUIREMENTS.md`, when present, as the local current accepted requirements",
        "`docs/tests/test-index.md` as a compact process/TDD catalog",
        "`BACKLOGS.md` is the single compact queue",
        "`CONTEXT.md` (`## Resume Packet`) is the resume state",
    ],
    "docs/tests/test-index.md": [
        "docs/tests/cases/T001-project-operations.md",
        "docs/tests/cases/T002-operating-file-roles.md",
    ],
}


def fail(message: str) -> None:
    print(f"check-docs: FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def candidate_files() -> list[Path]:
    out = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        text=True,
    )
    return [ROOT / line for line in out.splitlines() if line]


def main() -> None:
    for rel in REQUIRED:
        if not (ROOT / rel).exists():
            fail(f"missing {rel}")

    for rel, snippets in REQUIRED_TEXT.items():
        text = (ROOT / rel).read_text(encoding="utf-8", errors="ignore")
        for snippet in snippets:
            if snippet not in text:
                fail(f"missing required guidance in {rel}: {snippet}")

    private_pattern = re.compile(
        r"(/op" + r"t/data(?:/|$)|/m" + r"nt(?:/|$)|"
        r"/ho" + r"me/hermes(?:/|$)|/Us" + r"ers/hermes(?:/|$)|"
        r"github-personal-access-" + r"token|ssh -" + r"i|"
        r"BEGIN\s+[A-Z0-9\s]*PRI" + r"VATE\s+KEY)"
    )
    for path in candidate_files():
        if path.suffix.lower() not in {".c", ".h", ".md", ".txt", ".py", ".sh"}:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if private_pattern.search(text):
            fail(f"private path or key-like pattern in {path.relative_to(ROOT)}")

    print("check-docs: ok")


if __name__ == "__main__":
    main()

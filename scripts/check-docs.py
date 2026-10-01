#!/usr/bin/env python3
"""Mechanical documentation checks for proven_c_lib."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# The operating docs are the maintainers' and live in the private sibling; a clean public clone
# has none, and then these checks are skipped rather than failed.
PRIVATE = ROOT.parent / (ROOT.name + "_private")

# Paths that belong to the private sibling and must never be tracked here again: design RFCs,
# benchmark records and measurement programs, the backlog, process docs and maintainer scripts.
PRIVATE_ONLY = [
    r"^docs/RFC-", r"^docs/rfc", r"^docs/b0\d\d-", r"^docs/benchmarks?/", r"^docs/BACKLOG\.md$",
    r"^docs/REPORT\.md$", r"^docs/case-study", r"^docs/TESTING\.md$", r"^docs/DOCUMENTING\.md$",
    r"^docs/operations/", r"^docs/tests/", r"^docs/primitives-benchmark\.md$", r"^docs/checks/",
    r"^docs/reviews/", r"^CHECKLIST\.md$", r"^GEMINI\.md$", r"^scripts/win11kd-", r"^scripts/build-b0",
    r"^scripts/build-rfc-",
]

REQUIRED = [
    "docs/operations/README.md",
    "docs/tests/README.md",
    "docs/tests/test-index.md",
    "docs/tests/cases/T001-project-operations.md",
    "docs/tests/cases/T002-operating-file-roles.md",
    "scripts/project-check.sh",
]
PUBLIC_REQUIRED = {"scripts/project-check.sh"}

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
    def locate(rel: str) -> Path | None:
        for base in (ROOT, PRIVATE):
            if (base / rel).exists():
                return base / rel
        return None

    tracked = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).splitlines()
    for rel in tracked:
        for pattern in PRIVATE_ONLY:
            if re.search(pattern, rel):
                fail(f"{rel} belongs in ../{PRIVATE.name}/, not in the public repository")

    have_private_docs = locate("docs/operations/README.md") is not None
    if not have_private_docs:
        print("check-docs: note: operating docs not found here or in ../%s; skipping their checks" % PRIVATE.name)
    for rel in REQUIRED:
        if rel not in PUBLIC_REQUIRED and not have_private_docs:
            continue
        if locate(rel) is None:
            fail(f"missing {rel}")

    for rel, snippets in REQUIRED_TEXT.items():
        path = locate(rel)
        if path is None:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        for snippet in snippets:
            if snippet not in text:
                fail(f"missing required guidance in {rel}: {snippet}")

    private_pattern = re.compile(
        r"(/op" + r"t/data(?:/|$)|/m" + r"nt(?:/|$)|"
        r"/ho" + r"me(?:/|$)|/Us" + r"ers/hermes(?:/|$)|"
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

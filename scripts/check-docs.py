#!/usr/bin/env python3
"""Mechanical documentation checks for proven_c_lib."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# The operating docs are the maintainers' and live in the internal `ops/` directory, which the
# public repository does not carry; a clean public clone has none, and then these checks are
# skipped rather than failed. Paths below are relative to that directory.
PRIVATE = ROOT / "ops"
# Tracked here, never published: the internal directory and the agents' own files.
INTERNAL_PREFIXES = ("ops/", ".agents/")
INTERNAL_FILES = {"AGENTS.md", "CLAUDE.md", "public.manifest", ".gitignore.public"}


def is_internal(rel: str) -> bool:
    return rel.startswith(INTERNAL_PREFIXES) or rel in INTERNAL_FILES

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
    "operations/README.md",
    "tests/README.md",
    "tests/test-index.md",
    "tests/cases/T001-project-operations.md",
    "tests/cases/T002-operating-file-roles.md",
    "scripts/project-check.sh",
]

REQUIRED_TEXT = {
    "operations/README.md": [
        "`SPEC.md` as the local current behavior, architecture, and layout contract",
        "`REQUIREMENTS.md`, when present, as the local current accepted requirements",
        "`docs/tests/test-index.md` as a compact process/TDD catalog",
        "`BACKLOGS.md` is the single compact queue",
        "`CONTEXT.md` (`## Resume Packet`) is the resume state",
    ],
    "tests/test-index.md": [
        "docs/tests/cases/T001-project-operations.md",
        "docs/tests/cases/T002-operating-file-roles.md",
    ],
}


def fail(message: str) -> None:
    print(f"check-docs: FAIL: {message}", file=sys.stderr)
    sys.exit(1)


LINK_THEN_PAREN = re.compile(r"\]\([^()\s]*\)\(")


def link_then_paren(text: str) -> int | None:
    """The line of a Markdown link followed directly by `(`, outside code; None when there is none.

    `[Chapter 14](manual-14-tls.md)(section 3)` reads as a link and a remark, and the site's
    converter takes the second parenthesis for part of the first: the build of the Korean
    edition stopped on one at a release. A space between the two is all it takes.
    """
    fenced = False
    for number, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        if LINK_THEN_PAREN.search(re.sub(r"`[^`]*`", "``", line)):
            return number
    return None


def candidate_files() -> list[Path]:
    out = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        text=True,
    )
    return [ROOT / line for line in out.splitlines() if line and not is_internal(line)]


def main() -> None:
    tracked = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).splitlines()
    for rel in tracked:
        if is_internal(rel):
            continue
        for pattern in PRIVATE_ONLY:
            if re.search(pattern, rel):
                fail(f"{rel} belongs in {PRIVATE.name}/, not in the public tree")

    have_private_docs = (PRIVATE / "operations/README.md").exists()
    if not have_private_docs:
        print(f"check-docs: note: no {PRIVATE.name}/ here (a public clone); skipping its checks")
    else:
        for rel in REQUIRED:
            if not (PRIVATE / rel).exists():
                fail(f"missing {PRIVATE.name}/{rel}")

    for rel, snippets in REQUIRED_TEXT.items():
        path = PRIVATE / rel
        if not path.exists():
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

    for path in candidate_files():
        if path.suffix.lower() != ".md" or not path.exists():
            continue
        line = link_then_paren(path.read_text(encoding="utf-8", errors="ignore"))
        if line is not None:
            fail(f"{path.relative_to(ROOT)}:{line}: a Markdown link followed directly by '(' - put a space between them")

    print("check-docs: ok")


if __name__ == "__main__":
    main()

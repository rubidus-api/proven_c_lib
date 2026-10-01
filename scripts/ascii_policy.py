#!/usr/bin/env python3
"""The English-ASCII policy for public text (AGENTS.md; B-036).

Public English documentation and every source comment are English ASCII. Korean text is allowed
where it is the point - the Korean edition, links to it, glossary pairs, tests that hold Korean
words - so Hangul is exempt; every other non-ASCII byte is not.

    scripts/ascii_policy.py check       fail listing each offending file:line (project-check runs this)
    scripts/ascii_policy.py normalize   rewrite the in-scope files to ASCII, updating Markdown
                                        anchors whose headings changed

Scope: README.md, TEST.md, CHANGELOG.md, the English manual and its English examples, and the
C sources, headers and build files. Out of scope, deliberately: the Korean mirrors
(manual-ko/, manual/examples/ko/, README-ko.md, *-ko.md), the generated site (docs/en, docs/ko),
the historical design records in docs/*.md, and scripts/ (the site generator carries the Korean
edition's interface strings).
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

REPLACE = [
    (" — ", " - "), ("—", " - "),   # em dash
    ("–", "-"),                          # en dash
    ("§§", "sections "), ("§", "section "),
    ("→", "->"), ("↑", "up-arrow"),
    ("…", "..."),
    ("×", "x"),
    (" · ", " | "), ("·", "*"),
    ("²", "^2"),
    ("−", "-"),
    ("±", "+/-"),
    ("★", "*"),
    ("σ", " sigma"),
    ("ï", "i"),
    ("\U0001f4d6 ", ""), ("\U0001f4d6", ""),
    ("‘", "'"), ("’", "'"), ("“", '"'), ("”", '"'),
]


def in_scope(path: str) -> bool:
    if path in ("README.md", "TEST.md", "CHANGELOG.md", "nob.c", "build_headers.inc", "build_tests.inc", "build_sources.inc"):
        return True
    if path.startswith("manual/examples/ko/") or path.startswith("manual-ko/") or "-ko." in path:
        return False
    if path.startswith("manual/") and path.endswith((".md", ".c", ".h")):
        return True
    if path.startswith(("include/", "src/", "platform/", "tests/")) and path.endswith((".c", ".h", ".inc")):
        return True
    return False


def hangul(ch: str) -> bool:
    o = ord(ch)
    return 0xAC00 <= o <= 0xD7A3 or 0x1100 <= o <= 0x11FF or 0x3130 <= o <= 0x318F


def tracked() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [p for p in out.split("\n") if p and in_scope(p)]


def slug(heading: str) -> str:
    """GitHub-style anchor, as the site builder and the manual's links use it."""
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s, flags=re.UNICODE)
    return s.replace(" ", "-")


def check() -> int:
    bad = 0
    for path in tracked():
        for n, line in enumerate((ROOT / path).read_text(encoding="utf-8").split("\n"), 1):
            chars = sorted({c for c in line if ord(c) > 127 and not hangul(c)})
            if chars:
                bad += 1
                if bad <= 40:
                    print(f"{path}:{n}: non-ASCII {' '.join(repr(c) for c in chars)}", file=sys.stderr)
    if bad:
        print(f"ascii-policy: FAIL: {bad} line(s) outside English ASCII (Hangul excepted); "
              f"run scripts/ascii_policy.py normalize or write ASCII", file=sys.stderr)
        return 1
    print("ascii-policy: ok")
    return 0


def normalize() -> int:
    anchors: dict[str, str] = {}
    changed = 0
    for path in tracked():
        p = ROOT / path
        text = p.read_text(encoding="utf-8")
        # An em dash at the start of a line would become " - ", which Markdown reads as a list
        # item; at the end of a line it would leave trailing whitespace. Both become "--".
        new = re.sub(r"(?m)^\u2014 ?", "-- ", text)
        new = re.sub(r"(?m) ?\u2014$", " --", new)
        for a, b in REPLACE:
            new = new.replace(a, b)
        if path.endswith(".md"):
            for old_line, new_line in zip(text.split("\n"), new.split("\n")):
                if old_line != new_line and old_line.startswith("#"):
                    h_old = old_line.lstrip("#").strip()
                    h_new = new_line.lstrip("#").strip()
                    if slug(h_old) != slug(h_new):
                        anchors[slug(h_old)] = slug(h_new)
        if new != text:
            p.write_text(new, encoding="utf-8")
            changed += 1
    # Links anywhere in the Markdown sources to a heading whose anchor moved.
    if anchors:
        for p in list(ROOT.glob("*.md")) + list((ROOT / "manual").glob("*.md")) + list((ROOT / "manual-ko").glob("*.md")) + list((ROOT / "docs").glob("*.md")):
            t = p.read_text(encoding="utf-8")
            u = t
            for old, new in anchors.items():
                u = u.replace("#" + old + ")", "#" + new + ")")
            if u != t:
                p.write_text(u, encoding="utf-8")
    print(f"ascii-policy: normalised {changed} file(s); {len(anchors)} heading anchor(s) moved")
    for old, new in anchors.items():
        print(f"  #{old} -> #{new}")
    return 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    sys.exit(check() if cmd == "check" else normalize() if cmd == "normalize" else 2)

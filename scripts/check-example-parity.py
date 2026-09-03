#!/usr/bin/env python3
"""The two example trees must differ only in their comments.

`manual/examples/en/` and `manual/examples/ko/` hold the same programs: the
English edition prints the English-commented copy, the Korean edition prints the
Korean-commented one. That is only honest if the *code* is identical - otherwise
a reader of one edition is being shown a program the other edition never ran,
and the two would drift the moment somebody fixed a bug in one tree.

So this strips comments and string contents from both files and compares what is
left. It is deliberately crude: it does not parse C, it removes comments and
normalises whitespace, which is enough to catch an edit that changed a call, a
constant, or a control flow while translating a comment around it.

    check-example-parity.py            report
    check-example-parity.py --check    exit 1 if the trees disagree
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EN = ROOT / "manual" / "examples" / "en"
KO = ROOT / "manual" / "examples" / "ko"


def code_only(text):
    """Comments out, string bodies blanked, whitespace flattened."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c in '"\'':
            quote, j = c, i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == '\\' else 1
            out.append(quote + quote)          # keep that a literal was here
            i = j + 1
        else:
            out.append(c)
            i += 1
    return re.sub(r"\s+", " ", "".join(out)).strip()


def main():
    if not EN.is_dir() or not KO.is_dir():
        print("check-example-parity: both example trees must exist")
        return 1

    en = {p.name for p in EN.glob("*.c")}
    ko = {p.name for p in KO.glob("*.c")}
    problems = []

    for name in sorted(en - ko):
        problems.append(f"only in en/: {name}")
    for name in sorted(ko - en):
        problems.append(f"only in ko/: {name}")

    same = 0
    for name in sorted(en & ko):
        a = code_only((EN / name).read_text(encoding="utf-8"))
        b = code_only((KO / name).read_text(encoding="utf-8"))
        if a == b:
            same += 1
        else:
            problems.append(f"code differs (not only comments): {name}")

    if "--check" in sys.argv and problems:
        for p in problems:
            print(f"  ⚠️  {p}")
        print(f"check-example-parity: {len(problems)} problem(s) --- "
              "the two trees may differ in comments only")
        return 1

    print(f"check-example-parity: {same} program(s) identical in code, "
          "differing only in comments")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

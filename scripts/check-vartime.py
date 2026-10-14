#!/usr/bin/env python3
"""Variable-time code is reachable from verification alone.

Signature verification handles nothing secret, and src/proven/crypto_ec.c and
src/proven/crypto_25519.c verify with functions that branch and index on the values they are
given - fast, and correct only for public values. Those functions are named `*_vartime`. This
check fails when anything but verification can reach one:

  - every `*_vartime` function is `static`, and defined in one of the files below;
  - every use of one is a direct call, inside a function that is itself `*_vartime`, one of
    the verification functions, or one of the suite's hooks (`proven_crypto_*_test_*`);
  - no hook is called from the library: hooks are for tests/;
  - the same holds in the object code, compiled without inlining (when a compiler and objdump
    are there; a skip says so).

It refuses to pass on nothing: each file must define a `*_vartime` function, and each
verification function must call one.

    python3 scripts/check-vartime.py
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = ("src/proven/crypto_ec.c", "src/proven/crypto_25519.c")
VERIFY = {
    "src/proven/crypto_ec.c": "proven_crypto_ecdsa_verify",
    "src/proven/crypto_25519.c": "proven_crypto_ed25519_verify",
}
SCANNED = ("src", "platform", "include")
VARTIME = re.compile(r"\b(\w+_vartime)\b")
HOOK = re.compile(r"\bproven_crypto_\w+_test_\w+\b")
IDENT = re.compile(r"[A-Za-z_]\w*")


def strip(text: str) -> str:
    """Comments and string and character literals blanked, newlines kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + c if j - i >= 2 else text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def preprocessor(text: str) -> list[tuple[int, str]]:
    """(offset, line) of each preprocessor line, continuations joined."""
    found = []
    for m in re.finditer(r"(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", text):
        found.append((m.start(), m.group(0)))
    return found


def blank(text: str, spans: list[tuple[int, str]]) -> str:
    chars = list(text)
    for at, line in spans:
        for i in range(at, at + len(line)):
            if chars[i] != "\n":
                chars[i] = " "
    return "".join(chars)


def functions(text: str):
    """(name, name offset, body start, body end) of each function definition."""
    found = []
    depth = 0
    candidate = None              # the first identifier followed by "(" since the last ; or }
    current = None
    for m in re.finditer(r"[{};]|[A-Za-z_]\w*(?=\s*\()", text):
        tok = m.group(0)
        if tok == "{":
            if depth == 0:
                current = None
                if candidate and text[:m.start()].rstrip().endswith(")"):
                    current = (candidate[0], candidate[1], m.end())
            depth += 1
        elif tok == "}":
            depth -= 1
            if depth == 0:
                if current is not None:
                    found.append((current[0], current[1], current[2], m.start()))
                current = None
                candidate = None
        elif tok == ";":
            if depth == 0:
                candidate = None
        elif depth == 0 and candidate is None:
            candidate = (tok, m.start())
    return found


def enclosing(funcs, pos):
    for name, _, a, b in funcs:
        if a <= pos < b:
            return name
    return None


def allowed_caller(name: str | None, verify: set[str]) -> bool:
    return name is not None and (name.endswith("_vartime") or name in verify or HOOK.fullmatch(name) is not None)


def source_check(errors: list[str]) -> set[str]:
    defined: dict[str, str] = {}
    verify = set(VERIFY.values())
    calls_from_verify: dict[str, int] = {v: 0 for v in verify}
    for top in SCANNED:
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix not in (".c", ".h") or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            text = strip(path.read_text(encoding="utf-8", errors="replace"))
            pp = preprocessor(text)
            for at, line in pp:
                for m in VARTIME.finditer(line):
                    errors.append(f"{rel}:{text.count(chr(10), 0, at) + 1}: {m.group(1)} named in a preprocessor line")
            text = blank(text, pp)
            funcs = functions(text)
            names_at = {at for _, at, _, _ in funcs}
            for name, _, a, _ in funcs:
                if name.endswith("_vartime"):
                    head_start = max(text.rfind(";", 0, a), text.rfind("}", 0, a)) + 1
                    head = text[head_start:a]
                    if rel not in FILES:
                        errors.append(f"{rel}: {name} is defined outside {', '.join(FILES)}")
                    if not re.search(r"\bstatic\b", head):
                        errors.append(f"{rel}: {name} is not static")
                    defined[name] = rel
            for m in VARTIME.finditer(text):
                where = enclosing(funcs, m.start())
                line = text.count("\n", 0, m.start()) + 1
                after = text[m.end():].lstrip()
                if where is None:
                    if m.start() not in names_at:
                        errors.append(f"{rel}:{line}: {m.group(1)} named outside a function body (a declaration or a pointer)")
                    continue
                if not after.startswith("("):
                    errors.append(f"{rel}:{line}: {m.group(1)} used other than as a direct call (in {where})")
                elif not allowed_caller(where, verify):
                    errors.append(f"{rel}:{line}: {where} calls {m.group(1)}; only verification may")
                elif where in calls_from_verify:
                    calls_from_verify[where] += 1
            if rel.startswith("src/"):
                for m in HOOK.finditer(text):
                    where = enclosing(funcs, m.start())
                    if where is not None:
                        line = text.count("\n", 0, m.start()) + 1
                        errors.append(f"{rel}:{line}: {where} calls the suite's hook {m.group(0)}")
    for rel in FILES:
        if rel not in defined.values():
            errors.append(f"{rel}: no *_vartime function found - the check would pass on nothing")
    for v, count in calls_from_verify.items():
        if count == 0:
            errors.append(f"{v} calls no *_vartime function - the check would pass on nothing")
    return set(defined)


def object_check(errors: list[str], names: set[str]) -> str:
    cc = os.environ.get("CC", "cc")
    objdump = shutil.which("objdump")
    if not shutil.which(cc) or not objdump:
        return "object code: SKIP (no compiler or no objdump)"
    verify = set(VERIFY.values())
    seen = 0
    with tempfile.TemporaryDirectory() as tmp:
        for rel in FILES:
            obj = Path(tmp) / (Path(rel).stem + ".o")
            build = subprocess.run([cc, "-std=c2x", "-O0", "-fno-inline", "-I", str(ROOT / "include"), "-I", str(ROOT / "platform"),
                                    "-I", str(ROOT / "src/proven"), "-c", str(ROOT / rel), "-o", str(obj)],
                                   capture_output=True, text=True)
            if build.returncode != 0:
                # A compiler that refuses these flags (the build has a fallback for -std=c2x;
                # this does not) is no evidence either way: the source reading above stands.
                first = (build.stderr.strip().splitlines() or ["no message"])[0][:160]
                return f"object code: SKIP ({cc} would not compile {rel} with -std=c2x -O0 -fno-inline: {first})"
            dump = subprocess.run([objdump, "-d", "-r", "--no-show-raw-insn", str(obj)], capture_output=True, text=True)
            if dump.returncode != 0:
                return "object code: SKIP (objdump could not read the object)"
            current = None
            for line in dump.stdout.splitlines():
                head = re.match(r"^[0-9a-f]+ <([\w.]+)>:$", line)
                if head:
                    current = head.group(1)
                    continue
                for ref in re.findall(r"<(\w+)(?:[+-]0x[0-9a-f]+)?>|R_\w+\s+(\w+)", line):
                    sym = ref[0] or ref[1]
                    if sym in names and sym != current:
                        seen += 1
                        if not allowed_caller(current, verify):
                            errors.append(f"{rel} (object code): {current} refers to {sym}; only verification may")
    if seen == 0:
        errors.append("object code: no reference to a *_vartime function found - the check would pass on nothing")
    return f"object code: {seen} references read"


def main() -> int:
    errors: list[str] = []
    names = source_check(errors)
    note = object_check(errors, names) if names else "object code: not read"
    if errors:
        for e in errors:
            print(f"check-vartime: {e}", file=sys.stderr)
        return 1
    print(f"check-vartime: ok - {len(names)} variable-time functions, reachable from verification only; {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

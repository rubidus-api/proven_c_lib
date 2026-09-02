#!/usr/bin/env python3
"""Re-copy every quoted example body into the chapters that quote it.

The example files under manual/examples/ are the source of truth: the build
compiles and runs them. A chapter quotes one with

    <!-- example: manual/examples/ex_01_errors.c -->
    ```c
    ...body...
    ```

and tests/test_docs_manual_examples.c fails the build when the two differ. This
script performs the copy so nobody has to do it by hand, in every manual
directory given (default: manual/ and manual-ko/).
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = re.compile(r'(<!-- example: (manual/examples/[A-Za-z0-9_./-]+\.c) -->\n```c\n)(.*?)(```)', re.S)

def body(path):
    src = open(path, encoding='utf-8').read()
    i = src.find('#include "example.h"')
    if i < 0:
        raise SystemExit(f'{path}: no #include "example.h"')
    rest = src[src.index('\n', i) + 1:]
    return rest.lstrip('\n')

def sync(md_dir):
    changed = []
    d = os.path.join(ROOT, md_dir)
    for name in sorted(os.listdir(d)):
        if not name.endswith('.md'):
            continue
        p = os.path.join(d, name)
        text = open(p, encoding='utf-8').read()
        def repl(m):
            return m.group(1) + body(os.path.join(ROOT, m.group(2))) + m.group(4)
        new = MARKER.sub(repl, text).rstrip('\n') + '\n'
        if new != text:
            open(p, 'w', encoding='utf-8').write(new)
            changed.append(md_dir + '/' + name)
    return changed

if __name__ == '__main__':
    dirs = sys.argv[1:] or ['manual', 'manual-ko']
    changed = [c for d in dirs for c in sync(d)]
    print('\n'.join(changed) if changed else 'all quoted examples already match')

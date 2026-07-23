# Manual examples

Every example printed in `manual/` lives here as a real program.

The manual used to carry ~190 fenced C blocks and **only four of them could be
compiled at all** — the rest were sketches that referenced imaginary helpers
(`do_work()`, `get_view()`). An example nobody can compile is an example nobody
can check, so it rots quietly, and the reader is the one who finds out.

So the rule is now:

> **A `c` code block in the manual is either quoted verbatim from a file in this
> directory, or it is not code.** Signature listings and "not available here"
> lists are fenced as `text`, not `c`.

Two things enforce that, and both fail the build:

- `./nob` compiles and **runs** every `manual/examples/*.c`. They are ordinary
  programs, not test-harness code: they read like the code a caller would write,
  and they check their own results, returning non-zero if an assumption breaks.
- `tests/test_docs_manual_examples` re-reads the markdown and the files side by side.
  If a chapter quotes an example that no longer matches the file — or quotes a
  file that does not exist, or a file exists that no chapter quotes — the test
  fails and names the file.

## Writing one

Keep them the way a caller would write them: a `main`, an allocator, real error
handling, and a `destroy` for everything owned. Use `EXAMPLE_REQUIRE` for the
checks so a broken example fails loudly instead of printing something wrong.

```c
#include "example.h"

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();
    /* ... */
    EXAMPLE_REQUIRE(condition, "what went wrong");
    return EXAMPLE_OK();
}
```

## Quoting one from a chapter

Put the marker immediately before the fence. The path is relative to the
repository root, and the block must be the file's body verbatim (everything after
its `#include "example.h"` line):

```markdown
<!-- example: manual/examples/ex_01_errors.c -->
```

The checker takes it from there.

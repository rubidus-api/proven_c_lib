# Tutorial: the library in six short programs

**Part I — Start here.** No prerequisites beyond one introductory C book.
**After this tutorial** you can read the greeting program in
[Chapter 0](manual-00-start-here.md) line by line, and the reference chapters stop looking like a
wall of new words.

## Why this exists

Chapter 0 shows a working program on its first page. That program is thirty lines long and every
one of them is a new idea: an allocator you pass in, a result you have to check, a view that
carries its length, an append that refuses rather than truncates, a destroy paired with the
allocator that created it. If you have finished one C book and no more, five new ideas arriving
together is four too many.

So this tutorial takes them one at a time. Six programs, each a few lines longer than the last,
each introducing exactly one thing. Every one of them is a real file under `manual/examples/`
that the build compiles and runs, so what you read here is what actually ran.

Build any of them the way you build any C program — the library is source you compile with your
own code, so there is nothing to install:

```text
cc -std=c23 -Iinclude your_program.c src/proven/*.c platform/*.c -o your_program
```

If that line is unfamiliar, read [Chapter 0 §4](manual-00-start-here.md#4-building-and-including)
first and come back.

## Table of contents

1. [Lesson 1 — printing, and a build that works](#lesson-1--printing-and-a-build-that-works)
2. [Lesson 2 — text that knows how long it is](#lesson-2--text-that-knows-how-long-it-is)
3. [Lesson 3 — a call that can fail says so](#lesson-3--a-call-that-can-fail-says-so)
4. [Lesson 4 — the value and the error arrive together](#lesson-4--the-value-and-the-error-arrive-together)
5. [Lesson 5 — who gives out the memory is an argument](#lesson-5--who-gives-out-the-memory-is-an-argument)
6. [Lesson 6 — the Chapter 0 program, read line by line](#lesson-6--the-chapter-0-program-read-line-by-line)
7. [Where to go next](#where-to-go-next)

---

## Lesson 1 — printing, and a build that works

**The one new thing:** `proven_println`, and the fact that its arguments are checked.

`printf("%d", 3.5)` compiles. It is wrong, and on a bad day it prints garbage or crashes, because
the format string is a *string* — nothing connects it to the arguments that follow. `proven_println`
uses `{}` as the placeholder and wraps each argument in `PROVEN_ARG`, which records the argument's
type. The two cannot disagree, because the type travels with the value.

<!-- example: manual/examples/en/tut_01_hello.c -->
```c
/*
 * Lesson 1 - get one line to build and run.
 *
 * Nothing here is about the library's ideas yet. The only question this program
 * answers is "does my build command work?", and it is worth answering on its
 * own, because every later lesson assumes it.
 *
 * printf(fmt, ...) is not checked: the compiler cannot tell you that "%d" was
 * handed a double. proven_println checks, because every argument is wrapped by
 * PROVEN_ARG and carries its own type.
 */

int main(void) {
    proven_println("hello from proven");

    /* {} is the placeholder. The value goes through PROVEN_ARG, which records
     * what type it is, so the format string and the argument cannot disagree. */
    proven_println("one number: {}", PROVEN_ARG(42));
    proven_println("two of them: {} and {}", PROVEN_ARG(1), PROVEN_ARG(2));

    return EXAMPLE_OK();
}
```

Running it prints three lines. If it built and ran, your include path and your source list are
right, and everything after this is about the library rather than about your build.

**Common first stumble.** `{}` is not `%s`. There is no letter to get wrong, and there is nothing
to remember about `long` versus `int`: `PROVEN_ARG` knows.

---

## Lesson 2 — text that knows how long it is

**The one new thing:** the *view* — a pointer and a size that travel together.

In the C you know, a string is a pointer, and its length is wherever the first zero byte happens
to be. Every function that touches it has to walk the bytes to find out how much there is; if the
zero is missing, it walks off the end. That single design decision is behind a large share of the
security advisories in the language's history.

A view makes the missing half explicit. It does not own the bytes — it borrows them, and it never
frees anything.

<!-- example: manual/examples/en/tut_02_view.c -->
```c
/*
 * Lesson 2 - text that knows how long it is.
 *
 * In the C you already know, a string is a pointer and the length is wherever
 * the first zero byte happens to be. Every function has to walk the bytes to
 * find out how much there is, and if the zero is missing it walks off the end.
 *
 * A view is the pair that C leaves implicit: a pointer AND a size, travelling
 * together. It borrows - it does not own the bytes and never frees them.
 */

int main(void) {
    /* PROVEN_LIT makes a view from a literal. The size is computed at compile
     * time, so no scan happens here - unlike strlen. */
    proven_u8str_view_t hello = PROVEN_LIT("hello");

    EXAMPLE_REQUIRE(hello.size == 5, "the view already knows its own length");
    EXAMPLE_REQUIRE(hello.ptr != NULL, "and it points at the literal's bytes");

    /* Because the length travels with the pointer, comparing is a size check
     * plus a memcmp. No walking, and no way to run off the end. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(hello, PROVEN_LIT("hello")), "same text");
    EXAMPLE_REQUIRE(!proven_u8str_view_eq(hello, PROVEN_LIT("hell")), "shorter text differs");

    /* A view can name PART of something without copying it. Here is the middle
     * of the literal - still borrowed, still no allocation. */
    proven_u8str_view_t ell = { .ptr = hello.ptr + 1, .size = 3 };
    EXAMPLE_REQUIRE(proven_u8str_view_eq(ell, PROVEN_LIT("ell")), "a window onto the same bytes");

    proven_println("whole: {} (size {})", PROVEN_ARG(hello), PROVEN_ARG(hello.size));
    proven_println("part : {} (size {})", PROVEN_ARG(ell), PROVEN_ARG(ell.size));

    return EXAMPLE_OK();
}
```

**What to notice.** `PROVEN_LIT` computes the size at compile time, so it is not `strlen`. And the
third view in the program names the *middle* of the literal without copying it: a view can describe
part of something, which is why splitting and parsing in this library allocate nothing.

**Common first stumble.** A view is borrowed. If the bytes it points at go away — a local buffer
that leaves scope, a string you destroyed — the view is dangling, exactly like any other C pointer.
The library never extends a lifetime behind your back.

---

## Lesson 3 — a call that can fail says so

**The one new thing:** `proven_err_t`, and refusal instead of truncation.

C reports failure in three unrelated ways: a magic return value, a null pointer, or the global
`errno` that the next call overwrites. All three are easy not to look at, and nothing complains
when you do not. Here, a call that can fail returns an error *value* — and these functions are
marked `[[nodiscard]]`, so throwing it away is a compiler warning rather than a habit.

<!-- example: manual/examples/en/tut_03_error.c -->
```c
/*
 * Lesson 3 - a call that can fail says so, in its return value.
 *
 * The C you know reports failure in three different ways: a magic return value
 * (-1), a null pointer, or a global called errno that the next call overwrites.
 * All three are easy to not look at, and nothing complains when you don't.
 *
 * Here a fallible call returns proven_err_t. It is an ordinary value: you can
 * store it, compare it, and pass it upward. And [[nodiscard]] on these
 * functions means ignoring it is a compiler warning, not a habit.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* Room for exactly 8 bytes. (Chapter 2 is about where that room comes
     * from; for now, notice only that we said how much.) */
    proven_result_u8str_t s = proven_u8str_create(alloc, 8);
    EXAMPLE_REQUIRE(proven_is_ok(s.err), "8 bytes should be available");

    /* This fits. */
    proven_err_t err = proven_u8str_append(&s.value, PROVEN_LIT("12345678"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "eight bytes into eight bytes fits exactly");

    /* This does not - and the library REFUSES it. It does not append the part
     * that would fit, because half a word is not a shorter word, it is a
     * different one. */
    proven_err_t too_much = proven_u8str_append(&s.value, PROVEN_LIT("9"));
    EXAMPLE_REQUIRE(!proven_is_ok(too_much), "one byte more than capacity must fail");
    EXAMPLE_REQUIRE(too_much == PROVEN_ERR_OUT_OF_BOUNDS, "and it says why: out of bounds");

    /* The refusal changed nothing. The string is exactly as it was, which is
     * what "failure-atomic" means: a failed call leaves no half-done state. */
    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&s.value), PROVEN_LIT("12345678")),
                    "the refused append must not have written anything");

    proven_println("after the refusal, the string is still: {}",
                   PROVEN_ARG(proven_u8str_as_view(&s.value)));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

**What to notice.** The append that does not fit changes *nothing*. It does not store the part that
would have fit. This is called failure atomicity, and the reason for it is that a truncated string
is not a shorter message, it is a different one — a truncated path names a different file, and a
truncated command is a different command.

**Common first stumble.** `proven_is_ok(err)` is the check; `err == PROVEN_OK` says the same thing.
What you must not do is ignore it and read the value anyway.

---

## Lesson 4 — the value and the error arrive together

**The one new thing:** result structs — `.err` and `.value` in one return.

When a call has nothing to give back, an error code is enough. When it does have something, the
library returns both in one small struct, and the rule is one sentence: **`value` means nothing
until you have looked at `err`.**

<!-- example: manual/examples/en/tut_04_result.c -->
```c
/*
 * Lesson 4 - when the call has a value to give back, the value and the error
 * arrive together.
 *
 * proven_err_t alone is enough when there is nothing to return. When there IS
 * something, you get a small struct with two fields: `err` and `value`. The
 * rule is one sentence: `value` means nothing until you have looked at `err`.
 *
 * This is the same discipline as checking malloc for NULL, except the checking
 * place is part of the type instead of a convention you have to remember.
 */

/* A function of your own can return one too. Nothing in the library is magic. */
static proven_result_size_t safe_div(proven_size_t a, proven_size_t b) {
    proven_result_size_t res = {0};
    if (b == 0) {
        res.err = PROVEN_ERR_INVALID_ARG;    /* value stays 0 - and means nothing */
        return res;
    }
    res.err = PROVEN_OK;
    res.value = a / b;
    return res;
}

int main(void) {
    proven_result_size_t ok = safe_div(10, 2);
    EXAMPLE_REQUIRE(proven_is_ok(ok.err), "dividing by 2 is fine");
    EXAMPLE_REQUIRE(ok.value == 5, "and only now may we read the value");

    proven_result_size_t bad = safe_div(10, 0);
    EXAMPLE_REQUIRE(!proven_is_ok(bad.err), "dividing by zero must fail");
    EXAMPLE_REQUIRE(bad.err == PROVEN_ERR_INVALID_ARG, "and say which rule was broken");
    /* bad.value is 0, but that is not an answer. It is the absence of one. */

    /* The library's own calls have the same shape. proven_u8str_create hands
     * back the string it made together with the error that guards it. */
    proven_allocator_t alloc = proven_heap_allocator();
    proven_result_u8str_t s = proven_u8str_create(alloc, 16);
    if (!proven_is_ok(s.err)) return 1;      /* nothing was created; nothing to destroy */

    proven_println("10 / 2 = {}", PROVEN_ARG(ok.value));

    proven_u8str_destroy(alloc, &s.value);
    return EXAMPLE_OK();
}
```

**What to notice.** `safe_div` is *your* function, not the library's. The pattern is a plain struct
return; there is no machinery underneath, which is why you can use it in your own code the moment
you have read this page.

**Common first stumble.** On failure the `value` field usually holds a zero. That zero is not an
answer — it is the absence of one. Checking `err` is what tells them apart.

---

## Lesson 5 — who gives out the memory is an argument

**The one new thing:** `proven_allocator_t`, passed in rather than assumed.

`malloc` is a decision made for you: one heap, one strategy, and nothing at the call site says so.
In this library, anything that needs memory takes an allocator and uses only that one. Two things
follow, and both are the point: you can always answer "who allocated this?" by reading the call,
and you can hand the same code a different allocator without changing it.

<!-- example: manual/examples/en/tut_05_allocator.c -->
```c
/*
 * Lesson 5 - who gives out the memory is an argument, not a global.
 *
 * malloc is a global decision made for you: one heap, one strategy, invisible
 * at the call site. Here, anything that needs memory takes a
 * proven_allocator_t and uses only that. Two consequences follow, and both are
 * the point.
 *
 *   - You can always answer "who allocated this?" by looking at the call.
 *   - You can hand a different allocator to the same code without changing it.
 */

/* This function does not know or care where the memory comes from. */
static proven_result_u8str_t make_greeting(proven_allocator_t alloc,
                                           proven_u8str_view_t name) {
    proven_result_u8str_t out = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(out.err)) return out;

    proven_err_t err = proven_u8str_append(&out.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&out.value, name);
    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &out.value);   /* undo what we made */
        out.err = err;
        out.value = (proven_u8str_t){0};
    }
    return out;
}

int main(void) {
    /* (a) the ordinary heap - malloc and free underneath */
    proven_allocator_t heap = proven_heap_allocator();

    proven_result_u8str_t a = make_greeting(heap, PROVEN_LIT("world"));
    EXAMPLE_REQUIRE(proven_is_ok(a.err), "the heap should be able to give 64 bytes");
    proven_println("from the heap : {}", PROVEN_ARG(proven_u8str_as_view(&a.value)));
    /* Destroyed with the SAME allocator that created it. That pairing is the
     * whole ownership rule of this library. */
    proven_u8str_destroy(heap, &a.value);

    /* (b) an arena - one block of memory handed out in order, freed all at once.
     *     Note that make_greeting above did not change at all. */
    alignas(PROVEN_MAX_ALIGN) proven_byte_t backing[512];
    proven_arena_t arena = proven_arena_create((proven_mem_mut_t){
        .ptr = backing, .size = sizeof backing });
    proven_allocator_t from_arena = proven_arena_as_allocator(&arena);

    proven_result_u8str_t b = make_greeting(from_arena, PROVEN_LIT("arena"));
    EXAMPLE_REQUIRE(proven_is_ok(b.err), "the arena has room for this too");
    proven_println("from an arena : {}", PROVEN_ARG(proven_u8str_as_view(&b.value)));

    /* No destroy loop here: an arena frees everything by being reset. That is
     * lesson 7's subject; the point now is only that the caller chose. */
    proven_arena_reset(&arena);

    return EXAMPLE_OK();
}
```

**What to notice.** `make_greeting` is written once and runs against the heap and against an arena
— a block of memory you own, handed out by bumping a pointer, and released all at once. The
function did not change, and nothing global was configured.

**The ownership rule, in one line:** whatever created it destroys it, with the *same* allocator.

**Common first stumble.** With an arena, `destroy` reclaims nothing — an arena frees by being
reset. Call it anyway: the pairing is what makes the code correct when the allocator later changes.

---

## Lesson 6 — the Chapter 0 program, read line by line

**The one new thing:** nothing. That is the exercise.

<!-- example: manual/examples/en/tut_06_hello_again.c -->
```c
/*
 * Lesson 6 - the program from Chapter 0, read line by line.
 *
 * Nothing new is introduced here. This is the same greeting program the manual
 * opens with, and the point of the lesson is that you can now name every part
 * of it: the allocator you pass in (lesson 5), the result you must check
 * (lesson 4), the error a refusal returns (lesson 3), the view that carries its
 * own length (lesson 2), and the printing that checks its arguments (lesson 1).
 *
 * If that reads as ordinary now, the tutorial has done its job and the
 * reference chapters are open to you.
 */

int main(void) {
    /* lesson 5 - the caller decides where memory comes from */
    proven_allocator_t alloc = proven_heap_allocator();

    /* lesson 4 - the string and the error that guards it, together */
    proven_result_u8str_t greeting = proven_u8str_create(alloc, 64);
    if (!proven_is_ok(greeting.err)) return 1;

    /* lesson 2 - borrowed text that knows its own size */
    proven_u8str_view_t name = PROVEN_LIT("world");

    /* lesson 3 - each append either fits or refuses; none of them truncates */
    proven_err_t err = proven_u8str_append(&greeting.value, PROVEN_LIT("hello, "));
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, name);
    if (proven_is_ok(err)) err = proven_u8str_append(&greeting.value, PROVEN_LIT("!"));

    if (!proven_is_ok(err)) {
        proven_u8str_destroy(alloc, &greeting.value);
        return 1;
    }

    EXAMPLE_REQUIRE(proven_u8str_view_eq(proven_u8str_as_view(&greeting.value),
                                         PROVEN_LIT("hello, world!")),
                    "the three appends should have built the whole greeting");

    /* lesson 1 - the format string and the argument cannot disagree */
    proven_println("{}", PROVEN_ARG(proven_u8str_as_view(&greeting.value)));

    /* lesson 5 again - destroyed with the allocator that created it */
    proven_u8str_destroy(alloc, &greeting.value);
    return EXAMPLE_OK();
}
```

If those comments now read as labels for things you know rather than as new information, you are
ready for the reference chapters.

---

## Where to go next

| If you want to | Read |
|---|---|
| the full version of everything above | [Chapter 0](manual-00-start-here.md) |
| the types, errors and contracts in detail | [Chapter 1](manual-01-foundation.md) |
| arenas, pools and buffers properly | [Chapter 2](manual-02-allocation.md) |
| owned strings, splitting, encodings | [Chapter 3](manual-03-strings-text.md) |
| arrays, maps, lists, hashing | [Chapter 4](manual-04-containers-algorithms.md) |
| files, streams, time, randomness | [Chapter 5](manual-05-hosted-services.md) |
| a word you do not recognise | [the glossary](manual-00-start-here.md#6-appendix-b-glossary) |

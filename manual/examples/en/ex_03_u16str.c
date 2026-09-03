#include "example.h"

/*
 * UTF-16 exists in this library for one reason: some operating system calls
 * take it and nothing else. The Windows "wide" API is the usual case - the file
 * name you hand to CreateFileW is a NUL-terminated run of 16-bit code units,
 * not bytes.
 *
 * So the job this type does is narrow: assemble the code units, keep the count
 * right, and produce the pointer the system call wants. Everything else in your
 * program should stay UTF-8.
 *
 * The one thing to keep straight is the unit. A capacity of 32 here means 32
 * CODE UNITS, which is 64 bytes, and a character outside the Basic Multilingual
 * Plane - an emoji, most of the rarer CJK characters - costs two of them. A
 * count of code units is not a count of characters and never has been.
 */

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    /* The argument is a code-unit limit, not a byte limit. */
    proven_result_u16str_t r = proven_u16str_create(alloc, 32);
    EXAMPLE_REQUIRE(proven_is_ok(r.err), "creating a 32-code-unit string must succeed");
    if (!proven_is_ok(r.err)) {
        return 1;
    }
    proven_u16str_t name = r.value;
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == 0, "a new string is empty");

    /* PROVEN_U16_LIT builds a view from a u"..." literal and computes the unit
     * count from the literal itself, so the count cannot disagree with the text. */
    proven_err_t err = proven_u16str_append(&name, PROVEN_U16_LIT("C:\\logs\\"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the directory prefix fits in 32 code units");

    err = proven_u16str_append(&name, PROVEN_U16_LIT("service.log"));
    EXAMPLE_REQUIRE(proven_is_ok(err), "the file name fits too");
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == 8 + 11, "the length is a count of code units");

    /* Atomic, like its byte-string twin: too much data is refused and the string
     * is left exactly as it was, so a path is never half-written. */
    proven_size_t before = proven_u16str_len(&name);
    err = proven_u16str_append(&name, PROVEN_U16_LIT(".a-suffix-long-enough-to-overflow-the-capacity"));
    EXAMPLE_REQUIRE(err == PROVEN_ERR_OUT_OF_BOUNDS, "an oversized append must be refused");
    EXAMPLE_REQUIRE(proven_u16str_len(&name) == before, "and must not truncate the path");

    /* --- the pointer the system call wants -------------------------------- */

    /* as_ptr hands back the internal code units, NUL-terminated, without
     * copying. It is the last step before the call, and the pointer is only
     * valid until the next append: a growing append may move the storage. */
    const proven_u16 *wide = proven_u16str_as_ptr(&name);
    EXAMPLE_REQUIRE(wide != NULL, "an assembled string must yield a pointer");
    EXAMPLE_REQUIRE(wide[0] == (proven_u16)'C', "the first code unit is the drive letter");
    EXAMPLE_REQUIRE(wide[proven_u16str_len(&name)] == 0, "the sequence is NUL-terminated for the system call");
    /* On Windows this is the whole point of the type:
     *     HANDLE h = CreateFileW((LPCWSTR)wide, ...);
     * Nothing here calls it, because this example must also run everywhere else. */

    /* --- when truncation is the correct answer ---------------------------- */

    /* Some system structures have a fixed-width field - a 16-unit label, say -
     * where a name that does not fit is meant to be cut, not rejected. That is
     * what the partial append is for: it fills what it can and reports the unit
     * count it wrote, so the caller can mark the value as truncated. */
    proven_result_u16str_t r2 = proven_u16str_create(alloc, 16);
    EXAMPLE_REQUIRE(proven_is_ok(r2.err), "creating the fixed-width label must succeed");
    proven_u16str_t label = r2.value;

    proven_result_size_t wrote = proven_u16str_append_partial(&label, PROVEN_U16_LIT("a-label-that-is-longer-than-the-field"));
    EXAMPLE_REQUIRE(wrote.err == PROVEN_ERR_OUT_OF_BOUNDS, "the truncation is reported, not hidden");
    EXAMPLE_REQUIRE(wrote.value == 16, "it filled the field exactly");
    EXAMPLE_REQUIRE(proven_u16str_len(&label) == wrote.value, "and the length matches what it says it wrote");
    EXAMPLE_REQUIRE(proven_u16str_as_ptr(&label)[wrote.value] == 0,
                    "a truncated string is still NUL-terminated, so it is still safe to pass on");

    printf("assembled %zu code unit(s); label truncated to %zu\n",
           (size_t)proven_u16str_len(&name), (size_t)wrote.value);

    proven_u16str_destroy(alloc, &label);
    proven_u16str_destroy(alloc, &name);
    return EXAMPLE_OK();
}

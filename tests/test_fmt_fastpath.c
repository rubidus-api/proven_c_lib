#include "proven/fmt.h"
#include "proven/heap.h"
#include "proven_test.h"
#include <string.h>

static void expect_trunc_matches_reference(
    const char *label,
    proven_allocator_t alloc,
    proven_size_t capacity,
    const char *fmt,
    const proven_arg_t *args,
    proven_size_t args_count
) {
    proven_result_u8str_t trunc_res0 = proven_u8str_create(alloc, capacity);
    proven_result_u8str_t ref_res0 = proven_u8str_create(alloc, capacity + 32u);
    PROVEN_TEST_ASSERT(trunc_res0.err == PROVEN_OK, label, "Inspect fixed-capacity string creation in the reference helper.");
    PROVEN_TEST_ASSERT(ref_res0.err == PROVEN_OK, label, "Inspect growable string creation in the reference helper.");
    proven_u8str_t trunc = trunc_res0.value;
    proven_u8str_t ref = ref_res0.value;

    proven_fmt_result_t trunc_res = proven_u8str_fmt_internal((proven_allocator_t){0}, &trunc, true, fmt, (proven_allocator_t){0}, args, args_count);
    proven_fmt_result_t ref_res = proven_u8str_fmt_internal(alloc, &ref, false, fmt, (proven_allocator_t){0}, args, args_count);

    const char *trunc_text = proven_u8str_as_cstr(&trunc);
    const char *ref_text = proven_u8str_as_cstr(&ref);
    proven_size_t ref_len = proven_cstr_len(ref_text);
    proven_size_t trunc_limit = (capacity > 0) ? (capacity - 1u) : 0u;
    proven_size_t expected_len = ref_len < trunc_limit ? ref_len : trunc_limit;

    PROVEN_TEST_ASSERT(trunc_res.err == (ref_res.err == PROVEN_OK && ref_len > trunc_limit ? PROVEN_ERR_OUT_OF_BOUNDS : ref_res.err), label, "Inspect truncation mode error handling and compare it with the reference path.");
    PROVEN_TEST_ASSERT(trunc_res.required == ref_res.required || (ref_res.err == PROVEN_OK && ref_len > trunc_limit && trunc_res.required == ref_res.required), label, "Inspect required-byte accounting in truncation mode.");
    PROVEN_TEST_ASSERT(trunc_res.written == expected_len, label, "Inspect truncation-length accounting and trailing NUL placement.");
    PROVEN_TEST_ASSERT(strncmp(trunc_text, ref_text, expected_len) == 0, label, "Inspect truncation output bytes against the reference path.");
    PROVEN_TEST_ASSERT(trunc_text[expected_len] == '\0', label, "Inspect trailing NUL placement after truncation.");

    proven_u8str_destroy(alloc, &ref);
    proven_u8str_destroy(alloc, &trunc);
}

int main(void) {
    proven_allocator_t alloc = proven_heap_allocator();

    PROVEN_TEST_SUITE(
        "test_fmt_fastpath",
        "Validate truncating fixed-capacity formatting against a reference growable path across exact-fit, truncating, excess-argument, and malformed-format inputs.",
        "Inspect proven_u8str_fmt_internal if truncation output, required-byte accounting, or error reporting drifts from the reference path."
    );

    PROVEN_TEST_SECTION(
        "exact fit and truncation",
        "Confirm that truncating fixed-capacity writes match the reference output bytes and accounting.",
        "Inspect append-clamping logic and required-byte counting if these cases diverge.");
    {
        const proven_arg_t args[] = { PROVEN_ARG(1), PROVEN_ARG("abc") };
        expect_trunc_matches_reference("exact fit", alloc, 16, "{} {}", args, 2);
    }
    {
        const proven_arg_t args[] = { PROVEN_ARG(100) };
        expect_trunc_matches_reference("truncation", alloc, 5, "Number: {}", args, 1);
    }

    PROVEN_TEST_SECTION(
        "validation failures",
        "Confirm that argument-count mismatches and malformed formats report the same errors as the reference path.",
        "Inspect format parsing and argument count validation if these failures change.");
    {
        const proven_arg_t args[] = { PROVEN_ARG(1), PROVEN_ARG(2) };
        expect_trunc_matches_reference("excess args", alloc, 16, "{}", args, 2);
    }
    {
        const proven_arg_t args[] = { PROVEN_ARG(1) };
        expect_trunc_matches_reference("malformed format", alloc, 16, "{", args, 1);
    }
    {
        const proven_arg_t args[] = { { .type = (proven_arg_type_t)99 } };
        proven_result_u8str_t out_res = proven_u8str_create(alloc, 16);
        PROVEN_TEST_ASSERT(out_res.err == PROVEN_OK, "invalid arg type setup", "Inspect the helper allocator if test setup fails.");
        proven_u8str_t out = out_res.value;
        proven_fmt_result_t res = proven_u8str_fmt_internal(alloc, &out, false, "{}", (proven_allocator_t){0}, args, 1u);
        PROVEN_TEST_ASSERT(res.err == PROVEN_ERR_INVALID_ARG, "invalid arg type should fail", "Inspect render_arg default handling if unknown argument types are ignored.");
        proven_u8str_destroy(alloc, &out);
    }

    PROVEN_TEST_PASS("Formatter fast-path comparison checks passed.");
    return 0;
}

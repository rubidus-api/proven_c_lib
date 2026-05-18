#include "proven/fmt.h"
#include "proven_internal_memrange.h"
#include "../../platform/proven_sys_mem.h"

// =============================================================================
// Formatting Context
// =============================================================================

// Internal helpers for string reversal and integer conversion
static const char digit_pairs[201] = 
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

typedef struct {
    char fill;
    char align; // '<', '>', '^'
    int  width;
    bool hex;
} proven_fmt_spec_t;
 
 typedef struct {
    proven_u8str_t *str;
    proven_err_t    err;
    proven_size_t   written;
    proven_size_t   required;
    bool            measure_only;
} proven_fmt_ctx_t;

// =============================================================================
// Rendering Helpers
// =============================================================================

static void fmt_append_view(proven_fmt_ctx_t *ctx, proven_u8str_view_t view) {
    if (!proven_is_ok(ctx->err)) return;

    if (view.size > 0 && !view.ptr) {
        ctx->err = PROVEN_ERR_INVALID_ARG;
        return;
    }

    if (view.size == 0) return;

    if (PROVEN_CKD_ADD(&ctx->required, ctx->required, view.size)) {
        ctx->err = PROVEN_ERR_OVERFLOW;
        return;
    }

    if (ctx->measure_only) return;

    proven_size_t cap = ctx->str->internal.cap;
    proven_size_t len = ctx->str->internal.len;

    if (len + 1 >= cap) {
        return; // Truncated
    }

    proven_size_t available = cap - len - 1;
    proven_size_t to_copy = view.size;
    if (to_copy > available) {
        to_copy = available;
    }

    proven_sys_mem_move(ctx->str->internal.ptr + len, view.ptr, to_copy);
    ctx->str->internal.len += to_copy;
    ctx->str->internal.ptr[ctx->str->internal.len] = 0;
    ctx->written += to_copy;
}

static void fmt_append_byte(proven_fmt_ctx_t *ctx, proven_u8 b) {
    proven_u8str_view_t v = { .ptr = &b, .size = 1 };
    fmt_append_view(ctx, v);
}

static int itoa_raw(unsigned long long val, char *s, int base_in) {
    if (val == 0) {
        s[0] = '0';
        s[1] = '\0';
        return 1;
    }

    unsigned long long base = (unsigned int)base_in;
    char temp[64];
    int i = 0;
    
    if (base == 10) {
        while (val >= 100) {
            unsigned int rem = (unsigned int)(val % 100);
            val /= 100;
            temp[i++] = digit_pairs[rem * 2 + 1];
            temp[i++] = digit_pairs[rem * 2];
        }
        if (val < 10) {
            temp[i++] = '0' + (char)val;
        } else {
            unsigned int v = (unsigned int)val;
            temp[i++] = digit_pairs[v * 2 + 1];
            temp[i++] = digit_pairs[v * 2];
        }
    } else if (base == 16) {
        static const char hex_chars[] = "0123456789abcdef";
        while (val != 0) {
            temp[i++] = hex_chars[val & 0xFu];
            val >>= 4;
        }
    } else {
        while (val != 0) {
            unsigned int r = (unsigned int)(val % base);
            temp[i++] = (char)((r > 9) ? (char)((r - 10) + 'a') : (char)(r + '0'));
            val /= base;
        }
    }
    
    for (int j = 0; j < i; j++) {
        s[j] = temp[i - 1 - j];
    }
    s[i] = '\0';
    return i;
}

static void append_padding(proven_fmt_ctx_t *ctx, char fill, int count) {
    if (count <= 0) return;
    if (ctx->measure_only) {
        if (PROVEN_CKD_ADD(&ctx->required, ctx->required, (proven_size_t)count)) {
            ctx->err = PROVEN_ERR_OVERFLOW;
        }
        return;
    }

    proven_u8str_view_t v = { .ptr = NULL, .size = (proven_size_t)count };
    
    // We can optimize append_padding to not use fmt_append_view which requires a ptr.
    // Instead we directly manipulate the ctx.
    if (!proven_is_ok(ctx->err)) return;

    if (PROVEN_CKD_ADD(&ctx->required, ctx->required, v.size)) {
        ctx->err = PROVEN_ERR_OVERFLOW;
        return;
    }

    proven_size_t cap = ctx->str->internal.cap;
    proven_size_t len = ctx->str->internal.len;

    if (len + 1 >= cap) return;

    proven_size_t available = cap - len - 1;
    proven_size_t to_write = v.size;
    if (to_write > available) to_write = available;

    proven_sys_mem_set(ctx->str->internal.ptr + len, (int)fill, to_write);
    ctx->str->internal.len += to_write;
    ctx->str->internal.ptr[ctx->str->internal.len] = 0;
    ctx->written += to_write;
}

static void render_with_spec(proven_fmt_ctx_t *ctx, const char *val, proven_size_t val_len, proven_fmt_spec_t spec) {
    if (spec.width == 0 || (proven_size_t)spec.width <= val_len) {
        fmt_append_view(ctx, (proven_u8str_view_t){(const proven_u8*)val, val_len});
        return;
    }

    int total_padding = spec.width - (int)val_len;
    if (spec.align == '>') {
        append_padding(ctx, spec.fill, total_padding);
        fmt_append_view(ctx, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
    } else if (spec.align == '<') {
        fmt_append_view(ctx, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
        append_padding(ctx, spec.fill, total_padding);
    } else { // '^'
        int left = total_padding / 2;
        int right = total_padding - left;
        append_padding(ctx, spec.fill, left);
        fmt_append_view(ctx, (proven_u8str_view_t){(const proven_u8*)val, (proven_size_t)val_len});
        append_padding(ctx, spec.fill, right);
    }
}

static void render_arg(proven_fmt_ctx_t *ctx, const proven_arg_t *arg, proven_fmt_spec_t spec) {
    if (!proven_is_ok(ctx->err)) return;

    char buf[128];
    proven_size_t len = 0;

    switch (arg->type) {
        case PROVEN_ARG_I32: {
            if (spec.hex) {
                len = (proven_size_t)itoa_raw((unsigned int)arg->value.i32, buf, 16);
            } else {
                long long v = arg->value.i32;
                uint64_t mag;
                proven_size_t offset = 0;
                if (v < 0) {
                    buf[offset++] = '-';
                    mag = (uint64_t)(-(v + 1)) + 1;
                } else {
                    mag = (uint64_t)v;
                }
                len = (proven_size_t)itoa_raw((unsigned long long)mag, buf + offset, 10) + offset;
            }
            render_with_spec(ctx, buf, len, spec);
            break;
        }
        case PROVEN_ARG_U32:
            len = (proven_size_t)itoa_raw(arg->value.u32, buf, spec.hex ? 16 : 10);
            render_with_spec(ctx, buf, len, spec);
            break;
        case PROVEN_ARG_I64: {
            if (spec.hex) {
                len = (proven_size_t)itoa_raw((unsigned long long)arg->value.i64, buf, 16);
            } else {
                long long v = arg->value.i64;
                uint64_t mag;
                proven_size_t offset = 0;
                if (v < 0) {
                    buf[offset++] = '-';
                    mag = (uint64_t)(-(v + 1)) + 1;
                } else {
                    mag = (uint64_t)v;
                }
                len = (proven_size_t)itoa_raw((unsigned long long)mag, buf + offset, 10) + offset;
            }
            render_with_spec(ctx, buf, len, spec);
            break;
        }
        case PROVEN_ARG_U64:
            len = (proven_size_t)itoa_raw(arg->value.u64, buf, spec.hex ? 16 : 10);
            render_with_spec(ctx, buf, len, spec);
            break;
#ifndef PROVEN_FMT_NO_FLOAT
        case PROVEN_ARG_F64: {
            double v = arg->value.f64;
            unsigned long long bits;
            proven_sys_mem_copy(&bits, &v, sizeof(double));
            bool sign = (bits >> 63) != 0;
            int exp = (int)((bits >> 52) & 0x7FF);
            unsigned long long mantissa = bits & 0xFFFFFFFFFFFFFull;

            if (exp == 0x7FF) {
                if (mantissa != 0) {
                    render_with_spec(ctx, "NaN", 3, spec);
                } else if (sign) {
                    render_with_spec(ctx, "-Inf", 4, spec);
                } else {
                    render_with_spec(ctx, "Inf", 3, spec);
                }
                break;
            }

            proven_size_t offset = 0;
            long double abs_v = sign ? -(long double)v : (long double)v;
            if (sign) {
                buf[offset++] = '-';
            }

            const unsigned long long precision_scale = 1000000ULL;
            const int precision = 6;
            bool use_scientific = (abs_v >= 1e18L || (abs_v > 0.0L && abs_v < 1e-4L));

            if (use_scientific) {
                int sci_exp = 0;
                while (abs_v >= 1e256L) { abs_v /= 1e256L; sci_exp += 256; }
                while (abs_v >= 1e128L) { abs_v /= 1e128L; sci_exp += 128; }
                while (abs_v >= 1e64L) { abs_v /= 1e64L; sci_exp += 64; }
                while (abs_v >= 1e32L) { abs_v /= 1e32L; sci_exp += 32; }
                while (abs_v >= 1e16L) { abs_v /= 1e16L; sci_exp += 16; }
                while (abs_v >= 1e8L) { abs_v /= 1e8L; sci_exp += 8; }
                while (abs_v >= 1e4L) { abs_v /= 1e4L; sci_exp += 4; }
                while (abs_v >= 1e2L) { abs_v /= 1e2L; sci_exp += 2; }
                while (abs_v >= 10.0L) { abs_v /= 10.0L; sci_exp++; }
                while (abs_v > 0.0L && abs_v < 1.0L) {
                    abs_v *= 1e256L; sci_exp -= 256;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e128L; sci_exp -= 128;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e64L; sci_exp -= 64;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e32L; sci_exp -= 32;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e16L; sci_exp -= 16;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e8L; sci_exp -= 8;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e4L; sci_exp -= 4;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 1e2L; sci_exp -= 2;
                    if (abs_v >= 1.0L) break;
                    abs_v *= 10.0L; sci_exp--;
                }

                unsigned long long digit = (unsigned long long)abs_v;
                long double frac = abs_v - (long double)digit;
                unsigned long long frac_i = (unsigned long long)(frac * (long double)precision_scale + 0.5L);
                if (frac_i >= precision_scale) {
                    frac_i -= precision_scale;
                    digit++;
                    if (digit >= 10ULL) {
                        digit = 1ULL;
                        sci_exp++;
                    }
                }

                buf[offset++] = (char)('0' + (int)digit);
                buf[offset++] = '.';
                for (int i = precision - 1; i >= 0; --i) {
                    buf[offset + (proven_size_t)i] = (char)('0' + (int)(frac_i % 10ULL));
                    frac_i /= 10ULL;
                }
                offset += (proven_size_t)precision;
                buf[offset++] = 'e';
                if (sci_exp >= 0) {
                    buf[offset++] = '+';
                    offset += (proven_size_t)itoa_raw((unsigned long long)sci_exp, buf + offset, 10);
                } else {
                    buf[offset++] = '-';
                    offset += (proven_size_t)itoa_raw((unsigned long long)(-sci_exp), buf + offset, 10);
                }
                buf[offset] = '\0';
                render_with_spec(ctx, buf, offset, spec);
                break;
            }

            unsigned long long ipart = (unsigned long long)abs_v;
            long double frac = abs_v - (long double)ipart;
            unsigned long long frac_i = (unsigned long long)(frac * (long double)precision_scale + 0.5L);
            if (frac_i >= precision_scale) {
                frac_i -= precision_scale;
                ipart++;
            }

            offset += (proven_size_t)itoa_raw(ipart, buf + offset, 10);
            buf[offset++] = '.';
            for (int i = precision - 1; i >= 0; --i) {
                buf[offset + (proven_size_t)i] = (char)('0' + (int)(frac_i % 10ULL));
                frac_i /= 10ULL;
            }
            offset += (proven_size_t)precision;
            buf[offset] = '\0';
            render_with_spec(ctx, buf, offset, spec);
            break;
        }
#endif
        case PROVEN_ARG_CSTR:
            render_with_spec(ctx, arg->value.cstr, (proven_size_t)proven_cstr_len(arg->value.cstr), spec);
            break;
        case PROVEN_ARG_STR_VIEW:
            render_with_spec(ctx, (const char*)arg->value.str_view.ptr, arg->value.str_view.size, spec);
            break;
        case PROVEN_ARG_DATETIME: {
            proven_datetime_t dt = arg->value.datetime;
            char *curr = buf;
            
            #define APPEND_FIXED(val, digits) { \
                char temp[20]; \
                int n = itoa_raw((unsigned long long)(val), temp, 10); \
                for (int _i = 0; _i < (digits) - n; _i++) *curr++ = '0'; \
                for (int _i = 0; _i < n; _i++) *curr++ = temp[_i]; \
            }
            
            APPEND_FIXED(dt.year, 4); *curr++ = '-';
            APPEND_FIXED(dt.month, 2); *curr++ = '-';
            APPEND_FIXED(dt.day, 2); *curr++ = ' ';
            APPEND_FIXED(dt.hour, 2); *curr++ = ':';
            APPEND_FIXED(dt.min, 2); *curr++ = ':';
            APPEND_FIXED(dt.sec, 2);
            *curr = '\0';
            len = (proven_size_t)(curr - buf);
            #undef APPEND_FIXED
            
            render_with_spec(ctx, buf, len, spec);
            break;
        }
        case PROVEN_ARG_PTR: {
            buf[0] = '0'; buf[1] = 'x';
            len = (proven_size_t)itoa_raw((unsigned long long)(proven_uintptr_t)arg->value.ptr, buf + 2, 16) + 2;
            render_with_spec(ctx, buf, len, spec);
            break;
        }
        case PROVEN_ARG_FN: {
            const unsigned char *p = (const unsigned char *)&arg->value.fn;
            proven_size_t s = sizeof(arg->value.fn);
            char *curr = buf;
            // Respect ISO C: function pointers are not data pointers.
            // We format the raw representation bytes.
            *curr++ = '('; *curr++ = 'f'; *curr++ = 'n'; *curr++ = ':';
            *curr++ = '0'; *curr++ = 'x';
            for (proven_size_t i = 0; i < s; i++) {
                static const char hex_map[] = "0123456789abcdef";
                unsigned char b = p[i];
                *curr++ = hex_map[b >> 4];
                *curr++ = hex_map[b & 0xf];
            }
            *curr++ = ')';
            *curr = '\0';
            len = (proven_size_t)(curr - buf);
            render_with_spec(ctx, buf, len, spec);
            break;
        }
        default: break;
    }
}

// =============================================================================
// Parsing Helpers
// =============================================================================

static bool proven_fmt_parse_arg_index(const char **p_ptr, proven_size_t *out_idx, proven_err_t *err) {
    const char *p = *p_ptr;
    if (*p >= '0' && *p <= '9') {
        proven_size_t arg_idx_raw = 0;
        while (*p >= '0' && *p <= '9') { 
            proven_size_t digit = (proven_size_t)(*p - '0');
            if (PROVEN_CKD_MUL(&arg_idx_raw, arg_idx_raw, 10) ||
                PROVEN_CKD_ADD(&arg_idx_raw, arg_idx_raw, digit)) {
                *err = PROVEN_ERR_OVERFLOW;
                return true;
            }
            p++; 
        }
        *p_ptr = p;
        *out_idx = arg_idx_raw;
        return true;
    }
    return false;
}

// =============================================================================
// Format Engine
// =============================================================================

static void fmt_run(proven_fmt_ctx_t *ctx, const char *fmt, const proven_arg_t *args, proven_size_t args_count, proven_size_t *max_idx) {
    const char *p = fmt;
    proven_size_t next_arg_idx = 1;
    *max_idx = 0;

    while (*p && proven_is_ok(ctx->err)) {
        if (*p == '}') {
            p++;
            if (*p == '}') {
                fmt_append_byte(ctx, (proven_u8)'}');
                p++; continue;
            }
            // Bare '}' is an error in some specs, but let's be strict.
            ctx->err = PROVEN_ERR_INVALID_FORMAT;
            return;
        }

        if (*p != '{') {
            fmt_append_byte(ctx, (proven_u8)*p);
            p++; continue;
        }

        p++; // skip {
        if (*p == '{') {
            fmt_append_byte(ctx, (proven_u8)'{');
            p++; continue;
        }

        proven_size_t arg_idx_raw = 0;
        bool has_idx = proven_fmt_parse_arg_index(&p, &arg_idx_raw, &ctx->err);
        if (!proven_is_ok(ctx->err)) return;

        proven_size_t arg_idx = PROVEN_INDEX_NOT_FOUND;
        if (has_idx) {
            proven_size_t mapped_idx;
            if (PROVEN_CKD_ADD(&mapped_idx, arg_idx_raw, 1)) {
                ctx->err = PROVEN_ERR_OVERFLOW;
                return;
            }
            arg_idx = mapped_idx;
        } else if (*p == ':' || *p == '}') {
            arg_idx = next_arg_idx++;
        } else {
            ctx->err = PROVEN_ERR_INVALID_FORMAT;
            return;
        }

        if (arg_idx != PROVEN_INDEX_NOT_FOUND && arg_idx > *max_idx) *max_idx = arg_idx;

        proven_fmt_spec_t spec = { .fill = ' ', .align = '>', .width = 0, .hex = false };
        if (*p == ':') {
            p++;
            // Check for fill + align
            if (*p && p[0] != '}' && (p[1] == '<' || p[1] == '>' || p[1] == '^')) {
                spec.fill = *p;
                spec.align = p[1];
                p += 2;
            } else if (*p == '<' || *p == '>' || *p == '^') {
                spec.align = *p;
                p++;
            }

            int limit = 10000;
            while (*p >= '0' && *p <= '9') {
                int digit = (*p - '0');
                if (PROVEN_CKD_MUL(&spec.width, spec.width, 10)) { ctx->err = PROVEN_ERR_OVERFLOW; return; }
                if (PROVEN_CKD_ADD(&spec.width, spec.width, digit)) { ctx->err = PROVEN_ERR_OVERFLOW; return; }
                if (spec.width > limit) { ctx->err = PROVEN_ERR_OUT_OF_BOUNDS; return; }
                p++;
            }

            if (*p == 'x') { 
                spec.hex = true; 
                p++; 
            }
            
            // If we are not at '}', it's an invalid specifier character
            if (*p != '}') {
                ctx->err = PROVEN_ERR_INVALID_FORMAT;
                return;
            }
        }

        if (*p != '}') {
            ctx->err = PROVEN_ERR_INVALID_FORMAT;
            return;
        }
        p++; // skip }

        if (arg_idx != PROVEN_INDEX_NOT_FOUND && arg_idx > 0 && (size_t)arg_idx < args_count) {
            render_arg(ctx, &args[arg_idx], spec);
        } else {
            ctx->err = PROVEN_ERR_INVALID_ARG;
        }
    }
}

// =============================================================================
// Alias Patching Helpers
// =============================================================================

typedef struct {
    const proven_arg_t *final_args;
    proven_arg_t *heap_patched;
    proven_arg_t stack_patched[16];
    proven_size_t args_count;
    proven_allocator_t patch_alloc;
} proven_fmt_arg_patch_t;

static proven_err_t proven_fmt_arg_patch_prepare(
    proven_fmt_arg_patch_t *patch,
    const proven_arg_t *args,
    proven_size_t args_count,
    const proven_byte_t *old_ptr,
    proven_size_t old_cap,
    proven_allocator_t patch_alloc)
{
    patch->final_args = args;
    patch->heap_patched = NULL;
    patch->args_count = args_count;
    patch->patch_alloc = patch_alloc;

    bool will_need_patch = false;

    // To guarantee failure-atomicity, pre-allocate heap_patched before string reallocation if an alias is detected
    for (proven_size_t i = 0; i < args_count; i++) {
        if (args[i].type == PROVEN_ARG_STR_VIEW) {
            proven_bufref_t alias_ref = proven_bufref_capture(old_ptr, old_cap, args[i].value.str_view.ptr, args[i].value.str_view.size);
            if (alias_ref.valid) {
                will_need_patch = true;
                break;
            }
        }
    }

    if (will_need_patch && args_count > 16) {
        proven_size_t patch_bytes;
        if (PROVEN_CKD_MUL(&patch_bytes, (proven_size_t)args_count, sizeof(proven_arg_t))) {
            return PROVEN_ERR_OVERFLOW;
        }
        proven_result_mem_mut_t m = patch_alloc.alloc_fn(patch_alloc.ctx, patch_bytes, alignof(proven_arg_t));
        if (!PROVEN_IS_OK(m.err)) {
            return m.err;
        }
        patch->heap_patched = (proven_arg_t*)m.value.ptr;
    }

    return PROVEN_OK;
}

static proven_err_t proven_fmt_arg_patch_apply(
    proven_fmt_arg_patch_t *patch,
    const proven_arg_t *args,
    const proven_byte_t *old_ptr,
    proven_size_t old_cap,
    const proven_byte_t *new_ptr)
{
    patch->final_args = args;
    
    bool needs_patch = (old_ptr != NULL && old_ptr != new_ptr && patch->args_count > 0);
    if (!needs_patch) {
        return PROVEN_OK;
    }

    bool found_any_alias = false;
    for (proven_size_t i = 0; i < patch->args_count; i++) {
        if (args[i].type == PROVEN_ARG_STR_VIEW) {
            proven_bufref_t alias_ref = proven_bufref_capture(old_ptr, old_cap, args[i].value.str_view.ptr, args[i].value.str_view.size);
            if (alias_ref.valid) {
                found_any_alias = true;
                break;
            }
        }
    }

    if (found_any_alias) {
        if (patch->args_count > 16 && !patch->heap_patched) {
            return PROVEN_ERR_NOMEM;
        }

        proven_arg_t *patched_args = patch->heap_patched ? patch->heap_patched : patch->stack_patched;
        for (proven_size_t i = 0; i < patch->args_count; i++) {
            patched_args[i] = args[i];
            if (args[i].type == PROVEN_ARG_STR_VIEW) {
                proven_bufref_t alias_ref = proven_bufref_capture(old_ptr, old_cap, args[i].value.str_view.ptr, args[i].value.str_view.size);
                if (alias_ref.valid) {
                    patched_args[i].value.str_view.ptr = (const proven_byte_t *)proven_bufref_rebase_const(alias_ref, new_ptr);
                }
            }
        }
        patch->final_args = patched_args;
    }

    return PROVEN_OK;
}

static void proven_fmt_arg_patch_destroy(proven_fmt_arg_patch_t *patch) {
    if (patch->heap_patched && proven_alloc_is_valid(patch->patch_alloc)) {
        patch->patch_alloc.free_fn(patch->patch_alloc.ctx, patch->heap_patched);
        patch->heap_patched = NULL;
    }
}

// =============================================================================
// Entry Points
// =============================================================================

proven_fmt_result_t proven_u8str_fmt_internal(proven_allocator_t alloc, proven_u8str_t *str, bool trunc, const char *fmt, proven_allocator_t scratch, const proven_arg_t *args, proven_size_t args_count) {
    proven_fmt_result_t res = { .err = PROVEN_OK };
    if (!str || !fmt || (args_count > 0 && !args)) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    // Pass 0: Global self-alias validation for CSTR.
    // CSTR self-alias check (Option A: Explicitly reject as per docs)
    for (proven_size_t i = 0; i < args_count; i++) {
        if (args[i].type == PROVEN_ARG_CSTR) {
            proven_bufref_t alias_ref = proven_bufref_capture(str->internal.ptr, str->internal.cap, args[i].value.cstr, 0);
            if (alias_ref.valid) {
                res.err = PROVEN_ERR_INVALID_ARG;
                return res;
            }
        }
    }

    // Pass 1: Measure required length
    proven_fmt_ctx_t measure_ctx = {
        .str = str,
        .err = PROVEN_OK,
        .written = 0,
        .required = 0,
        .measure_only = true
    };
    proven_size_t max_idx = 0;
    fmt_run(&measure_ctx, fmt, args, args_count, &max_idx);
    if (!proven_is_ok(measure_ctx.err)) {
        res.err = measure_ctx.err;
        return res;
    }

    // Verification-oriented policy: detection of excess arguments
    proven_size_t expected_args;
    if (PROVEN_CKD_ADD(&expected_args, max_idx, 1)) {
        res.err = PROVEN_ERR_OVERFLOW;
        return res;
    }
    
    if (expected_args != args_count) {
        res.err = PROVEN_ERR_INVALID_ARG;
        return res;
    }

    res.required = measure_ctx.required;

    const proven_byte_t *old_ptr = str->internal.ptr;
    proven_size_t old_cap = str->internal.cap;
    proven_fmt_arg_patch_t patch = {0};

    // Growth logic
    if (proven_alloc_is_valid(alloc)) {
        proven_size_t total_needed;
        if (PROVEN_CKD_ADD(&total_needed, str->internal.len, measure_ctx.required)) return (proven_fmt_result_t){.err = PROVEN_ERR_OVERFLOW};
        if (PROVEN_CKD_ADD(&total_needed, total_needed, 1)) return (proven_fmt_result_t){.err = PROVEN_ERR_OVERFLOW};

        if (total_needed > str->internal.cap) {
            proven_size_t new_cap = str->internal.cap == 0 ? 16 : str->internal.cap;
            while (new_cap < total_needed) {
                if (PROVEN_CKD_MUL(&new_cap, new_cap, 2)) {
                    new_cap = total_needed;
                    break;
                }
            }
            proven_allocator_t patch_alloc = proven_alloc_is_valid(scratch) ? scratch : alloc;
            proven_err_t patch_err = proven_fmt_arg_patch_prepare(&patch, args, args_count, old_ptr, old_cap, patch_alloc);
            if (!PROVEN_IS_OK(patch_err)) {
                res.err = patch_err;
                return res;
            }

            proven_result_mem_mut_t new_mem = alloc.realloc_fn(alloc.ctx, str->internal.ptr, str->internal.cap, new_cap, PROVEN_DEFAULT_ALIGNMENT);
            if (!proven_is_ok(new_mem.err)) {
                proven_fmt_arg_patch_destroy(&patch);
                res.err = new_mem.err;
                return res;
            }
            str->internal.ptr = (proven_byte_t*)new_mem.value.ptr;
            str->internal.cap = new_cap;
        }
    }

    // Atomic fixed-capacity check
    if (!trunc && !proven_alloc_is_valid(alloc)) {
        proven_size_t needed;
        if (PROVEN_CKD_ADD(&needed, str->internal.len, res.required) ||
            PROVEN_CKD_ADD(&needed, needed, 1)) {
            res.err = PROVEN_ERR_OVERFLOW;
            return res;
        }
        if (needed > str->internal.cap) {
            res.err = PROVEN_ERR_OUT_OF_BOUNDS;
            return res;
        }
    }

    // Pass 2: Write
    proven_err_t apply_err = proven_fmt_arg_patch_apply(&patch, args, old_ptr, old_cap, str->internal.ptr);
    if (!PROVEN_IS_OK(apply_err)) {
        res.err = apply_err;
        proven_fmt_arg_patch_destroy(&patch);
        return res;
    }

    proven_fmt_ctx_t write_ctx = {
        .str = str,
        .err = PROVEN_OK,
        .written = 0,
        .required = 0,
        .measure_only = false
    };
    proven_size_t dummy_idx = 0;
    fmt_run(&write_ctx, fmt, patch.final_args, args_count, &dummy_idx);
    
    res.err = write_ctx.err;
    res.written = write_ctx.written;
    
    proven_fmt_arg_patch_destroy(&patch);

    // If we are in trunc mode and wasn't able to write all, signal OOB
    if (trunc && res.written < res.required) {
        res.err = PROVEN_ERR_OUT_OF_BOUNDS;
    }

    return res;
}


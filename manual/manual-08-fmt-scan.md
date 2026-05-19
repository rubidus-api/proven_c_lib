     1|# Chapter 8: Formatting and Scanning (v26.05.19i)
     2|
     3|This chapter is the detailed reference for `fmt.h` and `scan.h`.
     4|Chapter 3 gives the shorter overview and the everyday examples.
     5|This chapter focuses on exact syntax, parameter shapes, return values, and the places where callers usually make mistakes.
     6|
     7|## Table of contents
     8|
     9|1. [Design model](#1-design-model)
    10|2. [Formatter data model](#2-formatter-data-model)
    11|3. [Formatter constructors and selectors](#3-formatter-constructors-and-selectors)
    12|4. [Format string grammar](#4-format-string-grammar)
    13|5. [Formatting APIs](#5-formatting-apis)
    14|6. [Console print helpers](#6-console-print-helpers)
    15|7. [Scanner data model](#7-scanner-data-model)
    16|8. [Scanner primitive APIs](#8-scanner-primitive-apis)
    17|9. [Scan argument model](#9-scan-argument-model)
    18|10. [Structural scan grammar](#10-structural-scan-grammar)
    19|11. [Scan formatting APIs](#11-scan-formatting-apis)
    20|11.1. [Scan error code guide and recovery](#111-scan-error-code-guide-and-recovery)
    21|12. [Examples and misuse cases](#12-examples-and-misuse-cases)
    22|13. [Freestanding and build-mode notes](#13-freestanding-and-build-mode-notes)
    23|
    24|## 1. Design model
    25|
    26|The formatting side and the scanning side solve opposite problems.
    27|
    28|- Formatting takes typed values and renders text.
    29|- Scanning takes text and writes typed values.
    30|
    31|The project keeps both sides intentionally small:
    32|
    33|- formatting supports a compact placeholder language, positional reuse, simple alignment, width, and hex rendering for numeric values;
    34|- scanning supports typed destination pointers, strict placeholder counting, and literal matching with whitespace collapsing;
    35|- neither side tries to become a full `printf` or `scanf` clone.
    36|
    37|The practical result is that the APIs are easier to reason about than large general-purpose format engines, but the syntax is still expressive enough for common systems-code tasks.
    38|
    39|## 2. Formatter data model
    40|
    41|### `proven_fmt_result_t`
    42|
    43|```c
    44|typedef struct {
    45|    proven_err_t  err;
    46|    proven_size_t written;
    47|    proven_size_t required;
    48|} proven_fmt_result_t;
    49|```
    50|
    51|Meaning:
    52|
    53|- `err`: the status code for the operation.
    54|- `written`: bytes actually written into the destination.
    55|- `required`: total bytes needed for the full formatted output.
    56|
    57|Use `err` first. The other fields are most useful for truncating or partially successful operations.
    58|
    59|A successful result looks like this:
    60|
    61|```c
    62|proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
    63|    &s,
    64|    "hello {}",
    65|    PROVEN_ARG("world")
    66|);
    67|if (!proven_is_ok(r.err)) {
    68|    return r.err;
    69|}
    70|```
    71|
    72|A truncating result can still tell you how much more space you would have needed:
    73|
    74|```c
    75|proven_fmt_result_t r = proven_u8str_append_fmt_trunc(
    76|    &s,
    77|    "name={} score={}",
    78|    PROVEN_ARG("ada"),
    79|    PROVEN_ARG(42)
    80|);
    81|/* r.written and r.required are useful here. */
    82|```
    83|
    84|### `proven_arg_type_t`
    85|
    86|```c
    87|typedef enum {
    88|    PROVEN_ARG_NONE,
    89|    PROVEN_ARG_I32,
    90|    PROVEN_ARG_U32,
    91|    PROVEN_ARG_I64,
    92|    PROVEN_ARG_U64,
    93|#ifndef PROVEN_FMT_NO_FLOAT
    94|    PROVEN_ARG_F64,
    95|#endif
    96|    PROVEN_ARG_CSTR,
    97|    PROVEN_ARG_STR_VIEW,
    98|    PROVEN_ARG_DATETIME,
    99|    PROVEN_ARG_PTR,
   100|    PROVEN_ARG_FN,
   101|} proven_arg_type_t;
   102|```
   103|
   104|The formatter currently recognizes these value classes:
   105|
   106|- signed 32-bit integers
   107|- unsigned 32-bit integers
   108|- signed 64-bit integers
   109|- unsigned 64-bit integers
   110|- floating-point values, unless `PROVEN_FMT_NO_FLOAT` is defined
   111|- trusted C strings
   112|- borrowed U8 string views
   113|- datetimes
   114|- object pointers
   115|- function pointers
   116|
   117|### `proven_arg_t`
   118|
   119|```c
   120|typedef struct {
   121|    proven_arg_type_t type;
   122|    union {
   123|        proven_i32 i32;
   124|        proven_u32 u32;
   125|        proven_i64 i64;
   126|        proven_u64 u64;
   127|        double f64;
   128|        const char *cstr;
   129|        proven_u8str_view_t str_view;
   130|        proven_datetime_t datetime;
   131|        const void *ptr;
   132|        void (*fn)(void);
   133|    } value;
   134|} proven_arg_t;
   135|```
   136|
   137|The union field must match the selected `type`.
   138|Do not manufacture a `proven_arg_t` by writing a mismatched union field and hoping the formatter will guess.
   139|
   140|Wrong:
   141|
   142|```c
   143|proven_arg_t arg = {0};
   144|arg.type = PROVEN_ARG_I64;
   145|arg.value.u64 = 123; /* wrong: type and union field do not match */
   146|```
   147|
   148|Correct:
   149|
   150|```c
   151|proven_arg_t arg = proven_arg_i64(123);
   152|```
   153|
   154|## 3. Formatter constructors and selectors
   155|
   156|### Constructor summary
   157|
   158|| API | Parameters | Returns | Intent |
   159||---|---|---|---|
   160|| `proven_arg_none(void)` | none | `proven_arg_t` | Internal sentinel value. |
   161|| `proven_arg_i32(int v)` | signed integer | `proven_arg_t` | Render as 32-bit signed integer. |
   162|| `proven_arg_u32(unsigned int v)` | unsigned integer | `proven_arg_t` | Render as 32-bit unsigned integer. |
   163|| `proven_arg_i64(long long v)` | wide signed integer | `proven_arg_t` | Render as 64-bit signed integer. |
   164|| `proven_arg_u64(unsigned long long v)` | wide unsigned integer | `proven_arg_t` | Render as 64-bit unsigned integer. |
   165|| `proven_arg_f64(double v)` | floating-point value | `proven_arg_t` | Render as floating-point text, unless float formatting is disabled. |
   166|| `proven_arg_cstr(const char *v)` | trusted live C string | `proven_arg_t` | Render a NUL-terminated C string. |
   167|| `proven_arg_cstr_n(const char *v, proven_size_t max_len)` | possibly bounded C string | `proven_arg_t` | Render only up to `max_len` while searching for NUL. |
   168|| `proven_arg_str_view(proven_u8str_view_t v)` | borrowed U8 view | `proven_arg_t` | Render a borrowed view without assuming NUL termination. |
   169|| `proven_arg_datetime(proven_datetime_t v)` | datetime value | `proven_arg_t` | Render a datetime using the formatter's datetime rules. |
   170|| `proven_arg_ptr(const void *v)` | object pointer | `proven_arg_t` | Render the pointer value. |
   171|| `proven_arg_fn(void (*v)(void))` | function pointer | `proven_arg_t` | Render the raw function-pointer representation. |
   172|| `proven_arg_ucstr(const unsigned char *v)` | unsigned-char string | `proven_arg_t` | Convenience wrapper around `proven_arg_cstr`. |
   173|| `proven_arg_identity(proven_arg_t v)` | existing argument object | `proven_arg_t` | Pass-through helper. |
   174|
   175|### `PROVEN_ARG(x)`
   176|
   177|`PROVEN_ARG(x)` is the usual entry point.
   178|It uses `_Generic` so the compiler chooses a constructor from the type of `x`.
   179|
   180|The current mapping is:
   181|
   182|- `_Bool`, `char`, `signed char`, `short`, `int` -> `proven_arg_i32`
   183|- `unsigned char`, `unsigned short`, `unsigned int` -> `proven_arg_u32`
   184|- `long`, `long long` -> `proven_arg_i64`
   185|- `unsigned long`, `unsigned long long` -> `proven_arg_u64`
   186|- `double`, `float` -> `proven_arg_f64`, unless `PROVEN_FMT_NO_FLOAT` is defined
   187|- `const char *`, `char *` -> `proven_arg_cstr`
   188|- `unsigned char *`, `const unsigned char *` -> `proven_arg_ucstr`
   189|- `void *`, `const void *` -> `proven_arg_ptr`
   190|- `proven_u8str_view_t` -> `proven_arg_str_view`
   191|- `proven_datetime_t` -> `proven_arg_datetime`
   192|- `proven_arg_t` -> `proven_arg_identity`
   193|
   194|Important consequence:
   195|
   196|- `PROVEN_ARG` does not select function pointers.
   197|- Use `PROVEN_ARG_FN(f)` for function pointers.
   198|
   199|Wrong:
   200|
   201|```c
   202|void helper(void) {}
   203|proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(helper)); /* wrong */
   204|```
   205|
   206|Correct:
   207|
   208|```c
   209|proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG_FN(helper));
   210|```
   211|
   212|### `PROVEN_ARG_FN(f)`
   213|
   214|This macro exists so callers can pass function pointers without casting them through `void *`.
   215|It is a small safety wrapper around `proven_arg_fn`.
   216|
   217|Example:
   218|
   219|```c
   220|proven_fmt_result_t r = proven_u8str_append_fmt_grow(
   221|    alloc,
   222|    &s,
   223|    "callback = {}",
   224|    PROVEN_ARG_FN(helper)
   225|);
   226|```
   227|
   228|### `PROVEN_ARG_CSTR_N(v, max_len)`
   229|
   230|This macro is the bounded-string helper.
   231|Use it when the source may not be a fully trusted C string, but you still want C-string-like input handling.
   232|
   233|It scans for NUL only up to `max_len` and then formats the bounded prefix as a view.
   234|
   235|Good use case:
   236|
   237|```c
   238|const char *buf = get_network_buffer();
   239|proven_fmt_result_t r = proven_u8str_append_fmt_grow(
   240|    alloc,
   241|    &s,
   242|    "payload={}",
   243|    PROVEN_ARG_CSTR_N(buf, 128)
   244|);
   245|```
   246|
   247|Bad use case:
   248|
   249|```c
   250|const char *buf = get_network_buffer();
   251|proven_u8str_append_fmt_grow(alloc, &s, "{}", PROVEN_ARG(buf)); /* wrong if buf is not trusted */
   252|```
   253|
   254|### Float formatting note
   255|
   256|If `PROVEN_FMT_NO_FLOAT` is defined, float support is removed from the generic selector and the float constructor is not available.
   257|That is a compile-time configuration choice, not a runtime toggle.
   258|
   259|Current float rendering keeps a fixed six-digit fractional form for finite values, then switches to scientific notation when the magnitude is too large or too small for the compact form. The carry logic is bounded so values near a rounding boundary stay stable instead of expanding into an unbounded normalization loop.
   260|
   261|### Accuracy and limits
   262|
   263|- Floating-point output uses six fractional digits with round-half-up behavior.
   264|- The text form is intended for diagnostics and logs, not for round-trip serialization.
   265|- Decimal-to-double scanning is designed to stay exact within the implementation's limited power-of-ten range; outside that range, results are approximate but target-deterministic.
- Values below the smallest subnormal round to signed zero with the input sign preserved.
   266|
   267|## 4. Format string grammar
   268|
   269|The formatter accepts a deliberately small grammar.
   270|
   271|### Replacement fields
   272|
   273|Supported forms:
   274|
   275|- `{}`: next positional argument
   276|- `{0}`: first user argument
   277|- `{1}`: second user argument
   278|- `{2}`: third user argument
   279|- and so on
   280|
   281|The numbering is user-facing and zero-based.
   282|The implementation stores a hidden sentinel at index 0 and maps user index `0` to internal argument slot `1`.
   283|
   284|### Escaped braces
   285|
   286|- `{{` becomes a literal `{`
   287|- `}}` becomes a literal `}`
   288|
   289|### Alignment and width specifiers
   290|
   291|The formatter accepts a compact layout spec after `:`:
   292|
   293|```text
   294|{:fillalignwidthx}
   295|```
   296|
   297|More precisely:
   298|
   299|- optional fill character, followed by alignment
   300|- or alignment by itself
   301|- optional decimal width
   302|- optional trailing `x` for hexadecimal numeric rendering
   303|
   304|Supported alignment characters:
   305|
   306|- `<` left align
   307|- `>` right align
   308|- `^` center align
   309|
   310|Default behavior:
   311|
   312|- fill = space
   313|- align = right
   314|- width = 0
   315|- hex mode = off
   316|
   317|Examples:
   318|
   319|```c
   320|proven_u8str_append_fmt_grow(alloc, &s, "{:0>5}", PROVEN_ARG(42));    /* 00042 */
   321|proven_u8str_append_fmt_grow(alloc, &s, "{:*^10}", PROVEN_ARG("ok")); /* ****ok**** */
   322|proven_u8str_append_fmt_grow(alloc, &s, "{:.<10}", PROVEN_ARG("x"));  /* x......... */
   323|```
   324|
   325|### Hex mode
   326|
   327|A trailing `x` turns on lowercase hexadecimal rendering for numeric arguments.
   328|The formatter does not use uppercase hex and does not add a `0x` prefix for integer values.
   329|
   330|Example:
   331|
   332|```c
   333|proven_u8str_append_fmt_grow(alloc, &s, "0x{:x}", PROVEN_ARG(48879));
   334|```
   335|
   336|For signed integers, the numeric value is rendered through the implementation's unsigned conversion path when hex mode is enabled.
   337|That means negative numbers are shown in their unsigned representation rather than as a signed decimal value.
   338|
   339|### Width limit and invalid specs
   340|
   341|Width parsing is checked.
   342|Very large widths are rejected instead of silently wrapping.
   343|The current parser also rejects unknown format characters.
   344|
   345|Wrong:
   346|
   347|```c
   348|proven_u8str_append_fmt_grow(alloc, &s, "{:>9999999999}", PROVEN_ARG(123)); /* width too large */
   349|proven_u8str_append_fmt_grow(alloc, &s, "{:q}", PROVEN_ARG(123));            /* invalid spec */
   350|proven_u8str_append_fmt_grow(alloc, &s, "{", PROVEN_ARG(123));               /* invalid format */
   351|```
   352|
   353|### What the formatter does not support
   354|
   355|Do not expect these features:
   356|
   357|- precision fields
   358|- sign flags
   359|- alternate form flags such as `#`
   360|- locale-aware grouping
   361|- nested format language
   362|- Python-style format type families
   363|- full `printf` compatibility
   364|
   365|The project intentionally keeps the language small.
   366|
   367|### Type-specific rendering notes
   368|
   369|- integers render in base 10 unless hex mode is set
   370|- strings and string views are rendered as byte sequences
   371|- datetimes render using the datetime formatter in `time.h`
   372|- object pointers render as pointer text
   373|- function pointers render as raw representation bytes with a function-pointer prefix
   374|
   375|That means a spec like `:x` mostly matters for the numeric types.
   376|For strings, views, and datetimes, width and alignment are the important pieces.
   377|
   378|## 5. Formatting APIs
   379|
   380|### `proven_u8str_fmt_internal(...)`
   381|
   382|```c
   383|proven_fmt_result_t proven_u8str_fmt_internal(
   384|    proven_allocator_t alloc,
   385|    proven_u8str_t *str,
   386|    bool trunc,
   387|    const char *fmt,
   388|    proven_allocator_t scratch,
   389|    const proven_arg_t *args,
   390|    proven_size_t args_count
   391|);
   392|```
   393|
   394|This is the internal formatting engine.
   395|User code should normally call the public macros instead.
   396|
   397|Parameters:
   398|
   399|- `alloc`: allocator used when the string must grow
   400|- `str`: destination U8 string
   401|- `trunc`: if true, allow best-effort truncation; if false, keep atomic behavior
   402|- `fmt`: format text
   403|- `scratch`: allocator used for temporary alias-patching when needed
   404|- `args`: array of format arguments, including the hidden sentinel at index 0
   405|- `args_count`: total length of `args`, including the sentinel
   406|
   407|Return value:
   408|
   409|- a `proven_fmt_result_t`
   410|
   411|Important rules:
   412|
   413|- `args_count` must match the number of placeholders plus the hidden sentinel
   414|- extra unused arguments are an error
   415|- missing arguments are an error
   416|- if the engine detects aliasing between the destination string and a borrowed view argument, it may use the scratch allocator to preserve failure atomicity
   417|
   418|### `proven_u8str_append_fmt(str, fmt, ...)`
   419|
   420|Atomic formatting into a fixed-capacity string.
   421|If the result does not fit, the function reports failure and leaves the destination unchanged.
   422|
   423|Use this when you want all-or-nothing behavior.
   424|
   425|### `proven_u8str_append_fmt_trunc(str, fmt, ...)`
   426|
   427|Best-effort formatting.
   428|It writes as much as fits and reports how much was written and how much was required.
   429|
   430|Use this when partial output is acceptable.
   431|
   432|### `proven_u8str_append_fmt_grow(alloc, str, fmt, ...)`
   433|
   434|Growable formatting.
   435|It may reallocate the destination string through the supplied allocator.
   436|On allocation failure, the old string remains valid.
   437|
   438|Use this when you want the output to fit without manual capacity planning.
   439|
   440|### `proven_u8str_append_fmt_with_scratch(alloc, str, fmt, scratch, ...)`
   441|
   442|Growable formatting with a separate scratch allocator.
   443|This is useful when the argument list contains string views that may alias the destination buffer and temporary patching is needed.
   444|
   445|Use a real allocator for both `alloc` and `scratch`.
   446|Do not pass a dead arena or a one-shot temporary buffer unless its lifetime is long enough for the call.
   447|
   448|### `PROVEN_FMT_IS_OK(res)`
   449|
   450|A small helper macro for checking `proven_fmt_result_t`.
   451|Use it when you want the intent to stay compact.
   452|
   453|Example:
   454|
   455|```c
   456|proven_fmt_result_t r = proven_u8str_append_fmt_grow(
   457|    alloc,
   458|    &s,
   459|    "name={} score={:0>4}",
   460|    PROVEN_ARG("ada"),
   461|    PROVEN_ARG(42)
   462|);
   463|if (!PROVEN_FMT_IS_OK(r)) {
   464|    return r.err;
   465|}
   466|```
   467|
   468|### Console-style helpers
   469|
   470|The `sysio` layer provides print helpers that use the same formatter machinery:
   471|
   472|- `proven_print(fmt, ...)`
   473|- `proven_println(fmt, ...)`
   474|- `proven_eprint(fmt, ...)`
   475|- `proven_eprintln(fmt, ...)`
   476|
   477|They are convenient when you want formatted output directly to stdout or stderr.
   478|They still return `proven_err_t`, so check the result when the output matters.
   479|
   480|Example:
   481|
   482|```c
   483|if (!proven_is_ok(proven_println("hello {}", PROVEN_ARG("world")))) {
   484|    return 1;
   485|}
   486|```
   487|
   488|## 6. Console print helpers
   489|
   490|This section is intentionally short because the detailed I/O API lives in Chapter 5.
   491|The important point for formatter users is that the console helpers share the same argument rules as the string append APIs.
   492|
   493|Common mistakes:
   494|
   495|- using `PROVEN_SCAN_ARG` with `proven_println`
   496|- assuming `PROVEN_LIT` is needed for every format string
   497|- forgetting that output functions can still fail
   498|
   499|## 7. Scanner data model
   500|
   501|
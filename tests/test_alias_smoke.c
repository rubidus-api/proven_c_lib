#include "proven.h"
#include "proven_test.h"
#include "proven/alias_xcv.h"

static void xcv_alias_test_fn(void) {}

int main(void) {
    /* Test scan native integer aliases */
    (void)xcv_scan_arg_short;
    (void)xcv_scan_arg_ushort;
    (void)xcv_scan_arg_int;
    (void)xcv_scan_arg_uint;
    (void)xcv_scan_arg_long;
    (void)xcv_scan_arg_ulong;
    (void)xcv_scan_arg_llong;
    (void)xcv_scan_arg_ullong;

    /* Test scan function aliases */
    (void)xcv_scan_fmt_internal;
    (void)xcv_scan_fmt_internal_view;
    (void)xcv_u8str_fmt_internal;

    /* Test arg alias compilation */
    xcv_arg_t a01 = XCV_ARG(42);
    xcv_arg_t a02 = XCV_ARG_FN(xcv_alias_test_fn);
    xcv_arg_t a03 = XCV_ARG_CSTR("hi");
    xcv_arg_t a04 = XCV_ARG_PTR((void*)0);
    xcv_arg_t a05 = XCV_ARG_I32(1);
    xcv_arg_t a06 = XCV_ARG_I64(2);
    xcv_arg_t a07 = XCV_ARG_U32(3u);
    xcv_arg_t a08 = XCV_ARG_U64(4u);

    (void)a01; (void)a02; (void)a03; (void)a04;
    (void)a05; (void)a06; (void)a07; (void)a08;
    (void)xcv_arg_fn;

    /* Test macro alias compilation */
    int e0 = XCV_ERR_NEED_MORE;
    int t0  = XCV_SCAN_ARG_TYPE_NONE;
    int t1  = XCV_SCAN_ARG_TYPE_I32;
    int t2  = XCV_SCAN_ARG_TYPE_U32;
    int t3  = XCV_SCAN_ARG_TYPE_I64;
    int t4  = XCV_SCAN_ARG_TYPE_U64;
    int t5  = XCV_SCAN_ARG_TYPE_SHORT;
    int t6  = XCV_SCAN_ARG_TYPE_USHORT;
    int t7  = XCV_SCAN_ARG_TYPE_INT;
    int t8  = XCV_SCAN_ARG_TYPE_UINT;
    int t9  = XCV_SCAN_ARG_TYPE_LONG;
    int t10 = XCV_SCAN_ARG_TYPE_ULONG;
    int t11 = XCV_SCAN_ARG_TYPE_LLONG;
    int t12 = XCV_SCAN_ARG_TYPE_ULLONG;
    int t13 = XCV_SCAN_ARG_TYPE_F64;
    int t14 = XCV_SCAN_ARG_TYPE_STR_VIEW;

    (void)e0;
    (void)t0;  (void)t1;  (void)t2;  (void)t3;  (void)t4;
    (void)t5;  (void)t6;  (void)t7;  (void)t8;  (void)t9;
    (void)t10; (void)t11; (void)t12; (void)t13; (void)t14;

    short s = 0;
    xcv_scan_arg_t a1 = XCV_SCAN_ARG(&s);
    (void)a1;

    long l = 0;
    xcv_scan_arg_t a2 = XCV_SCAN_ARG(&l);
    (void)a2;

    unsigned long ul = 0;
    xcv_scan_arg_t a3 = XCV_SCAN_ARG(&ul);
    (void)a3;

    PROVEN_TEST_PASS("Test Alias Smoke: All selected alias macros expanded and compiled successfully.");
    return 0;
}

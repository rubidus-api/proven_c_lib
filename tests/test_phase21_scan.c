#include "proven.h"
#include "proven_test.h"
#include "proven/scan.h"
#include "proven/sysio.h"

#include <math.h>
#include <limits.h>

int main(void) {
    // Test 1: Simple Integer Scanning
    PROVEN_TEST_INFO("Testing simple integer scanning...");
    {
        proven_u8str_view_t input = PROVEN_LIT("  123 -456 789  ");
        proven_scan_t scan = proven_scan_init(input);

        proven_result_u64_t u1 = proven_scan_u64(&scan);
        PROVEN_TEST_ASSERT(u1.err == PROVEN_OK && u1.val == 123, "Testing condition: u1.err == PROVEN_OK && u1.val == 123", "Review logic surrounding u1.err == PROVEN_OK && u1.val == 123");

        proven_result_i64_t i1 = proven_scan_i64(&scan);
        PROVEN_TEST_ASSERT(i1.err == PROVEN_OK && i1.val == -456, "Testing condition: i1.err == PROVEN_OK && i1.val == -456", "Review logic surrounding i1.err == PROVEN_OK && i1.val == -456");

        proven_result_u64_t u2 = proven_scan_u64(&scan);
        PROVEN_TEST_ASSERT(u2.err == PROVEN_OK && u2.val == 789, "Testing condition: u2.err == PROVEN_OK && u2.val == 789", "Review logic surrounding u2.err == PROVEN_OK && u2.val == 789");
    }

    // Test 2: Floating Point Scanning
    PROVEN_TEST_INFO("Testing floating point scanning...");
    {
        proven_u8str_view_t input = PROVEN_LIT(" 3.14159 -0.001 2.5e2 ");
        proven_scan_t scan = proven_scan_init(input);

        proven_result_f64_t f1 = proven_scan_f64(&scan);
        PROVEN_TEST_ASSERT(f1.err == PROVEN_OK, "Testing condition: f1.err == PROVEN_OK", "Review logic surrounding f1.err == PROVEN_OK");
        PROVEN_TEST_ASSERT(f1.val > 3.1415 && f1.val < 3.1416, "Testing condition: f1.val > 3.1415 && f1.val < 3.1416", "Review logic surrounding f1.val > 3.1415 && f1.val < 3.1416");

        proven_result_f64_t f2 = proven_scan_f64(&scan);
        PROVEN_TEST_ASSERT(f2.err == PROVEN_OK, "Testing condition: f2.err == PROVEN_OK", "Review logic surrounding f2.err == PROVEN_OK");
        // Note: exact comparison for floats is tricky, but -0.001 should be close
        PROVEN_TEST_ASSERT(f2.val < -0.0009 && f2.val > -0.0011, "Testing condition: f2.val < -0.0009 && f2.val > -0.0011", "Review logic surrounding f2.val < -0.0009 && f2.val > -0.0011");

        proven_result_f64_t f3 = proven_scan_f64(&scan);
        PROVEN_TEST_ASSERT(f3.err == PROVEN_OK, "Testing condition: f3.err == PROVEN_OK", "Review logic surrounding f3.err == PROVEN_OK");
        PROVEN_TEST_ASSERT(f3.val == 250.0, "Testing condition: f3.val == 250.0", "Review logic surrounding f3.val == 250.0");
    }

    // Test 3: Token Scanning
    PROVEN_TEST_INFO("Testing token scanning...");
    {
        proven_u8str_view_t input = PROVEN_LIT("hello   world\nproven");
        proven_scan_t scan = proven_scan_init(input);

        proven_result_u8str_view_t t1 = proven_scan_str(&scan);
        PROVEN_TEST_ASSERT(t1.err == PROVEN_OK, "Testing condition: t1.err == PROVEN_OK", "Review logic surrounding t1.err == PROVEN_OK");
        PROVEN_TEST_ASSERT(t1.val.size == 5 && t1.val.ptr[0] == 'h', "Testing condition: t1.val.size == 5 && t1.val.ptr[0] == 'h'", "Review logic surrounding t1.val.size == 5 && t1.val.ptr[0] == 'h'");

        proven_result_u8str_view_t t2 = proven_scan_str(&scan);
        PROVEN_TEST_ASSERT(t2.err == PROVEN_OK, "Testing condition: t2.err == PROVEN_OK", "Review logic surrounding t2.err == PROVEN_OK");
        PROVEN_TEST_ASSERT(t2.val.size == 5 && t2.val.ptr[0] == 'w', "Testing condition: t2.val.size == 5 && t2.val.ptr[0] == 'w'", "Review logic surrounding t2.val.size == 5 && t2.val.ptr[0] == 'w'");

        proven_result_u8str_view_t t3 = proven_scan_str(&scan);
        PROVEN_TEST_ASSERT(t3.err == PROVEN_OK, "Testing condition: t3.err == PROVEN_OK", "Review logic surrounding t3.err == PROVEN_OK");
        PROVEN_TEST_ASSERT(t3.val.size == 6 && t3.val.ptr[0] == 'p', "Testing condition: t3.val.size == 6 && t3.val.ptr[0] == 'p'", "Review logic surrounding t3.val.size == 6 && t3.val.ptr[0] == 'p'");
    }

    // Test 4: Skip Until
    PROVEN_TEST_INFO("Testing skip until substring...");
    {
        proven_u8str_view_t input = PROVEN_LIT("seek result: 42; garbage: xxxxx");
        proven_scan_t scan = proven_scan_init(input);

        proven_err_t err = proven_scan_skip_until(&scan, PROVEN_LIT("42"));
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "Testing condition: err == PROVEN_OK", "Review logic surrounding err == PROVEN_OK");
        PROVEN_TEST_ASSERT(scan.cursor == 13, "Testing condition: scan.cursor == 13", "Review logic surrounding scan.cursor == 13"); // "seek result: " is 13 chars

        proven_result_u64_t res = proven_scan_u64(&scan);
        PROVEN_TEST_ASSERT(res.err == PROVEN_OK && res.val == 42, "Testing condition: res.err == PROVEN_OK && res.val == 42", "Review logic surrounding res.err == PROVEN_OK && res.val == 42");
        
        // Test not found
        err = proven_scan_skip_until(&scan, PROVEN_LIT("not_here"));
        PROVEN_TEST_ASSERT(err == PROVEN_ERR_NOT_FOUND, "Testing condition: err == PROVEN_ERR_NOT_FOUND", "Review logic surrounding err == PROVEN_ERR_NOT_FOUND");
    }

    // Test 5: Skip Until Number
    PROVEN_TEST_INFO("Testing skip until number...");
    {
        proven_u8str_view_t input = PROVEN_LIT("  some text and then -- -42 is here, and +3.14!");
        proven_scan_t scan = proven_scan_init(input);

        proven_scan_skip_until_number(&scan);
        
        proven_result_i64_t res = proven_scan_i64(&scan);
        PROVEN_TEST_ASSERT(res.err == PROVEN_OK && res.val == -42, "Testing condition: res.err == PROVEN_OK && res.val == -42", "Review logic surrounding res.err == PROVEN_OK && res.val == -42");
        
        proven_scan_skip_until_number(&scan);
        proven_result_f64_t res2 = proven_scan_f64(&scan);
        PROVEN_TEST_ASSERT(res2.err == PROVEN_OK && res2.val > 3.13 && res2.val < 3.15, "Testing condition: res2.err == PROVEN_OK && res2.val > 3.13 && res2.val < 3.15", "Review logic surrounding res2.err == PROVEN_OK && res2.val > 3.13 && res2.val < 3.15");
    }

    // Test 6: format scanning
    PROVEN_TEST_INFO("Testing format scanning ({}, {spec})...");
    {
        proven_u8str_view_t input = PROVEN_LIT("ID: 402, SCORE: 99.5 username");
        proven_scan_t scan = proven_scan_init(input);

        int id = 0;
        double score = 0.0;
        proven_u8str_view_t user = {0};

        // Note: Spaces in the format string automatically skip whitespace in the input
        proven_err_t err = proven_scan_fmt_cursor(&scan, "ID: {}, SCORE: {} {}", 
            PROVEN_SCAN_ARG(&id), PROVEN_SCAN_ARG(&score), PROVEN_SCAN_ARG(&user));
            
        PROVEN_TEST_ASSERT(err == PROVEN_OK, "Testing condition: err == PROVEN_OK", "Review logic surrounding err == PROVEN_OK");
        PROVEN_TEST_ASSERT(id == 402, "Testing condition: id == 402", "Review logic surrounding id == 402");
        PROVEN_TEST_ASSERT(score > 99.4 && score < 99.6, "Testing condition: score > 99.4 && score < 99.6", "Review logic surrounding score > 99.4 && score < 99.6");
        PROVEN_TEST_ASSERT(user.size == 8 && user.ptr[0] == 'u' && user.ptr[7] == 'e', "Testing condition: user.size == 8 && user.ptr[0] == 'u' && user.ptr[7] == 'e'", "Review logic surrounding user.size == 8 && user.ptr[0] == 'u' && user.ptr[7] == 'e'");
    }

    // Test 6.1: Native integer scan
    PROVEN_TEST_INFO("Testing native integer scan...");
    {
        short s = 0;
        int i = 0;
        long l = 0;
        long long ll = 0;

        unsigned short us = 0;
        unsigned int ui = 0;
        unsigned long ul = 0;
        unsigned long long ull = 0;

        proven_err_t e = proven_scan_fmt(
            PROVEN_LIT("1 2 3 4 5 6 7 8"),
            "{} {} {} {} {} {} {} {}",
            PROVEN_SCAN_ARG(&s),
            PROVEN_SCAN_ARG(&i),
            PROVEN_SCAN_ARG(&l),
            PROVEN_SCAN_ARG(&ll),
            PROVEN_SCAN_ARG(&us),
            PROVEN_SCAN_ARG(&ui),
            PROVEN_SCAN_ARG(&ul),
            PROVEN_SCAN_ARG(&ull)
        );

        PROVEN_TEST_ASSERT(e == PROVEN_OK, "native integer scan", "");
        PROVEN_TEST_ASSERT(s == 1, "short scan", "");
        PROVEN_TEST_ASSERT(i == 2, "int scan", "");
        PROVEN_TEST_ASSERT(l == 3, "long scan", "");
        PROVEN_TEST_ASSERT(ll == 4, "long long scan", "");
        PROVEN_TEST_ASSERT(us == 5, "unsigned short scan", "");
        PROVEN_TEST_ASSERT(ui == 6, "unsigned int scan", "");
        PROVEN_TEST_ASSERT(ul == 7, "unsigned long scan", "");
        PROVEN_TEST_ASSERT(ull == 8, "unsigned long long scan", "");
    }

    // Test 6.2: fixed proven integer generic scan
    PROVEN_TEST_INFO("Testing fixed-width proven integer scan...");
    {
        proven_i32 i32 = 0;
        proven_i64 i64 = 0;
        proven_u32 u32 = 0;
        proven_u64 u64 = 0;

        proven_err_t e = proven_scan_fmt(
            PROVEN_LIT("-1 -2 3 4"),
            "{} {} {} {}",
            PROVEN_SCAN_ARG(&i32),
            PROVEN_SCAN_ARG(&i64),
            PROVEN_SCAN_ARG(&u32),
            PROVEN_SCAN_ARG(&u64)
        );

        PROVEN_TEST_ASSERT(e == PROVEN_OK, "fixed proven integer generic scan", "");
        PROVEN_TEST_ASSERT(i32 == -1, "i32 scan", "");
        PROVEN_TEST_ASSERT(i64 == -2, "i64 scan", "");
        PROVEN_TEST_ASSERT(u32 == 3, "u32 scan", "");
        PROVEN_TEST_ASSERT(u64 == 4, "u64 scan", "");
    }

    // Test 6.3: Native integer overflow rollback
    PROVEN_TEST_INFO("Testing native integer overflow rollback...");
    {
        // short
        {
            proven_scan_t scan = proven_scan_init(PROVEN_LIT("32768"));
            short s = 123;
            proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&s));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "short overflow", "");
            PROVEN_TEST_ASSERT(scan.cursor == 0, "short overflow rollback", "");
            PROVEN_TEST_ASSERT(s == 123, "short value unchanged", "");
        }
        
        // unsigned short
        {
            proven_scan_t scan = proven_scan_init(PROVEN_LIT("65536"));
            unsigned short us = 123;
            proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&us));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "unsigned short overflow", "");
            PROVEN_TEST_ASSERT(scan.cursor == 0, "unsigned short overflow rollback", "");
            PROVEN_TEST_ASSERT(us == 123, "unsigned short value unchanged", "");
        }

#if INT_MAX == 2147483647
        // int (assuming 32-bit int here, so 2147483648 is overflow)
        {
            proven_scan_t scan = proven_scan_init(PROVEN_LIT("2147483648"));
            int i = 123;
            proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&i));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "int overflow", "");
            // rollback happens to index 0
            PROVEN_TEST_ASSERT(scan.cursor == 0, "int overflow rollback", "");
            PROVEN_TEST_ASSERT(i == 123, "int value unchanged", "");
        }
#endif

#if UINT_MAX == 4294967295u
        // unsigned int (assuming 32-bit uint here, so 4294967296 is overflow)
        {
            proven_scan_t scan = proven_scan_init(PROVEN_LIT("4294967296"));
            unsigned int ui = 123;
            proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&ui));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "unsigned int overflow", "");
            PROVEN_TEST_ASSERT(scan.cursor == 0, "unsigned int overflow rollback", "");
            PROVEN_TEST_ASSERT(ui == 123, "unsigned int value unchanged", "");
        }
#endif

#if LONG_MAX == 2147483647L
        // 32-bit long overflow
        {
            proven_scan_t scan = proven_scan_init(PROVEN_LIT("2147483648"));
            long l = 123;
            proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&l));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "long 32-bit overflow", "");
            PROVEN_TEST_ASSERT(scan.cursor == 0, "long 32-bit overflow rollback", "");
            PROVEN_TEST_ASSERT(l == 123, "long 32-bit value unchanged", "");
        }
#else
        // 64-bit long overflow (we use a literal that overflows 64-bit signed int)
        {
            proven_scan_t scan = proven_scan_init(PROVEN_LIT("9223372036854775808"));
            long l = 123;
            proven_err_t e = proven_scan_fmt_cursor(&scan, "{}", PROVEN_SCAN_ARG(&l));
            PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "long 64-bit overflow", "");
            PROVEN_TEST_ASSERT(scan.cursor == 0, "long 64-bit overflow rollback", "");
            PROVEN_TEST_ASSERT(l == 123, "long 64-bit value unchanged", "");
        }
#endif
    }

    // Test 6.4: Overflow rollback before whitespace
    PROVEN_TEST_INFO("Testing overflow rollback before whitespace...");
    {
        proven_scan_t scan = proven_scan_init(PROVEN_LIT(" 999999999999999999999999"));
        int value = 123;

        proven_err_t e = proven_scan_fmt_cursor(
            &scan,
            "{}",
            PROVEN_SCAN_ARG(&value)
        );

        PROVEN_TEST_ASSERT(e == PROVEN_ERR_OVERFLOW, "overflow detected", "");
        PROVEN_TEST_ASSERT(scan.cursor == 0, "cursor rolled back to argument start (before space)", "");
        PROVEN_TEST_ASSERT(value == 123, "destination unchanged", "");
    }

    // Test 6.5: NULL destination rejection
    PROVEN_TEST_INFO("Testing NULL destination rejection...");
    {
        proven_scan_t scan = proven_scan_init(PROVEN_LIT("123"));

        proven_err_t e = proven_scan_fmt_cursor(
            &scan,
            "{}",
            PROVEN_SCAN_ARG((int *)NULL)
        );

        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG, "null scan destination rejected", "");
        PROVEN_TEST_ASSERT(scan.cursor == 0, "cursor unchanged on null destination", "");
    }

    // Test 6.6: Extra argument rejection (atomicity)
    PROVEN_TEST_INFO("Testing extra argument rejection (atomicity)...");
    {
        proven_scan_t scan = proven_scan_init(PROVEN_LIT("123"));
        int a = 0;
        int b = 0;

        proven_err_t e = proven_scan_fmt_cursor(
            &scan,
            "{}",
            PROVEN_SCAN_ARG(&a),
            PROVEN_SCAN_ARG(&b)
        );

        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG, "extra arg rejected before scan", "");
        PROVEN_TEST_ASSERT(scan.cursor == 0, "cursor unchanged on extra arg", "");
        PROVEN_TEST_ASSERT(a == 0, "first destination unchanged on extra arg", "");
        PROVEN_TEST_ASSERT(b == 0, "second destination unchanged on extra arg", "");
    }

    // Test 6.7: Missing argument rejection
    PROVEN_TEST_INFO("Testing missing argument rejection...");
    {
        proven_scan_t scan = proven_scan_init(PROVEN_LIT("123 456"));
        int a = 0;

        proven_err_t e = proven_scan_fmt_cursor(
            &scan,
            "{} {}",
            PROVEN_SCAN_ARG(&a)
        );

        PROVEN_TEST_ASSERT(e == PROVEN_ERR_INVALID_ARG, "missing arg rejected before scan", "");
        PROVEN_TEST_ASSERT(scan.cursor == 0, "cursor unchanged on missing arg", "");
        PROVEN_TEST_ASSERT(a == 0, "destination unchanged on missing arg", "");
    }

    // Test 7: format scanning directly from string view using proven_scan_fmt
    PROVEN_TEST_INFO("Testing direct format scanning from view...");
    {
        proven_u8str_view_t input = PROVEN_LIT("VAL: -123.45 test_string");

        double val = 0.0;
        proven_u8str_view_t phrase = {0};

        proven_err_t err = proven_scan_fmt(input, "VAL: {} {}", 
            PROVEN_SCAN_ARG(&val), PROVEN_SCAN_ARG(&phrase));

        PROVEN_TEST_ASSERT(err == PROVEN_OK, "Testing condition: err == PROVEN_OK", "Review logic surrounding err == PROVEN_OK");
        PROVEN_TEST_ASSERT(val < -123.4 && val > -123.5, "Testing condition: val < -123.4 && val > -123.5", "Review logic surrounding val < -123.4 && val > -123.5");
        PROVEN_TEST_ASSERT(phrase.size == 11 && phrase.ptr[0] == 't', "Testing condition: phrase.size == 11 && phrase.ptr[0] == 't'", "Review logic surrounding phrase.size == 11 && phrase.ptr[0] == 't'");
    }

    // Test 8: Security and edge cases for f64 parsing
    PROVEN_TEST_INFO("Testing f64 scanning security and edge cases...");
    {
        proven_scan_t scan1 = proven_scan_init(PROVEN_LIT("1e 5"));
        proven_result_f64_t res1 = proven_scan_f64(&scan1);
        PROVEN_TEST_ASSERT(res1.err == PROVEN_ERR_INVALID_ARG || res1.err == PROVEN_ERR_OUT_OF_BOUNDS, 
                           "Whitespace inside exponent must be blocked", "Fix proven_scan_f64");

        proven_scan_t scan2 = proven_scan_init(PROVEN_LIT("1e9999999"));
        proven_result_f64_t res2 = proven_scan_f64(&scan2);
        PROVEN_TEST_ASSERT(res2.err == PROVEN_ERR_OUT_OF_BOUNDS, 
                           "Large exponent must be blocked to prevent DoS", "Limit e in proven_scan_f64");
                           
        proven_scan_t scan3 = proven_scan_init(PROVEN_LIT("1e+5"));
        proven_result_f64_t res3 = proven_scan_f64(&scan3);
        PROVEN_TEST_ASSERT(res3.err == PROVEN_OK && res3.val > 99999.0 && res3.val < 100001.0, 
                           "Valid exponent parsed correctly", "Fix e parsing");
    }

    PROVEN_TEST_INFO("Test Phase 21: Scan logic passed.");
    return 0;
}

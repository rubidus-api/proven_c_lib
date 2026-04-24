#include "proven.h"
#include "proven_test.h"
#include "proven/scan.h"
#include "proven/sysio.h"

#include <math.h>

int main(void) {
    // Test 1: Simple Integer Scanning
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

    // Test 7: format scanning directly from string view using proven_scan_fmt
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

    PROVEN_TEST_INFO("Test Phase 21: Scan logic passed.");
    return 0;
}

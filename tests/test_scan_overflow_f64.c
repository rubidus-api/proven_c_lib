#include "proven_test.h"
#include "proven/scan.h"
#include <math.h>

int main(void) {
    // Test case: 1000 '9's which should overflow double to inf
    char large_num[1001];
    for (int i = 0; i < 1000; i++) {
        large_num[i] = '9';
    }
    large_num[1000] = '\0';

    proven_scan_t scan = {
        .view = proven_u8str_view_from_cstr(large_num),
        .cursor = 0
    };

    proven_result_f64_t res = proven_scan_f64(&scan);
    
    PROVEN_TEST_INFO("Scan result: err={}, val={}", PROVEN_ARG((proven_i32)res.err), PROVEN_ARG(res.val));
    
    // Before fix, this might return PROVEN_OK (0) and val = inf
    // After fix, it should return PROVEN_ERR_OVERFLOW
    PROVEN_TEST_ASSERT(res.err == PROVEN_ERR_OVERFLOW, "Very large number should return PROVEN_ERR_OVERFLOW", "Review overflow detection in proven_scan_f64");
    
    PROVEN_TEST_PASS("Scan F64 Overflow Test Passed!");
    return 0;
}

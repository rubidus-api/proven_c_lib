#include "proven.h"
#include "proven_test.h"
#include <stdio.h>
#include <string.h>

/*
 * Manual chapter 8 documents the scanner's exact behaviour: which error code each
 * failure produces, whether the cursor is restored, that 0x10 is decimal zero,
 * that overflow refuses while underflow does not, and that a failed structural
 * scan has ALREADY written through the destinations before the mismatch.
 *
 * Those are contracts, and a contract that only exists in prose is a contract
 * that drifts. Each assertion below is a sentence the chapter states as fact.
 * If one fails, the chapter is lying to a reader - fix whichever is wrong.
 */
#define V(s) proven_u8str_view_from_cstr(s)
static int bad = 0;
#define CLAIM(c, msg)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            printf("[PROVEN][TEST][INFO] chapter 8 claims, falsely: %s\n", msg); \
            ++bad;                                                             \
        }                                                                      \
    } while (0)
int main(void){
  PROVEN_TEST_SUITE("manual chapter 8 scanner contracts",
      "Every behaviour manual chapter 8 states as fact about the scanner must actually be true.",
      "A failing claim is printed above. Either the scanner changed and the chapter was not updated, or the chapter was wrong when it was written.");

  PROVEN_TEST_SECTION("the chapter's factual claims about scan behaviour",
      "Error codes, cursor restoration, decimal-only integers, the overflow/underflow asymmetry, and the non-transactional structural scan.",
      "Find the claim named above in manual/manual-08-fmt-scan.md and decide which side is wrong before changing either.");
  { proven_scan_t s=proven_scan_init(V("  42")); proven_result_i64_t r=proven_scan_i64(&s);
    CLAIM(proven_is_ok(r.err)&&r.val==42, "value scanners skip leading whitespace"); }
  { proven_scan_t s=proven_scan_init(V("12abc")); proven_result_i64_t r=proven_scan_i64(&s);
    CLAIM(proven_is_ok(r.err)&&r.val==12&&s.cursor==2, "scanning stops at the first byte that cannot belong"); }
  { proven_scan_t s=proven_scan_init(V("abc")); proven_result_i64_t r=proven_scan_i64(&s);
    CLAIM(r.err==PROVEN_ERR_INVALID_ARG&&s.cursor==0, "on failure the cursor is restored"); }
  { proven_scan_t s=proven_scan_init(V("9223372036854775808")); proven_result_i64_t r=proven_scan_i64(&s);
    CLAIM(r.err==PROVEN_ERR_OVERFLOW, "one past INT64_MAX is OVERFLOW, not a wrap"); }
  { proven_scan_t s=proven_scan_init(V("0x10")); proven_result_i64_t r=proven_scan_i64(&s);
    CLAIM(proven_is_ok(r.err)&&r.val==0&&s.cursor==1, "0x10 scans as decimal 0, cursor at 1"); }
  { proven_scan_t s=proven_scan_init(V("-1")); proven_result_u64_t r=proven_scan_u64(&s);
    CLAIM(r.err==PROVEN_ERR_INVALID_ARG, "u64 rejects -1 rather than wrapping"); }
  { proven_scan_t s=proven_scan_init(V("1e309")); proven_result_f64_t r=proven_scan_f64(&s);
    CLAIM(r.err==PROVEN_ERR_OVERFLOW, "1e309 is OVERFLOW"); }
  { proven_scan_t s=proven_scan_init(V("-1e-400")); proven_result_f64_t r=proven_scan_f64(&s);
    CLAIM(proven_is_ok(r.err)&&r.val==0.0, "1e-400 underflows to zero with no error"); }
  { proven_scan_t s=proven_scan_init(V("nan")); proven_result_f64_t r=proven_scan_f64(&s);
    CLAIM(proven_is_ok(r.err), "nan is accepted"); }
  { proven_scan_t s=proven_scan_init(V("   ")); proven_result_u8str_view_t r=proven_scan_str(&s);
    CLAIM(r.err==PROVEN_ERR_INVALID_ARG, "whitespace-only input is INVALID_ARG for scan_str"); }
  { proven_scan_t s=proven_scan_init(V("port=8080")); proven_err_t e=proven_scan_skip_until(&s,PROVEN_LIT("="));
    CLAIM(proven_is_ok(e)&&s.cursor==4, "skip_until leaves the cursor ON the target"); }
  { proven_scan_t s=proven_scan_init(V("port=8080")); proven_err_t e=proven_scan_skip_until(&s,PROVEN_LIT("#"));
    CLAIM(e==PROVEN_ERR_NOT_FOUND&&s.cursor==0, "a missing target is NOT_FOUND and the cursor does not move"); }
  { proven_scan_t s=proven_scan_init(V("abc-x")); proven_scan_skip_until_number(&s);
    CLAIM(s.cursor==s.view.size, "skip_until_number runs to the end when there is no number"); }
  { int id=-1; double ra=-1; proven_err_t e=proven_scan_fmt(V("id=7 ratio=0.5"),"id={} XXX={}",PROVEN_SCAN_ARG(&id),PROVEN_SCAN_ARG(&ra));
    CLAIM(e==PROVEN_ERR_NOT_FOUND&&id==7, "a literal mismatch is NOT_FOUND, and earlier destinations are already written"); }
  { int a=0,b=0; proven_err_t e=proven_scan_fmt(V("5"),"{} {}",PROVEN_SCAN_ARG(&a),PROVEN_SCAN_ARG(&b));
    CLAIM(e==PROVEN_ERR_INVALID_ARG, "running out of input is INVALID_ARG"); }
  { int n=0; proven_scan_t s=proven_scan_init(V("7 8")); proven_err_t e=proven_scan_fmt_cursor(&s,"{}",PROVEN_SCAN_ARG(&n));
    CLAIM(proven_is_ok(e)&&n==7&&s.cursor<s.view.size, "trailing input is not an error"); }
  { short sh=0; proven_err_t e=proven_scan_fmt(V("70000"),"{}",PROVEN_SCAN_ARG(&sh));
    CLAIM(e==PROVEN_ERR_OVERFLOW, "a narrow destination is range-checked, not truncated"); }
  { int a=0,b=0; proven_err_t e1=proven_scan_fmt(V("7 8"),"{} {}",PROVEN_SCAN_ARG(&a),PROVEN_SCAN_ARG(&b));
    int c=0,d=0; proven_err_t e2=proven_scan_fmt(V("7 8"),"{}{}",PROVEN_SCAN_ARG(&c),PROVEN_SCAN_ARG(&d));
    CLAIM(proven_is_ok(e1)&&proven_is_ok(e2)&&a==c&&b==d, "'{} {}' and '{}{}' parse '7 8' identically"); }
  PROVEN_TEST_ASSERT(bad == 0,
      "manual chapter 8 states something about the scanner that is not true (listed above)",
      "The chapter is a contract. Fix the chapter, or fix the scanner - but do not leave them disagreeing.");

  PROVEN_TEST_PASS("every scanner behaviour chapter 8 documents is real.");
  return 0;
}

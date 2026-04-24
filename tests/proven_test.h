#ifndef PROVEN_TEST_H
#define PROVEN_TEST_H

#include "proven.h"
#include <stdlib.h>

#define PROVEN_TEST_ASSERT(cond, msg, hint) \
    do { \
        if (!(cond)) { \
            proven_eprintln("\n[TEST FAILED] {}:{}", PROVEN_ARG(PROVEN_LIT(__FILE__)), PROVEN_ARG(__LINE__)); \
            proven_eprintln("  Condition : {}", PROVEN_ARG(PROVEN_LIT(#cond))); \
            proven_eprintln("  Intent    : {}", PROVEN_ARG(PROVEN_LIT(msg))); \
            proven_eprintln("  Fix Hint  : {}", PROVEN_ARG(PROVEN_LIT(hint))); \
            exit(1); \
        } \
    } while(0)

#define PROVEN_TEST_INFO(fmt, ...) \
    proven_println("[INFO] " fmt, ##__VA_ARGS__)

#endif // PROVEN_TEST_H

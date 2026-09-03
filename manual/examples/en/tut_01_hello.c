#include "example.h"

/*
 * Lesson 1 - get one line to build and run.
 *
 * Nothing here is about the library's ideas yet. The only question this program
 * answers is "does my build command work?", and it is worth answering on its
 * own, because every later lesson assumes it.
 *
 * printf(fmt, ...) is not checked: the compiler cannot tell you that "%d" was
 * handed a double. proven_println checks, because every argument is wrapped by
 * PROVEN_ARG and carries its own type.
 */

int main(void) {
    proven_println("hello from proven");

    /* {} is the placeholder. The value goes through PROVEN_ARG, which records
     * what type it is, so the format string and the argument cannot disagree. */
    proven_println("one number: {}", PROVEN_ARG(42));
    proven_println("two of them: {} and {}", PROVEN_ARG(1), PROVEN_ARG(2));

    return EXAMPLE_OK();
}

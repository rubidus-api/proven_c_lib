#ifndef PROVEN_MATH_CONFIG_H
#define PROVEN_MATH_CONFIG_H

/*
 * Compile-time switches of proven_math.
 *
 * PROVEN_FLOAT_SOFT=1 makes every function of float_ops.h and float_math.h use its integer
 * implementation on every target, the square root included. The bits are the same either way;
 * set it for a program that changes the floating-point rounding mode or turns on flush-to-zero
 * (the instruction path assumes the default environment), or to measure the integer path.
 */
#ifndef PROVEN_FLOAT_SOFT
#define PROVEN_FLOAT_SOFT 0
#endif

/*
 * PROVEN_FLOAT_MATH_SMALL=1 leaves out float_math.h's FPU path (the CORE-MATH code and its
 * tables, some hundreds of KiB of object code): every function takes the integer path, which
 * is a few KiB and gives the same bits, more slowly.
 */
#ifndef PROVEN_FLOAT_MATH_SMALL
#define PROVEN_FLOAT_MATH_SMALL 0
#endif

#endif /* PROVEN_MATH_CONFIG_H */

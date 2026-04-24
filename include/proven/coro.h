#ifndef PROVEN_CORO_H
#define PROVEN_CORO_H

/**
 * @file coro.h
 * @brief Zero-overhead Stackless Coroutine Engine
 * 
 * pure C11/C23 macro tricks based on Duff's Device allowing asynchronous 
 * state-machine execution without OS Thread context switching overhead.
 */

/**
 * @brief Coroutine state tracker. Only 4 bytes (int) per Coroutine.
 */
typedef struct {
    int state;
} proven_coro_t;

/**
 * @brief Initializes a coroutine structure. Must be unconditionally called once.
 */
#define PROVEN_CORO_INIT(c) do { (c)->state = 0; } while(0)

/**
 * @brief Start the coroutine evaluation block.
 */
#define PROVEN_CORO_BEGIN(c) switch((c)->state) { case 0:

/**
 * @brief Voluntarily pause execution. 
 * Resolving back to caller, resuming exactly here upon next call.
 */
#define PROVEN_CORO_YIELD(c) \
    do { \
        (c)->state = __LINE__; \
        return 0; \
        case __LINE__:; \
    } while (0)

/**
 * @brief Continuously pause execution until condition is truthy.
 */
#define PROVEN_CORO_AWAIT(c, cond) \
    do { \
        (c)->state = __LINE__; \
        case __LINE__: \
        if (!(cond)) return 0; \
    } while (0)

/**
 * @brief Terminates the execution block and marks the coroutine context as permanently completed.
 */
#define PROVEN_CORO_END(c) \
    } \
    (c)->state = -1; \
    return 1

/**
 * @brief Checks if a coroutine has finished executing its entire block.
 */
#define PROVEN_CORO_IS_DONE(c) ((c)->state == -1)

#endif /* PROVEN_CORO_H */

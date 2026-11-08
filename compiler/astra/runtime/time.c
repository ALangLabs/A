#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* time module                                                         */
/* ------------------------------------------------------------------ */

aval bi_time_now(uint64_t n, aval *argv) {
    (void)argv;
    want_args("now", n, 0, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return mkint_full((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

aval bi_time_sleep(uint64_t n, aval *argv) {
    want_args("sleep", n, 1, 1);
    int64_t ms = expect_int64(argv[0], "sleep() milliseconds");
    if (ms < 0) ms = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    return A_NONE;
}


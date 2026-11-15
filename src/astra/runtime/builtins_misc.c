#include "a_runtime_internal.h"

aval bi_flush(uint64_t n, aval *argv) { (void)argv; want_args("flush", n, 0, 0); fflush(stdout); return A_NONE; }
aval bi_env_get(uint64_t n, aval *argv) {
    want_args("env_get", n, 1, 1);
    const char *name = path_arg(argv[0], "name");
    const char *v = getenv(name);
    if (!v) v = "";
    return a_str_new(v, strlen(v));
}

/* move/borrow/borrow_mut are compile-time ownership markers: at runtime
   they evaluate to the value itself. copy() performs a deep copy. */
aval bi_move(uint64_t n, aval *argv) { want_args("move", n, 1, 1); return argv[0]; }
aval bi_borrow(uint64_t n, aval *argv) { want_args("borrow", n, 1, 1); return argv[0]; }
aval bi_borrow_mut(uint64_t n, aval *argv) { want_args("borrow_mut", n, 1, 1); return argv[0]; }
aval bi_copy(uint64_t n, aval *argv) { want_args("copy", n, 1, 1); return a_deepcopy(argv[0]); }


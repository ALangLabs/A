#include "a_runtime_internal.h"

aval bi_flush(uint64_t n, aval *argv) { (void)argv; want_args("flush", n, 0, 0); fflush(stdout); return A_NONE; }
aval bi_env_get(uint64_t n, aval *argv) {
    want_args("env_get", n, 1, 1);
    const char *name = path_arg(argv[0], "name");
    const char *v = getenv(name);
    if (!v) v = "";
    return a_str_new(v, strlen(v));
}


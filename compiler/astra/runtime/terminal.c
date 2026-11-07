#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* terminal module (POSIX TTY primitives used by the self-hosted REPL) */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)
static struct termios g_repl_termios;
static int g_repl_termios_saved = 0;

aval bi_terminal_isatty(uint64_t n, aval *argv) {
    (void)argv;
    want_args("isatty", n, 0, 0);
    return isatty(STDIN_FILENO) ? A_TRUE : A_FALSE;
}

aval bi_terminal_raw_mode(uint64_t n, aval *argv) {
    want_args("raw_mode", n, 1, 1);
    int on = (argv[0] == A_TRUE);
    if (on) {
        if (tcgetattr(STDIN_FILENO, &g_repl_termios) != 0) return A_FALSE;
        g_repl_termios_saved = 1;
        struct termios raw = g_repl_termios;
        cfmakeraw(&raw);
        /* Keep output post-processing (NL -> CR-NL) enabled: the REPL and any
         * child processes it spawns (repl.eval) share this tty, and without
         * OPOST every '\n' they print staircases instead of returning to
         * column 0. Input stays raw. */
        raw.c_oflag |= OPOST;
        return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0 ? A_TRUE : A_FALSE;
    }
    if (!g_repl_termios_saved) return A_TRUE;
    int ok = tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_repl_termios) == 0;
    g_repl_termios_saved = 0;
    return ok ? A_TRUE : A_FALSE;
}

aval bi_terminal_read_char(uint64_t n, aval *argv) {
    (void)argv;
    want_args("read_char", n, 0, 0);
    char c;
    ssize_t r;
    do { r = read(STDIN_FILENO, &c, 1); } while (r < 0 && errno == EINTR);
    if (r <= 0) return A_NONE;
    return a_str_new(&c, 1);
}

aval bi_terminal_read_char_ms(uint64_t n, aval *argv) {
    want_args("read_char_ms", n, 1, 1);
    int64_t ms = int_val_full(argv[0]);
    if (ms < 0) ms = 0;
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int pr;
    do { pr = poll(&pfd, 1, (int)ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) return A_NONE;
    char c;
    ssize_t r;
    do { r = read(STDIN_FILENO, &c, 1); } while (r < 0 && errno == EINTR);
    if (r <= 0) return A_NONE;
    return a_str_new(&c, 1);
}

aval bi_terminal_write(uint64_t n, aval *argv) {
    want_args("write", n, 1, 1);
    uint64_t len;
    const char *p = str_data(argv[0], &len);
    if (len && fwrite(p, 1, (size_t)len, stdout) != len)
        panicf("terminal.write: write failed");
    return A_NONE;
}

aval bi_terminal_flush(uint64_t n, aval *argv) {
    (void)argv;
    want_args("flush", n, 0, 0);
    fflush(stdout);
    return A_NONE;
}

aval bi_terminal_size(uint64_t n, aval *argv) {
    (void)argv;
    want_args("size", n, 0, 0);
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    aval arr[2];
    arr[0] = a_mkint((int64_t)ws.ws_col);
    arr[1] = a_mkint((int64_t)ws.ws_row);
    return list_from_vec(arr, 2, 0);
}

aval bi_terminal_home(uint64_t n, aval *argv) {
    (void)argv;
    want_args("home", n, 0, 0);
    const char *h = getenv("HOME");
    if (!h || !*h) h = ".";
    return a_str_new(h, strlen(h));
}
#else
aval bi_terminal_unsupported(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("terminal module is not supported on this platform");
    return A_NONE;
}
#define bi_terminal_isatty bi_terminal_unsupported
#define bi_terminal_raw_mode bi_terminal_unsupported
#define bi_terminal_read_char bi_terminal_unsupported
#define bi_terminal_read_char_ms bi_terminal_unsupported
#define bi_terminal_write bi_terminal_unsupported
#define bi_terminal_flush bi_terminal_unsupported
#define bi_terminal_size bi_terminal_unsupported
#define bi_terminal_home bi_terminal_unsupported
#endif

/* ------------------------------------------------------------------ */
/* repl.eval: run A source through the `a` toolchain as a child process. */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)
aval bi_repl_eval(uint64_t n, aval *argv) {
    want_args("repl_eval", n, 1, 1);
    if (kind_of(argv[0]) != K_STR) panicf("repl.eval() source must be str");
    /* Nested evaluation is rejected: the child `a run` inherits
       A_REPL_DEPTH, so a repl.eval inside evaluated code returns 3. */
    const char *depth = getenv("A_REPL_DEPTH");
    if (depth && *depth) return a_mkint(3);
    const char *aBin = getenv("A_A_BIN");
    if (!aBin || !*aBin) aBin = "a";
    uint64_t len;
    const char *src = str_data(argv[0], &len);
    char tmpl[] = "/tmp/a_repl_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) panicf("repl.eval: cannot create temp file");
    ssize_t wr = write(fd, src, (size_t)len);
    close(fd);
    if (wr != (ssize_t)len) {
        unlink(tmpl);
        panicf("repl.eval: temp file write failed");
    }
    setenv("A_REPL_DEPTH", "1", 1);
    pid_t pid = fork();
    if (pid == 0) {
        execl(aBin, "a", "run", tmpl, (char *)NULL);
        execlp("a", "a", "run", tmpl, (char *)NULL);
        _exit(127);
    }
    unsetenv("A_REPL_DEPTH");
    if (pid < 0) {
        unlink(tmpl);
        panicf("repl.eval: fork failed");
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    unlink(tmpl);
    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc == 0) return a_mkint(0);
        if (rc == 1) return a_mkint(1);   /* compile error (kCErr) */
        return a_mkint(2);                /* runtime error / signal death */
    }
    return a_mkint(2);
}
#else
aval bi_repl_eval(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("repl.eval is not supported on this platform");
    return A_NONE;
}
#endif

/* repl.quiet(on): while set, print/eprint discard their output. The A REPL
 * wraps replayed session source in quiet(true)/quiet(false) so re-executed
 * history does not repeat its side-effect output. */
static int g_repl_quiet = 0;

aval bi_repl_quiet(uint64_t n, aval *argv) {
    want_args("repl_quiet", n, 1, 1);
    g_repl_quiet = (argv[0] == A_TRUE) ? 1 : 0;
    return A_NONE;
}


int a_rt_repl_quiet(void) { return g_repl_quiet; }
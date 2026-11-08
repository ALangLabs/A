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
#endif /* !_WIN32: POSIX console primitives */

aval bi_terminal_home(uint64_t n, aval *argv) {
    (void)argv;
    want_args("home", n, 0, 0);
#if !defined(_WIN32)
    const char *h = getenv("HOME");
    if (!h || !*h) h = ".";
    return a_str_new(h, strlen(h));
#else
    /* Windows has no HOME by convention; USERPROFILE is the account
       directory, with HOME honoured for portability and "." as fallback. */
    char buf[1024];
    unsigned long r = GetEnvironmentVariableA("USERPROFILE", buf, sizeof buf);
    if (r == 0 || r >= sizeof buf) {
        r = GetEnvironmentVariableA("HOME", buf, sizeof buf);
        if (r == 0 || r >= sizeof buf) buf[0] = '\0';
    }
    if (!buf[0]) { buf[0] = '.'; buf[1] = '\0'; }
    return a_str_new(buf, strlen(buf));
#endif
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

#if defined(_WIN32)
/* ------------------------------------------------------------------ */
/* Windows console primitives (kernel32 console API + msvcrt _read)   */
/* ------------------------------------------------------------------ */

static unsigned long g_repl_console_mode;
static int g_repl_console_saved = 0;

aval bi_terminal_isatty(uint64_t n, aval *argv) {
    (void)argv;
    want_args("isatty", n, 0, 0);
    unsigned long mode;
    return GetConsoleMode(GetStdHandle(A_WIN_STDIN_HANDLE), &mode)
               ? A_TRUE : A_FALSE;
}

aval bi_terminal_raw_mode(uint64_t n, aval *argv) {
    want_args("raw_mode", n, 1, 1);
    int on = (argv[0] == A_TRUE);
    void *h = GetStdHandle(A_WIN_STDIN_HANDLE);
    if (on) {
        unsigned long mode;
        if (!GetConsoleMode(h, &mode)) return A_FALSE;
        g_repl_console_mode = mode;
        g_repl_console_saved = 1;
        /* Clear line input and echo so read_char sees single
           keystrokes; processed input stays on so the CRT _read()
           still delivers a character stream. */
        return SetConsoleMode(h, mode & ~A_WIN_CONSOLE_RAW_MASK) ? A_TRUE : A_FALSE;
    }
    if (!g_repl_console_saved) return A_TRUE;
    int ok = SetConsoleMode(h, g_repl_console_mode) != 0;
    g_repl_console_saved = 0;
    return ok ? A_TRUE : A_FALSE;
}

aval bi_terminal_read_char(uint64_t n, aval *argv) {
    (void)argv;
    want_args("read_char", n, 0, 0);
    char c;
    int r;
    do { r = _read(0, &c, 1); } while (r < 0 && errno == EINTR);
    if (r <= 0) return A_NONE;
    return a_str_new(&c, 1);
}

aval bi_terminal_read_char_ms(uint64_t n, aval *argv) {
    want_args("read_char_ms", n, 1, 1);
    int64_t ms = int_val_full(argv[0]);
    if (ms < 0) ms = 0;
    /* The stdin handle (console input buffer or pipe) is waitable:
       WaitForSingleObject returns once input can be read. */
    void *h = GetStdHandle(A_WIN_STDIN_HANDLE);
    unsigned long w = WaitForSingleObject(h, (unsigned long)ms);
    if (w != A_WIN_WAIT_OBJECT_0) return A_NONE;
    char c;
    int r;
    do { r = _read(0, &c, 1); } while (r < 0 && errno == EINTR);
    if (r <= 0) return A_NONE;
    return a_str_new(&c, 1);
}

aval bi_terminal_size(uint64_t n, aval *argv) {
    (void)argv;
    want_args("size", n, 0, 0);
    AWinConsoleInfo ci;
    memset(&ci, 0, sizeof(ci));
    if (!GetConsoleScreenBufferInfo(GetStdHandle(A_WIN_STDOUT_HANDLE), &ci) ||
        ci.sizeX <= 0 || ci.sizeY <= 0) {
        ci.sizeX = 80;
        ci.sizeY = 24;
    }
    aval arr[2];
    arr[0] = a_mkint((int64_t)ci.sizeX);
    arr[1] = a_mkint((int64_t)ci.sizeY);
    return list_from_vec(arr, 2, 0);
}
#endif /* _WIN32 */

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
/* Windows: same contract via GetTempPathA + _spawnvp(_P_WAIT). The child
   exit code comes back directly (no waitpid status decoding); _P_WAIT is
   0 in the CRT headers. */
aval bi_repl_eval(uint64_t n, aval *argv) {
    want_args("repl_eval", n, 1, 1);
    if (kind_of(argv[0]) != K_STR) panicf("repl.eval() source must be str");
    const char *depth = getenv("A_REPL_DEPTH");
    if (depth && *depth) return a_mkint(3);
    const char *aBin = getenv("A_A_BIN");
    if (!aBin || !*aBin) aBin = "a";
    uint64_t len;
    const char *src = str_data(argv[0], &len);
    char dir[1024];
    unsigned long dl = GetTempPathA(sizeof(dir), dir);
    if (dl == 0 || dl >= sizeof(dir)) {
        dir[0] = '.';
        dir[1] = '\\';
        dir[2] = '\0';
    }
    size_t dlen = strlen(dir);
    if (dlen && dir[dlen - 1] != '\\' && dlen + 2 <= sizeof(dir)) {
        dir[dlen] = '\\';
        dir[dlen + 1] = '\0';
    }
    char tmpl[1200];
    snprintf(tmpl, sizeof(tmpl), "%sa_repl_XXXXXX", dir);
    if (!_mktemp(tmpl)) panicf("repl.eval: cannot create temp file name");
    FILE *f = fopen(tmpl, "wb");
    if (!f) panicf("repl.eval: cannot create temp file");
    if (len && fwrite(src, 1, (size_t)len, f) != len) {
        fclose(f);
        DeleteFileA(tmpl);
        panicf("repl.eval: temp file write failed");
    }
    fclose(f);
    /* _putenv keeps the string pointer, so the assignment must outlive
       the call — a static buffer does. Clearing sets an empty value,
       which the A_REPL_DEPTH check above treats as unset. */
    static char depth_env[] = "A_REPL_DEPTH=1";
    static char depth_clear[] = "A_REPL_DEPTH=";
    _putenv(depth_env);
    const char *args[4];
    args[0] = "a";
    args[1] = "run";
    args[2] = tmpl;
    args[3] = NULL;
    intptr_t rc = _spawnvp(0 /*_P_WAIT*/, aBin, args);
    _putenv(depth_clear);
    DeleteFileA(tmpl);
    if (rc < 0) panicf("repl.eval: failed to launch '%s'", aBin);
    if (rc == 0) return a_mkint(0);
    if (rc == 1) return a_mkint(1);   /* compile error (kCErr) */
    return a_mkint(2);                /* runtime error */
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
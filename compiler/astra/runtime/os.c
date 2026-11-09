/* OS primitives: process identity, stat, permissions, links, environment
 * enumeration and a few misc POSIX helpers backing std:os. Split out of
 * fs.c so the filesystem module stays focused on file/dir IO.
 *
 * Naming: every entry point is `os_*` so it does not collide with the
 * existing core builtins (read_file, create_dir, ...). The A package
 * `std:os` wraps these through the native `fs` module.
 *
 * Error policy matches the rest of the runtime: misuse/OS failures panic
 * with a message instead of returning an error value. Predicates
 * (access, stat-on-error) return a sentinel instead. */
#include "a_runtime_internal.h"

#if !defined(_WIN32)
#include <pwd.h>
#include <signal.h>
#include <stdlib.h> /* getloadavg */
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/utsname.h>
#else
#include <io.h>
#include <sys/stat.h>
#endif

/* ---- small value helpers ------------------------------------------------ */

static aval os_str_c(const char *s) {
    return a_str_new(s ? s : "", s ? strlen(s) : 0);
}

static void os_set_str(aval d, const char *k, const char *v) {
    a_dict_set(d, os_str_c(k), os_str_c(v));
}

static void os_set_int(aval d, const char *k, int64_t v) {
    a_dict_set(d, os_str_c(k), mkint_full(v));
}

static void os_set_float(aval d, const char *k, double v) {
    a_dict_set(d, os_str_c(k), a_mkdouble(v));
}

static void os_set_bool(aval d, const char *k, int v) {
    a_dict_set(d, os_str_c(k), v ? A_TRUE : A_FALSE);
}

static aval os_tuple(const aval *items, uint64_t n) {
    return list_from_vec(items, n, 1);
}

/* ---- process / cwd ------------------------------------------------------ */

aval os_chdir(uint64_t n, aval *argv) {
    want_args("chdir", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    if (chdir(path) != 0)
        panicf("os.chdir: cannot change directory to '%s': %s", path,
               strerror(errno));
#else
    if (!SetCurrentDirectoryA(path))
        panicf("os.chdir: cannot change directory to '%s'", path);
#endif
    return A_NONE;
}

aval os_getpid(uint64_t n, aval *argv) {
    want_args("getpid", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    return mkint_full((int64_t)getpid());
#else
    return mkint_full((int64_t)GetCurrentProcessId());
#endif
}

aval os_getppid(uint64_t n, aval *argv) {
    want_args("getppid", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    return mkint_full((int64_t)getppid());
#else
    panicf("os.getppid is not supported on Windows");
    return mkint_full(0);
#endif
}

static aval os_getuid_impl(const char *name) {
#if !defined(_WIN32)
    if (!strcmp(name, "getuid")) return mkint_full((int64_t)getuid());
    if (!strcmp(name, "geteuid")) return mkint_full((int64_t)geteuid());
    if (!strcmp(name, "getgid")) return mkint_full((int64_t)getgid());
    return mkint_full((int64_t)getegid());
#else
    panicf("os.%s is not supported on Windows", name);
    return mkint_full(0);
#endif
}

aval os_getuid(uint64_t n, aval *argv) {
    want_args("getuid", n, 0, 0);
    (void)argv;
    return os_getuid_impl("getuid");
}

aval os_geteuid(uint64_t n, aval *argv) {
    want_args("geteuid", n, 0, 0);
    (void)argv;
    return os_getuid_impl("geteuid");
}

aval os_getgid(uint64_t n, aval *argv) {
    want_args("getgid", n, 0, 0);
    (void)argv;
    return os_getuid_impl("getgid");
}

aval os_getegid(uint64_t n, aval *argv) {
    want_args("getegid", n, 0, 0);
    (void)argv;
    return os_getuid_impl("getegid");
}

aval os_getlogin(uint64_t n, aval *argv) {
    want_args("getlogin", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    const char *u = getlogin();
    if (!u || !*u) u = getenv("USER");
    if (!u || !*u) u = getenv("LOGNAME");
    return os_str_c(u ? u : "");
#else
    const char *u = getenv("USERNAME");
    return os_str_c(u ? u : "");
#endif
}

aval os_cpu_count(uint64_t n, aval *argv) {
    want_args("cpu_count", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return mkint_full(c > 0 ? (int64_t)c : 1);
#else
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return mkint_full(si.dwNumberOfProcessors > 0
                          ? (int64_t)si.dwNumberOfProcessors
                          : 1);
#endif
}

aval os_uname(uint64_t n, aval *argv) {
    want_args("uname", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    struct utsname u;
    if (uname(&u) != 0) panicf("os.uname: %s", strerror(errno));
    aval items[5];
    items[0] = os_str_c(u.sysname);
    items[1] = os_str_c(u.nodename);
    items[2] = os_str_c(u.release);
    items[3] = os_str_c(u.version);
    items[4] = os_str_c(u.machine);
    return os_tuple(items, 5);
#else
    aval items[5];
    items[0] = os_str_c("Windows");
    items[1] = os_str_c("");
    items[2] = os_str_c("");
    items[3] = os_str_c("");
    items[4] = os_str_c(getenv("PROCESSOR_ARCHITECTURE")
                            ? getenv("PROCESSOR_ARCHITECTURE")
                            : "");
    return os_tuple(items, 5);
#endif
}

/* ---- stat --------------------------------------------------------------- */

/* stat()/lstat() return a 13-element tuple so the A layer can build a
 * StatResult record without needing heterogeneous maps:
 *   0 mode, 1 ino, 2 dev, 3 nlink, 4 uid, 5 gid, 6 size,
 *   7 atime, 8 mtime, 9 ctime, 10 is_file, 11 is_dir, 12 is_symlink
 * Missing/inaccessible paths return None. */
#if !defined(_WIN32)
static aval os_stat_tuple(const struct stat *st) {
    aval t[13];
    t[0] = mkint_full((int64_t)st->st_mode);
    t[1] = mkint_full((int64_t)st->st_ino);
    t[2] = mkint_full((int64_t)st->st_dev);
    t[3] = mkint_full((int64_t)st->st_nlink);
    t[4] = mkint_full((int64_t)st->st_uid);
    t[5] = mkint_full((int64_t)st->st_gid);
    t[6] = mkint_full((int64_t)st->st_size);
    t[7] = a_mkdouble((double)st->st_atime);
    t[8] = a_mkdouble((double)st->st_mtime);
    t[9] = a_mkdouble((double)st->st_ctime);
    t[10] = S_ISREG(st->st_mode) ? A_TRUE : A_FALSE;
    t[11] = S_ISDIR(st->st_mode) ? A_TRUE : A_FALSE;
    t[12] = S_ISLNK(st->st_mode) ? A_TRUE : A_FALSE;
    return os_tuple(t, 13);
}
#endif

static aval os_stat_common(aval v, const char *what, int follow) {
    const char *path = path_arg(v, "path");
#if !defined(_WIN32)
    struct stat st;
    int rc = follow ? stat(path, &st) : lstat(path, &st);
    if (rc != 0) return os_tuple(NULL, 0);
    return os_stat_tuple(&st);
#else
    (void)follow;
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return os_tuple(NULL, 0);
    aval t[13];
    t[0] = mkint_full((int64_t)st.st_mode);
    t[1] = mkint_full((int64_t)st.st_ino);
    t[2] = mkint_full((int64_t)st.st_dev);
    t[3] = mkint_full((int64_t)st.st_nlink);
    t[4] = mkint_full((int64_t)st.st_uid);
    t[5] = mkint_full((int64_t)st.st_gid);
    t[6] = mkint_full((int64_t)st.st_size);
    t[7] = a_mkdouble((double)st.st_atime);
    t[8] = a_mkdouble((double)st.st_mtime);
    t[9] = a_mkdouble((double)st.st_ctime);
    t[10] = (st.st_mode & _S_IFREG) ? A_TRUE : A_FALSE;
    t[11] = (st.st_mode & _S_IFDIR) ? A_TRUE : A_FALSE;
    t[12] = A_FALSE;
    (void)what;
    return os_tuple(t, 13);
#endif
}

aval os_stat(uint64_t n, aval *argv) {
    want_args("stat", n, 1, 1);
    return os_stat_common(argv[0], "stat", 1);
}

aval os_lstat(uint64_t n, aval *argv) {
    want_args("lstat", n, 1, 1);
    return os_stat_common(argv[0], "lstat", 0);
}

/* ---- access / permissions ----------------------------------------------- */

aval os_access(uint64_t n, aval *argv) {
    want_args("access", n, 1, 2);
    const char *path = path_arg(argv[0], "path");
    int mode = 0;
    if (n >= 2) mode = (int)expect_int64(argv[1], "access mode");
#if !defined(_WIN32)
    return access(path, mode) == 0 ? A_TRUE : A_FALSE;
#else
    /* Windows has no access(2); existence + the read-only attribute is the
     * closest safe approximation for F_OK/R_OK/W_OK. X_OK is treated like
     * F_OK on Windows (no execute bit). */
    unsigned long attr = GetFileAttributesA(path);
    if (attr == A_WIN_INVALID_FILE_ATTRIBUTES) return A_FALSE;
    if ((mode & 0x2) && (attr & A_WIN_FILE_ATTRIBUTE_READONLY)) return A_FALSE;
    return A_TRUE;
#endif
}

aval os_chmod(uint64_t n, aval *argv) {
    want_args("chmod", n, 2, 2);
    const char *path = path_arg(argv[0], "path");
    int64_t mode = expect_int64(argv[1], "chmod mode");
#if !defined(_WIN32)
    if (chmod(path, (mode_t)mode) != 0)
        panicf("os.chmod: cannot chmod '%s': %s", path, strerror(errno));
#else
    unsigned long attr = GetFileAttributesA(path);
    if (attr == A_WIN_INVALID_FILE_ATTRIBUTES)
        panicf("os.chmod: cannot chmod '%s'", path);
    if ((mode & 0200) == 0)
        attr |= A_WIN_FILE_ATTRIBUTE_READONLY;
    else
        attr &= ~A_WIN_FILE_ATTRIBUTE_READONLY;
    if (!SetFileAttributesA(path, attr))
        panicf("os.chmod: cannot chmod '%s'", path);
#endif
    return A_NONE;
}

/* ---- links -------------------------------------------------------------- */

aval os_symlink(uint64_t n, aval *argv) {
    want_args("symlink", n, 2, 2);
#if !defined(_WIN32)
    const char *target = path_arg(argv[0], "target");
    const char *linkpath = path_arg(argv[1], "link");
    if (symlink(target, linkpath) != 0)
        panicf("os.symlink: cannot create link '%s': %s", linkpath,
               strerror(errno));
    return A_NONE;
#else
    (void)argv;
    panicf("os.symlink is not supported on Windows");
    return A_NONE;
#endif
}

aval os_link(uint64_t n, aval *argv) {
    want_args("link", n, 2, 2);
#if !defined(_WIN32)
    const char *src = path_arg(argv[0], "src");
    const char *dst = path_arg(argv[1], "dst");
    if (link(src, dst) != 0)
        panicf("os.link: cannot link '%s' to '%s': %s", src, dst,
               strerror(errno));
    return A_NONE;
#else
    (void)argv;
    panicf("os.link is not supported on Windows");
    return A_NONE;
#endif
}

aval os_readlink(uint64_t n, aval *argv) {
    want_args("readlink", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)xmalloc(cap);
        ssize_t r = readlink(path, buf, cap);
        if (r < 0) {
            free(buf);
            panicf("os.readlink: cannot read link '%s': %s", path,
                   strerror(errno));
        }
        if ((size_t)r < cap) {
            aval out = a_str_new(buf, (uint64_t)r);
            free(buf);
            return out;
        }
        free(buf);
        cap *= 2;
    }
#else
    panicf("os.readlink is not supported on Windows");
    return os_str_c("");
#endif
}

/* ---- timestamps --------------------------------------------------------- */

aval os_utime(uint64_t n, aval *argv) {
    want_args("utime", n, 1, 3);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    /* No times given: touch to the current time (Python semantics). */
    double now = (double)time(NULL);
    double at = (n >= 2) ? num_to_double(argv[1]) : now;
    double mt = (n >= 3) ? num_to_double(argv[2]) : now;
    struct timeval tv[2];
    tv[0].tv_sec = (time_t)at;
    tv[0].tv_usec = (suseconds_t)((at - (double)tv[0].tv_sec) * 1e6);
    tv[1].tv_sec = (time_t)mt;
    tv[1].tv_usec = (suseconds_t)((mt - (double)tv[1].tv_sec) * 1e6);
    if (utimes(path, tv) != 0)
        panicf("os.utime: cannot update '%s': %s", path, strerror(errno));
#else
    (void)argv;
    panicf("os.utime is not supported on Windows");
#endif
    return A_NONE;
}

/* ---- single-level directory ops ---------------------------------------- */

aval os_mkdir(uint64_t n, aval *argv) {
    want_args("mkdir", n, 1, 2);
    const char *path = path_arg(argv[0], "path");
    int mode = 0777;
    if (n >= 2) mode = (int)expect_int64(argv[1], "mkdir mode");
#if !defined(_WIN32)
    if (mkdir(path, (mode_t)mode) != 0)
        panicf("os.mkdir: cannot create '%s': %s", path, strerror(errno));
#else
    (void)mode;
    if (!CreateDirectoryA(path, NULL))
        panicf("os.mkdir: cannot create '%s'", path);
#endif
    return A_NONE;
}

aval os_rmdir(uint64_t n, aval *argv) {
    want_args("rmdir", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    if (rmdir(path) != 0)
        panicf("os.rmdir: cannot remove '%s': %s", path, strerror(errno));
#else
    if (!RemoveDirectoryA(path))
        panicf("os.rmdir: cannot remove '%s'", path);
#endif
    return A_NONE;
}

aval os_truncate(uint64_t n, aval *argv) {
    want_args("truncate", n, 2, 2);
    const char *path = path_arg(argv[0], "path");
    int64_t len = expect_int64(argv[1], "truncate length");
#if !defined(_WIN32)
    if (truncate(path, (off_t)len) != 0)
        panicf("os.truncate: cannot truncate '%s': %s", path,
               strerror(errno));
#else
    FILE *f = fopen(path, "r+b");
    if (!f) panicf("os.truncate: cannot open '%s'", path);
    if (_chsize_s(_fileno(f), (long long)len) != 0) {
        fclose(f);
        panicf("os.truncate: cannot truncate '%s'", path);
    }
    fclose(f);
#endif
    return A_NONE;
}

/* ---- environment -------------------------------------------------------- */

#if !defined(_WIN32)
extern char **environ;
#endif

aval os_getenv(uint64_t n, aval *argv) {
    want_args("getenv", n, 1, 1);
    const char *key = str_of(argv[0], "key")->data;
    const char *v = getenv(key);
    if (!v) return A_NONE;
    return os_str_c(v);
}

aval os_environ_all(uint64_t n, aval *argv) {
    want_args("environ", n, 0, 0);
    (void)argv;
    aval d = a_dict_new();
#if !defined(_WIN32)
    for (char **e = environ; e && *e; ++e) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        a_dict_set(d, a_str_new(*e, (uint64_t)(eq - *e)),
                   os_str_c(eq + 1));
    }
#else
    char *block = GetEnvironmentStringsA();
    if (block) {
        for (char *p = block; *p;) {
            size_t len = strlen(p);
            const char *eq = strchr(p, '=');
            /* Skip the hidden "=C:=..." pseudo-variables. */
            if (eq && eq != p)
                a_dict_set(d, a_str_new(p, (uint64_t)(eq - p)),
                           os_str_c(eq + 1));
            p += len + 1;
        }
        FreeEnvironmentStringsA(block);
    }
#endif
    return d;
}

aval os_setenv(uint64_t n, aval *argv) {
    want_args("setenv", n, 2, 3);
    const char *key = str_of(argv[0], "key")->data;
    const char *val = str_of(argv[1], "value")->data;
    int overwrite = 1;
    if (n >= 3) overwrite = a_truthy_raw_slow(argv[2]) ? 1 : 0;
#if !defined(_WIN32)
    if (setenv(key, val, overwrite) != 0)
        panicf("os.setenv: cannot set '%s': %s", key, strerror(errno));
#else
    if (!overwrite && getenv(key)) return A_NONE;
    if (_putenv_s(key, val) != 0)
        panicf("os.setenv: cannot set '%s'", key);
#endif
    return A_NONE;
}

aval os_unsetenv(uint64_t n, aval *argv) {
    want_args("unsetenv", n, 1, 1);
    const char *key = str_of(argv[0], "key")->data;
#if !defined(_WIN32)
    if (unsetenv(key) != 0)
        panicf("os.unsetenv: cannot unset '%s': %s", key, strerror(errno));
#else
    if (_putenv_s(key, "") != 0)
        panicf("os.unsetenv: cannot unset '%s'", key);
#endif
    return A_NONE;
}

/* ---- misc --------------------------------------------------------------- */

aval os_system(uint64_t n, aval *argv) {
    want_args("system", n, 1, 1);
    const char *cmd = str_of(argv[0], "command")->data;
    int rc = system(cmd);
    /* Return the process exit code, not the raw wait(2) status word
     * (so "exit 3" yields 3, matching Python's os.system contract on
     * POSIX via WEXITSTATUS). */
    if (rc == -1) return mkint_full(-1);
#if defined(_WIN32)
    return mkint_full((int64_t)rc);
#else
    if (WIFEXITED(rc)) return mkint_full((int64_t)WEXITSTATUS(rc));
    if (WIFSIGNALED(rc)) return mkint_full((int64_t)(-WTERMSIG(rc)));
    return mkint_full((int64_t)rc);
#endif
}

/* Cryptographically secure random bytes from the OS. Returns a list of
 * ints 0..255 (the same shape as random.randbytes). */
aval os_urandom(uint64_t n, aval *argv) {
    want_args("urandom", n, 1, 1);
    int64_t count = expect_int64(argv[0], "urandom size");
    if (count < 0) panicf("os.urandom: negative size %lld", (long long)count);
    unsigned char *buf = (unsigned char *)xmalloc((size_t)(count ? count : 1));
#if !defined(_WIN32)
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t got = fread(buf, 1, (size_t)count, f);
            fclose(f);
            if (got != (size_t)count) {
                free(buf);
                panicf("os.urandom: short read from /dev/urandom");
            }
        } else {
            free(buf);
            panicf("os.urandom: cannot open /dev/urandom");
        }
    }
#else
    (void)count;
    free(buf);
    panicf("os.urandom is not supported on Windows");
    return A_NONE;
#endif
    AVec out = { NULL, 0, 0 };
    for (int64_t i = 0; i < count; ++i)
        avec_push(&out, mkint_full((int64_t)buf[i]));
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    free(buf);
    return r;
}

/* ---- system-level extensions (C parity) ---------------------------------- */

aval os_getloadavg(uint64_t n, aval *argv) {
    want_args("getloadavg", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    double load[3] = { 0.0, 0.0, 0.0 };
    if (getloadavg(load, 3) != 3) {
        /* Fallback (some minimal/sandboxed environments deny the libc
         * call): parse /proc/loadavg directly. */
        FILE *f = fopen("/proc/loadavg", "r");
        if (!f || fscanf(f, "%lf %lf %lf", &load[0], &load[1], &load[2]) != 3)
            panicf("os.getloadavg: cannot read load average");
        if (f) fclose(f);
    }
    aval items[3];
    items[0] = a_mkdouble(load[0]);
    items[1] = a_mkdouble(load[1]);
    items[2] = a_mkdouble(load[2]);
    return os_tuple(items, 3);
#else
    panicf("os.getloadavg is not supported on Windows");
    return os_tuple(NULL, 0);
#endif
}

aval os_gethostname(uint64_t n, aval *argv) {
    want_args("gethostname", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    char host[256];
    if (gethostname(host, sizeof(host)) != 0)
        panicf("os.gethostname: %s", strerror(errno));
    return os_str_c(host);
#else
    char host[256];
    DWORD len = (DWORD)sizeof(host);
    if (!GetComputerNameA(host, &len))
        panicf("os.gethostname: cannot get computer name");
    return os_str_c(host);
#endif
}

aval os_umask(uint64_t n, aval *argv) {
    want_args("umask", n, 1, 1);
    int64_t mask = expect_int64(argv[0], "umask mode");
#if !defined(_WIN32)
    return mkint_full((int64_t)umask((mode_t)mask));
#else
    (void)mask;
    panicf("os.umask is not supported on Windows");
    return mkint_full(0);
#endif
}

aval os_getpagesize(uint64_t n, aval *argv) {
    want_args("getpagesize", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    long sz = sysconf(_SC_PAGESIZE);
    return mkint_full(sz > 0 ? (int64_t)sz : 4096);
#else
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return mkint_full((int64_t)si.dwPageSize);
#endif
}

aval os_chown(uint64_t n, aval *argv) {
    want_args("chown", n, 3, 3);
    const char *path = path_arg(argv[0], "path");
    int64_t uid = expect_int64(argv[1], "chown uid");
    int64_t gid = expect_int64(argv[2], "chown gid");
#if !defined(_WIN32)
    if (chown(path, (uid_t)uid, (gid_t)gid) != 0)
        panicf("os.chown: cannot chown '%s': %s", path, strerror(errno));
    return A_NONE;
#else
    (void)path; (void)uid; (void)gid;
    panicf("os.chown is not supported on Windows");
    return A_NONE;
#endif
}

/* Real mount-point probe: a path is a mount point when its device
 * differs from its parent's (POSIX), with "/" always a mount point. */
aval os_ismount(uint64_t n, aval *argv) {
    want_args("ismount", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    struct stat a, b;
    if (stat(path, &a) != 0) return A_FALSE;
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return A_TRUE; /* root (or no parent) */
    char *parent = xstrdup(path);
    parent[(size_t)(slash - path)] = '\0';
    int rc = stat(parent, &b);
    free(parent);
    if (rc != 0) return A_FALSE;
    return a.st_dev != b.st_dev ? A_TRUE : A_FALSE;
#else
    (void)path;
    return path && !strcmp(path, "\\") ? A_TRUE : A_FALSE;
#endif
}

/* statvfs(2): (total_bytes, used_bytes, free_bytes) for the fs holding path. */
aval os_disk_usage(uint64_t n, aval *argv) {
    want_args("disk_usage", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0)
        panicf("os.disk_usage: cannot stat '%s': %s", path, strerror(errno));
    unsigned long long total = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
    unsigned long long free_ = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
    aval items[3];
    items[0] = mkint_full((int64_t)total);
    items[1] = mkint_full((int64_t)(total - free_));
    items[2] = mkint_full((int64_t)free_);
    return os_tuple(items, 3);
#else
    (void)path;
    panicf("os.disk_usage is not supported on Windows");
    return os_tuple(NULL, 0);
#endif
}

/* popen(3): run a shell command and return its stdout as a string. */
aval os_popen(uint64_t n, aval *argv) {
    want_args("popen", n, 1, 1);
    const char *cmd = str_of(argv[0], "command")->data;
    FILE *p = popen(cmd, "r");
    if (!p) panicf("os.popen: cannot run '%s'", cmd);
    size_t cap = 4096, len = 0;
    char *buf = (char *)xmalloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = (char *)xrealloc(buf, cap); }
        size_t r = fread(buf + len, 1, cap - len, p);
        len += r;
        if (r == 0) break;
    }
    pclose(p);
    aval out = a_str_new(buf, (uint64_t)len);
    free(buf);
    return out;
}

/* Send a signal to a process (POSIX kill(2)). Windows: terminate only. */
aval os_kill(uint64_t n, aval *argv) {
    want_args("kill", n, 2, 2);
    int64_t pid = expect_int64(argv[0], "kill pid");
    int64_t sig = expect_int64(argv[1], "kill signal");
#if !defined(_WIN32)
    if (kill((pid_t)pid, (int)sig) != 0)
        panicf("os.kill: cannot signal %lld: %s", (long long)pid, strerror(errno));
    return A_NONE;
#else
    (void)sig;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) panicf("os.kill: cannot open process %lld", (long long)pid);
    BOOL ok = TerminateProcess(h, 0);
    CloseHandle(h);
    if (!ok) panicf("os.kill: cannot signal %lld", (long long)pid);
    return A_NONE;
#endif
}

/* Terminate the current process immediately with an exit code. */
aval os_exit(uint64_t n, aval *argv) {
    want_args("exit", n, 0, 1);
    int code = 0;
    if (n >= 1) code = (int)expect_int64(argv[0], "exit code");
    fflush(stdout);
    fflush(stderr);
    exit(code);
    return A_NONE;
}

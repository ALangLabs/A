#include "a_runtime_internal.h"

static char *read_all_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) panicf("cannot read file '%s'", path);
    size_t cap = 4096, len = 0;
    char *buf = (char *)xmalloc(cap + 1);
    for (;;) {
        if (len == cap) { cap *= 2; buf = (char *)xrealloc(buf, cap + 1); }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) break;
    }
    if (ferror(f)) { fclose(f); free(buf); panicf("cannot read file '%s'", path); }
    fclose(f);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

const char *path_arg(aval v, const char *what) {
    return str_of(v, what)->data;
}

aval bi_read_file(uint64_t n, aval *argv) {
    want_args("read_file", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
    size_t len;
    char *data = read_all_file(path, &len);
    aval r = a_str_new(data, (uint64_t)len);
    free(data);
    return r;
}

static aval write_file_impl(aval pathv, aval textv, const char *mode, const char *name) {
    const char *path = path_arg(pathv, "path");
    if (kind_of(textv) != K_STR) panicf("%s() text must be str", name);
    uint64_t len;
    const char *p = str_data(textv, &len);
    FILE *f = fopen(path, mode);
    if (!f) panicf("cannot open file '%s'", path);
    if (len && fwrite(p, 1, (size_t)len, f) != len) {
        fclose(f);
        panicf("cannot write file '%s'", path);
    }
    fclose(f);
    return A_NONE;
}

aval bi_write_file(uint64_t n, aval *argv) { want_args("write_file", n, 2, 2); return write_file_impl(argv[0], argv[1], "wb", "write_file"); }
aval bi_append_file(uint64_t n, aval *argv) { want_args("append_file", n, 2, 2); return write_file_impl(argv[0], argv[1], "ab", "append_file"); }

aval bi_file_exists(uint64_t n, aval *argv) {
    want_args("file_exists", n, 1, 1);
    FILE *f = fopen(path_arg(argv[0], "path"), "rb");
    if (!f) return A_FALSE;
    fclose(f);
    return A_TRUE;
}

static int str_ptr_cmp(const void *a, const void *b) {
    aval x = *(const aval *)a, y = *(const aval *)b;
    uint64_t lx, ly;
    const char *px = str_data(x, &lx), *py = str_data(y, &ly);
    size_t m = lx < ly ? (size_t)lx : (size_t)ly;
    int c = m ? memcmp(px, py, m) : 0;
    if (c) return c;
    return lx < ly ? -1 : lx > ly ? 1 : 0;
}

aval bi_list_dir(uint64_t n, aval *argv) {
    want_args("list_dir", n, 1, 1);
#if !defined(_WIN32)
    const char *path = path_arg(argv[0], "path");
    DIR *d = opendir(path);
    if (!d) panicf("cannot open directory '%s'", path);
    AVec out = { NULL, 0, 0 };
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        avec_push(&out, a_str_new(de->d_name, strlen(de->d_name)));
    }
    closedir(d);
    /* insertion sort using the same comparator as qsort would need context; simple shell sort */
    for (size_t gap = out.len / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < out.len; ++i) {
            aval t = out.p[i];
            size_t j = i;
            while (j >= gap && str_ptr_cmp(&out.p[j - gap], &t) > 0) {
                out.p[j] = out.p[j - gap];
                j -= gap;
            }
            out.p[j] = t;
        }
    }
    aval r = list_from_vec(out.p, out.len, 0);
    free(out.p);
    return r;
#else
    (void)argv;
    panicf("list_dir is not supported on this platform");
    return A_NONE;
#endif
}

aval bi_remove_file(uint64_t n, aval *argv) {
    want_args("remove_file", n, 1, 1);
    if (remove(path_arg(argv[0], "path")) != 0) panicf("cannot remove file '%s'", path_arg(argv[0], "path"));
    return A_NONE;
}

aval bi_create_dir(uint64_t n, aval *argv) {
    want_args("create_dir", n, 1, 1);
#if !defined(_WIN32)
    const char *path = path_arg(argv[0], "path");
    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                panicf("cannot create directory '%s'", tmp);
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) { free(tmp); return A_NONE; }
        }
        panicf("cannot create directory '%s'", path);
    }
    free(tmp);
    return A_NONE;
#else
    (void)argv;
    panicf("create_dir is not supported on this platform");
    return A_NONE;
#endif
}

static int remove_tree(const char *path) {
#if !defined(_WIN32)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        int ok = 1;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            size_t need = strlen(path) + strlen(de->d_name) + 2;
            char *child = (char *)xmalloc(need);
            snprintf(child, need, "%s/%s", path, de->d_name);
            if (remove_tree(child) != 0) ok = 0;
            free(child);
            if (!ok) break;
        }
        closedir(d);
        if (!ok) return -1;
        return rmdir(path);
    }
    return remove(path);
#else
    (void)path;
    return -1;
#endif
}

aval bi_remove_dir(uint64_t n, aval *argv) {
    want_args("remove_dir", n, 1, 1);
    if (remove_tree(path_arg(argv[0], "path")) != 0)
        panicf("cannot remove directory '%s'", path_arg(argv[0], "path"));
    return A_NONE;
}

aval bi_rename(uint64_t n, aval *argv) {
    want_args("rename", n, 2, 2);
    if (rename(path_arg(argv[0], "old"), path_arg(argv[1], "new")) != 0)
        panicf("cannot rename '%s' to '%s'", path_arg(argv[0], "old"), path_arg(argv[1], "new"));
    return A_NONE;
}

aval bi_current_dir(uint64_t n, aval *argv) {
    want_args("current_dir", n, 0, 0);
    (void)argv;
#if !defined(_WIN32)
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) panicf("cannot get current directory");
    return a_str_new(buf, strlen(buf));
#else
    panicf("current_dir is not supported on this platform");
    return A_NONE;
#endif
}


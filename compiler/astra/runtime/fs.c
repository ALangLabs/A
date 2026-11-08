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

aval bi_read_bytes(uint64_t n, aval *argv) {
    want_args("read_bytes", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
    size_t len;
    char *data = read_all_file(path, &len);
    AVec out = { NULL, 0, 0 };
    for (size_t i = 0; i < len; ++i)
        avec_push(&out, a_mkint((unsigned char)data[i]));
    aval r = list_from_vec(out.p, (uint64_t)out.len, 0);
    free(out.p);
    free(data);
    return r;
}

aval bi_write_bytes(uint64_t n, aval *argv) {
    want_args("write_bytes", n, 2, 2);
    const char *path = path_arg(argv[0], "path");
    AArr *a = arr_of(argv[1], "write_bytes bytes");
    uint64_t len = a->h.len;
    char *buf = (char *)xmalloc(len ? len : 1);
    for (uint64_t i = 0; i < len; ++i) {
        int64_t v = expect_int64(arr_elem(a, i), "write_bytes byte");
        if (v < 0 || v > 255) {
            free(buf);
            panicf("write_bytes: byte value %lld out of range 0-255",
                   (long long)v);
        }
        buf[i] = (char)v;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); panicf("cannot open file '%s'", path); }
    if (len && fwrite(buf, 1, (size_t)len, f) != len) {
        fclose(f);
        free(buf);
        panicf("cannot write file '%s'", path);
    }
    fclose(f);
    free(buf);
    return A_NONE;
}

aval bi_file_size(uint64_t n, aval *argv) {
    want_args("file_size", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
    FILE *f = fopen(path, "rb");
    if (!f) return mkint_full(-1);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return mkint_full(-1); }
    long sz = ftell(f);
    fclose(f);
    return mkint_full(sz < 0 ? -1 : (int64_t)sz);
}

aval bi_file_exists(uint64_t n, aval *argv) {
    want_args("file_exists", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    FILE *f = fopen(path, "rb");
    if (!f) return A_FALSE;
    fclose(f);
    return A_TRUE;
#else
    /* fopen() fails on directories under the Windows CRT, so probe with
       GetFileAttributesA instead — matching POSIX semantics where any
       existing path (file or directory) counts. */
    return GetFileAttributesA(path) != A_WIN_INVALID_FILE_ATTRIBUTES
               ? A_TRUE : A_FALSE;
#endif
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
    const char *path = path_arg(argv[0], "path");
    AVec out = { NULL, 0, 0 };
#if !defined(_WIN32)
    DIR *d = opendir(path);
    if (!d) panicf("cannot open directory '%s'", path);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        avec_push(&out, a_str_new(de->d_name, strlen(de->d_name)));
    }
    closedir(d);
#else
    /* FindFirstFileA("dir\*") enumerates every entry except "." and
       "..", matching the POSIX listing. Both '/' and '\' are accepted
       as separators by the Win32 file APIs. */
    size_t plen = strlen(path);
    if (plen == 0) panicf("cannot open directory '%s'", path);
    int need_sep = plen > 0 && path[plen - 1] != '/' && path[plen - 1] != '\\';
    char *pat = (char *)xmalloc(plen + (size_t)need_sep + 2);
    memcpy(pat, path, plen);
    size_t off = plen;
    if (need_sep) pat[off++] = '\\';
    pat[off] = '*';
    pat[off + 1] = '\0';
    AWinFindData fd;
    void *h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == A_WIN_INVALID_HANDLE) panicf("cannot open directory '%s'", path);
    for (;;) {
        avec_push(&out, a_str_new(fd.cFileName, strlen(fd.cFileName)));
        if (!FindNextFileA(h, &fd)) break;
    }
    FindClose(h);
#endif
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
}

aval bi_remove_file(uint64_t n, aval *argv) {
    want_args("remove_file", n, 1, 1);
    const char *path = path_arg(argv[0], "path");
#if !defined(_WIN32)
    if (remove(path) != 0) panicf("cannot remove file '%s'", path);
#else
    /* POSIX remove() unlinks files and removes empty directories; the
       CRT remove() on Windows only handles files. */
    if (DeleteFileA(path) == 0 && RemoveDirectoryA(path) == 0)
        panicf("cannot remove file '%s'", path);
#endif
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
    /* Win32 equivalent of the POSIX parents-first mkdir above: normalize
       '/' to '\', pre-create each ancestor (failures are ignored — the
       final CreateDirectoryA reports the real error), then create the
       leaf. A bare drive prefix like "C:" is skipped: "C:\" already
       exists and CreateDirectoryA on it is meaningless. */
    const char *path = path_arg(argv[0], "path");
    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    for (size_t i = 0; i < len; ++i)
        if (tmp[i] == '/') tmp[i] = '\\';
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '\\' && !(i == 2 && tmp[1] == ':')) {
            tmp[i] = '\0';
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        unsigned long e = GetLastError();
        if (e == A_WIN_ERROR_ALREADY_EXISTS || e == A_WIN_ERROR_FILE_EXISTS) {
            unsigned long attr = GetFileAttributesA(tmp);
            if (attr != A_WIN_INVALID_FILE_ATTRIBUTES &&
                (attr & A_WIN_FILE_ATTRIBUTE_DIRECTORY)) {
                free(tmp);
                return A_NONE;
            }
        }
        free(tmp);
        panicf("cannot create directory '%s'", path);
    }
    free(tmp);
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
    /* Win32 recursive delete. Reparse points (symlinks, junctions) are
       removed as links, never followed — the same policy the POSIX path
       gets from lstat()+!S_ISLNK. */
    unsigned long attr = GetFileAttributesA(path);
    if (attr == A_WIN_INVALID_FILE_ATTRIBUTES) return -1;
    if (!(attr & A_WIN_FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & A_WIN_FILE_ATTRIBUTE_REPARSE_POINT)) {
        /* File (or link): DeleteFileA covers files and file symlinks;
           RemoveDirectoryA covers directory symlinks and junctions. */
        if (DeleteFileA(path)) return 0;
        return RemoveDirectoryA(path) ? 0 : -1;
    }
    /* Real directory: enumerate children with FindFirstFileA and recurse.
       Trailing separators are tolerated when joining child paths. */
    size_t plen = strlen(path);
    int need_sep = plen > 0 && path[plen - 1] != '/' && path[plen - 1] != '\\';
    char *pat = (char *)xmalloc(plen + (size_t)need_sep + 2);
    memcpy(pat, path, plen);
    size_t off = plen;
    if (need_sep) pat[off++] = '\\';
    pat[off] = '*';
    pat[off + 1] = '\0';
    AWinFindData fd;
    void *h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == A_WIN_INVALID_HANDLE) return -1;
    int ok = 1;
    for (;;) {
        size_t nlen = strlen(fd.cFileName);
        size_t need = plen + (size_t)need_sep + nlen + 2;
        char *child = (char *)xmalloc(need);
        memcpy(child, path, plen);
        size_t coff = plen;
        if (need_sep) child[coff++] = '\\';
        memcpy(child + coff, fd.cFileName, nlen);
        child[coff + nlen] = '\0';
        if (remove_tree(child) != 0) { ok = 0; free(child); break; }
        free(child);
        if (!FindNextFileA(h, &fd)) break;
    }
    FindClose(h);
    if (!ok) return -1;
    return RemoveDirectoryA(path) ? 0 : -1;
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
    char buf[4096];
    if (!GetCurrentDirectoryA(sizeof(buf), buf))
        panicf("cannot get current directory");
    return a_str_new(buf, strlen(buf));
#endif
}


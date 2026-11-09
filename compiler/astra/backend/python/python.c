#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* python interop: libpython is dlopen'd at run time (no link-time     */
/* dependency). Mirrors compiler/astra/python/python_loader.cpp:       */
/* A_PYTHON_LIB / A_PYTHON overrides, project + env venvs, PATH         */
/* interpreters, uv installs, then system libpython*.so* as fallback.  */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)

/* Iterables reporting a length above this stay opaque PyObjects instead of
   being drained into A lists (see py_to_aval). */
#define PY_OPAQUE_LEN_HINT 65536

typedef void *PyObj;
typedef ptrdiff_t PySsize;

/* Py_buffer layout is a stable CPython ABI (Include/object.h); we cannot
   include Python.h, so mirror it. PySsize_t == ptrdiff_t on every supported
   platform. */
typedef struct {
    void *buf;
    PyObj obj;
    PySsize len;
    PySsize itemsize;
    int readonly;
    int ndim;
    char *format;
    PySsize *shape;
    PySsize *strides;
    PySsize *suboffsets;
    void *internal;
} PyBufferView;

/* PyBUF_* flag values (Include/object.h). */
#define PYBUF_SIMPLE 0
#define PYBUF_WRITABLE 0x0001
#define PYBUF_FORMAT 0x0004
#define PYBUF_ND 0x0008
#define PYBUF_STRIDES (0x0010 | PYBUF_ND)
#define PYBUF_C_CONTIGUOUS (0x0020 | PYBUF_STRIDES)

struct PyRt {
    void *handle;
    int state;          /* 0 = untried, 1 = ready, -1 = failed */
    int initialized;    /* Py_Initialize + sys.path setup done */
    char error[512];

    /* function pointers (all resolved with dlsym) */
    void (*Py_Initialize)(void);
    void (*Py_Finalize)(void);
    int (*PyGILState_Ensure)(void);
    void (*PyGILState_Release)(int);
    PyObj (*PyImport_ImportModule)(const char *);
    PyObj (*PyObject_GetAttrString)(PyObj, const char *);
    PyObj (*PyObject_Call)(PyObj, PyObj, PyObj);
    PyObj (*PyObject_Str)(PyObj);
    PyObj (*PyErr_Occurred)(void);
    void (*PyErr_Clear)(void);
    void (*PyErr_Fetch)(PyObj *, PyObj *, PyObj *);
    void (*PyErr_NormalizeException)(PyObj *, PyObj *, PyObj *);
    PyObj (*PySys_GetObject)(const char *);
    const char *(*PyUnicode_AsUTF8AndSize)(PyObj, PySsize *);
    PyObj (*PyUnicode_DecodeUTF8)(const char *, PySsize, const char *);
    long long (*PyLong_AsLongLongAndOverflow)(PyObj, int *);
    PyObj (*PyLong_FromLongLong)(long long);
    double (*PyFloat_AsDouble)(PyObj);
    PyObj (*PyFloat_FromDouble)(double);
    PyObj (*PyList_New)(PySsize);
    PySsize (*PyList_Size)(PyObj);
    PyObj (*PyList_GetItem)(PyObj, PySsize);
    int (*PyList_SetItem)(PyObj, PySsize, PyObj);
    int (*PyList_Append)(PyObj, PyObj);
    PyObj (*PyTuple_New)(PySsize);
    PySsize (*PyTuple_Size)(PyObj);
    PyObj (*PyTuple_GetItem)(PyObj, PySsize);
    int (*PyTuple_SetItem)(PyObj, PySsize, PyObj);
    PyObj (*PyDict_New)(void);
    int (*PyDict_SetItem)(PyObj, PyObj, PyObj);
    int (*PyDict_Next)(PyObj, PySsize *, PyObj *, PyObj *);
    PyObj (*PyObject_GetIter)(PyObj);
    PyObj (*PyIter_Next)(PyObj);
    PySsize (*PyObject_Length)(PyObj);
    PyObj (*PyObject_GetItem)(PyObj, PyObj);
    int (*PyObject_SetItem)(PyObj, PyObj, PyObj);
    int (*PyObject_DelItem)(PyObj, PyObj);
    int (*PyObject_SetAttrString)(PyObj, const char *, PyObj);
    int (*PyObject_HasAttrString)(PyObj, const char *);
    int (*PyObject_DelAttrString)(PyObj, const char *);
    int (*PySequence_Contains)(PyObj, PyObj);
    int (*PyObject_IsTrue)(PyObj);
    PyObj (*PyObject_Repr)(PyObj);
    PyObj (*PyObject_Type)(PyObj);
    int (*PyObject_RichCompareBool)(PyObj, PyObj, int);
    int (*PyObject_IsInstance)(PyObj, PyObj);
    int (*PyCallable_Check)(PyObj);
    void (*Py_IncRef)(PyObj);
    void (*Py_DecRef)(PyObj);
    PyObj (*PyNumber_Index)(PyObj);
    PyObj (*PyNumber_Float)(PyObj);
    char *(*PyBytes_AsString)(PyObj);
    PySsize (*PyBytes_Size)(PyObj);
    PyObj (*PySet_New)(PyObj);
    int (*PySet_Add)(PyObj, PyObj);
    int (*PyObject_CheckBuffer)(PyObj);
    int (*PyObject_GetBuffer)(PyObj, PyBufferView *, int);
    void (*PyBuffer_Release)(PyBufferView *);

    /* data symbols: addresses of the real objects inside libpython */
    PyObj none, py_true, py_false;
    PyObj t_bool, t_long, t_float, t_unicode, t_bytes;
    PyObj t_list, t_tuple, t_dict, t_set, t_frozenset;
    PyObj t_module, t_type;

    /* sys.path extras reported by the interpreter probe that won */
    char **site_dirs;
    int n_site_dirs, cap_site_dirs;
    char *py_home;
};

static struct PyRt g_py;

static void py_site_dir_add(const char *dir) {
    if (!dir || !*dir) return;
    if (g_py.n_site_dirs == g_py.cap_site_dirs) {
        g_py.cap_site_dirs = g_py.cap_site_dirs ? g_py.cap_site_dirs * 2 : 8;
        g_py.site_dirs = (char **)xrealloc(g_py.site_dirs,
                                           (size_t)g_py.cap_site_dirs * sizeof(char *));
    }
    g_py.site_dirs[g_py.n_site_dirs++] = xstrdup(dir);
}

/* Resolves every needed symbol; returns 1 on success. */
static int py_fill(void *h) {
    struct PyRt *p = &g_py;
#define A_PY_SYM(field, name)                                              \
    p->field = (__typeof__(p->field))dlsym(h, name);                        \
    if (!p->field) return 0
    A_PY_SYM(Py_Initialize, "Py_Initialize");
    A_PY_SYM(Py_Finalize, "Py_Finalize");
    A_PY_SYM(PyGILState_Ensure, "PyGILState_Ensure");
    A_PY_SYM(PyGILState_Release, "PyGILState_Release");
    A_PY_SYM(PyImport_ImportModule, "PyImport_ImportModule");
    A_PY_SYM(PyObject_GetAttrString, "PyObject_GetAttrString");
    A_PY_SYM(PyObject_Call, "PyObject_Call");
    A_PY_SYM(PyObject_Str, "PyObject_Str");
    A_PY_SYM(PyErr_Occurred, "PyErr_Occurred");
    A_PY_SYM(PyErr_Clear, "PyErr_Clear");
    A_PY_SYM(PyErr_Fetch, "PyErr_Fetch");
    A_PY_SYM(PyErr_NormalizeException, "PyErr_NormalizeException");
    A_PY_SYM(PySys_GetObject, "PySys_GetObject");
    A_PY_SYM(PyUnicode_AsUTF8AndSize, "PyUnicode_AsUTF8AndSize");
    A_PY_SYM(PyUnicode_DecodeUTF8, "PyUnicode_DecodeUTF8");
    A_PY_SYM(PyLong_AsLongLongAndOverflow, "PyLong_AsLongLongAndOverflow");
    A_PY_SYM(PyLong_FromLongLong, "PyLong_FromLongLong");
    A_PY_SYM(PyFloat_AsDouble, "PyFloat_AsDouble");
    A_PY_SYM(PyFloat_FromDouble, "PyFloat_FromDouble");
    A_PY_SYM(PyList_New, "PyList_New");
    A_PY_SYM(PyList_Size, "PyList_Size");
    A_PY_SYM(PyList_GetItem, "PyList_GetItem");
    A_PY_SYM(PyList_SetItem, "PyList_SetItem");
    A_PY_SYM(PyList_Append, "PyList_Append");
    A_PY_SYM(PyTuple_New, "PyTuple_New");
    A_PY_SYM(PyTuple_Size, "PyTuple_Size");
    A_PY_SYM(PyTuple_GetItem, "PyTuple_GetItem");
    A_PY_SYM(PyTuple_SetItem, "PyTuple_SetItem");
    A_PY_SYM(PyDict_New, "PyDict_New");
    A_PY_SYM(PyDict_SetItem, "PyDict_SetItem");
    A_PY_SYM(PyDict_Next, "PyDict_Next");
    A_PY_SYM(PyObject_GetIter, "PyObject_GetIter");
    A_PY_SYM(PyIter_Next, "PyIter_Next");
    A_PY_SYM(PyObject_Length, "PyObject_Length");
    A_PY_SYM(PyObject_GetItem, "PyObject_GetItem");
    A_PY_SYM(PyObject_SetItem, "PyObject_SetItem");
    A_PY_SYM(PyObject_DelItem, "PyObject_DelItem");
    A_PY_SYM(PyObject_SetAttrString, "PyObject_SetAttrString");
    A_PY_SYM(PyObject_HasAttrString, "PyObject_HasAttrString");
    A_PY_SYM(PyObject_DelAttrString, "PyObject_DelAttrString");
    A_PY_SYM(PySequence_Contains, "PySequence_Contains");
    A_PY_SYM(PyObject_IsTrue, "PyObject_IsTrue");
    A_PY_SYM(PyObject_Repr, "PyObject_Repr");
    A_PY_SYM(PyObject_Type, "PyObject_Type");
    A_PY_SYM(PyObject_RichCompareBool, "PyObject_RichCompareBool");
    A_PY_SYM(PyObject_IsInstance, "PyObject_IsInstance");
    A_PY_SYM(PyCallable_Check, "PyCallable_Check");
    A_PY_SYM(Py_IncRef, "Py_IncRef");
    A_PY_SYM(Py_DecRef, "Py_DecRef");
    A_PY_SYM(PyNumber_Index, "PyNumber_Index");
    A_PY_SYM(PyNumber_Float, "PyNumber_Float");
    A_PY_SYM(PyBytes_AsString, "PyBytes_AsString");
    A_PY_SYM(PyBytes_Size, "PyBytes_Size");
    A_PY_SYM(PySet_New, "PySet_New");
    A_PY_SYM(PySet_Add, "PySet_Add");
    A_PY_SYM(PyObject_CheckBuffer, "PyObject_CheckBuffer");
    A_PY_SYM(PyObject_GetBuffer, "PyObject_GetBuffer");
    A_PY_SYM(PyBuffer_Release, "PyBuffer_Release");
#undef A_PY_SYM
#define A_PY_DAT(field, name) p->field = dlsym(h, name)
    A_PY_DAT(none, "_Py_NoneStruct");
    A_PY_DAT(py_true, "_Py_TrueStruct");
    A_PY_DAT(py_false, "_Py_FalseStruct");
    A_PY_DAT(t_bool, "PyBool_Type");
    A_PY_DAT(t_long, "PyLong_Type");
    A_PY_DAT(t_float, "PyFloat_Type");
    A_PY_DAT(t_unicode, "PyUnicode_Type");
    A_PY_DAT(t_bytes, "PyBytes_Type");
    A_PY_DAT(t_list, "PyList_Type");
    A_PY_DAT(t_tuple, "PyTuple_Type");
    A_PY_DAT(t_dict, "PyDict_Type");
    A_PY_DAT(t_set, "PySet_Type");
    A_PY_DAT(t_frozenset, "PyFrozenSet_Type");
    A_PY_DAT(t_module, "PyModule_Type");
    A_PY_DAT(t_type, "PyType_Type");
#undef A_PY_DAT
    return p->none && p->py_true && p->py_false && p->t_bool && p->t_long &&
           p->t_float && p->t_unicode && p->t_bytes && p->t_list && p->t_tuple &&
           p->t_dict && p->t_set && p->t_frozenset;
}

/* Probe output: version, LIBDIR, LDLIBRARY, purelib, platlib, stdlib,
   base_prefix — same layout as the C++ loader's probe. */
typedef struct {
    char libdir[1024];
    char ldlib[1024];
    char site_dirs[3][1024];
    int n_site_dirs;
    char home[1024];
} PyProbe;

static int py_run_probe(const char *exe, PyProbe *out) {
    static const char kCode[] =
        "import sys,sysconfig\n"
        "print(sys.version.split()[0])\n"
        "print(sysconfig.get_config_var(\"LIBDIR\") or \"\")\n"
        "print(sysconfig.get_config_var(\"LDLIBRARY\") or \"\")\n"
        "print(sysconfig.get_paths()[\"purelib\"])\n"
        "print(sysconfig.get_paths()[\"platlib\"])\n"
        "print(sysconfig.get_paths()[\"stdlib\"])\n"
        "print(sys.base_prefix)\n";
    /* refuse shell metacharacters in the exe path */
    if (strpbrk(exe, "\"'$`\\")) return 0;
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"%s\" -c '%s' 2>/dev/null", exe, kCode);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char line[1100];
    int n = 0;
    memset(out, 0, sizeof *out);
    while (n < 8 && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (n == 1) snprintf(out->libdir, sizeof out->libdir, "%s", line);
        else if (n == 2) snprintf(out->ldlib, sizeof out->ldlib, "%s", line);
        else if (n == 3 || n == 4 || n == 5) {
            if (*line && out->n_site_dirs < 3)
                snprintf(out->site_dirs[out->n_site_dirs++], 1024, "%s", line);
        } else if (n == 6) snprintf(out->home, sizeof out->home, "%s", line);
        ++n;
    }
    pclose(f);
    return n >= 7;
}

/* Try one libpython path; on success records probe site info. */
static int py_try_lib(const char *path, const PyProbe *probe) {
    if (!path || !*path) return 0;
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return 0;
    if (!py_fill(h)) { dlclose(h); return 0; }
    g_py.handle = h;
    g_py.state = 1;
    if (probe) {
        for (int i = 0; i < probe->n_site_dirs; ++i)
            py_site_dir_add(probe->site_dirs[i]);
        if (probe->home[0]) g_py.py_home = xstrdup(probe->home);
    }
    return 1;
}

/* Candidate libpython paths from one probed interpreter. */
static void py_try_probe(const char *exe) {
    PyProbe p;
    if (!py_run_probe(exe, &p)) return;
    char cand[1400];
    /* LDLIBRARY first (resolved against LIBDIR when relative; .a -> .so). */
    if (p.ldlib[0]) {
        snprintf(cand, sizeof cand, "%s", p.ldlib);
        if (cand[0] != '/' && p.libdir[0]) {
            char tmp[1400];
            snprintf(tmp, sizeof tmp, "%s/%s", p.libdir, cand);
            snprintf(cand, sizeof cand, "%s", tmp);
        }
        size_t cl = strlen(cand);
        if (cl > 2 && strcmp(cand + cl - 2, ".a") == 0) {
            memcpy(cand + cl - 2, ".so", 4); /* includes NUL */
        }
        if (py_try_lib(cand, &p)) return;
    }
    /* Then every libpython3*.so* in LIBDIR (covers empty/misconfigured
       LDLIBRARY and alternative build layouts). */
    if (p.libdir[0]) {
        DIR *d = opendir(p.libdir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                const char *nm = e->d_name;
                if (strncmp(nm, "libpython3", 10) != 0 || !strstr(nm, ".so"))
                    continue;
                snprintf(cand, sizeof cand, "%s/%s", p.libdir, nm);
                if (py_try_lib(cand, &p)) { closedir(d); return; }
            }
            closedir(d);
        }
    }
}

static void py_add_exe(char *buf, size_t cap, const char *dir, const char *name) {
    if (!dir || !*dir) return;
    snprintf(buf, cap, "%s/%s", dir, name);
    if (access(buf, X_OK) == 0) py_try_probe(buf);
}

static int py_ensure(void) {
    if (g_py.state == 1) return 1;
    if (g_py.state == -1) return 0;
    g_py.state = -1;

    /* A_PYTHON_LIB: explicit shared object. */
    const char *forcedLib = getenv("A_PYTHON_LIB");
    if (forcedLib && *forcedLib && py_try_lib(forcedLib, NULL)) {
        g_py.state = 1;
        return 1;
    }

    /* A_PYTHON may be an interpreter or a libpython*.so path. */
    const char *forced = getenv("A_PYTHON");
    if (forced && *forced) {
        if (strstr(forced, "libpython") && strstr(forced, ".so") &&
            access(forced, R_OK) == 0) {
            if (py_try_lib(forced, NULL)) { g_py.state = 1; return 1; }
        } else {
            py_try_probe(forced);
            if (g_py.state == 1) return 1;
        }
    } else {
        char exe[1200];
        /* project venvs first */
        static const char *const venvs[] = {".venv", "venv"};
        for (int i = 0; i < 2; ++i) {
            py_add_exe(exe, sizeof exe, venvs[i], "bin/python3");
            if (g_py.state == 1) return 1;
            py_add_exe(exe, sizeof exe, venvs[i], "bin/python");
            if (g_py.state == 1) return 1;
        }
        static const char *const envs[] = {"VIRTUAL_ENV", "UV_PROJECT_ENVIRONMENT",
                                           "CONDA_PREFIX"};
        for (int i = 0; i < 3; ++i) {
            const char *d = getenv(envs[i]);
            if (!d || !*d) continue;
            py_add_exe(exe, sizeof exe, d, "bin/python3");
            if (g_py.state == 1) return 1;
            py_add_exe(exe, sizeof exe, d, "bin/python");
            if (g_py.state == 1) return 1;
        }
        py_try_probe("python3");
        if (g_py.state == 1) return 1;
        py_try_probe("python");
        if (g_py.state == 1) return 1;
        /* uv-managed installs */
        const char *home = getenv("HOME");
        if (home && *home) {
            char root[1200];
            snprintf(root, sizeof root, "%s/.local/share/uv/python", home);
            DIR *d = opendir(root);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (e->d_name[0] == '.') continue;
                    snprintf(exe, sizeof exe, "%s/%s/bin", root, e->d_name);
                    DIR *b = opendir(exe);
                    if (!b) continue;
                    struct dirent *f;
                    while ((f = readdir(b))) {
                        if (strncmp(f->d_name, "python3", 7) != 0) continue;
                        if (strstr(f->d_name, "config")) continue;
                        char full[1400];
                        snprintf(full, sizeof full, "%s/%s", exe, f->d_name);
                        if (access(full, X_OK) == 0) py_try_probe(full);
                        if (g_py.state == 1) { closedir(b); closedir(d); return 1; }
                    }
                    closedir(b);
                }
                closedir(d);
            }
        }
        static const char *const sysExe[] = {"/usr/bin/python3", "/usr/local/bin/python3",
                                             "/usr/bin/python"};
        for (int i = 0; i < 3; ++i) {
            py_try_probe(sysExe[i]);
            if (g_py.state == 1) return 1;
        }
    }

    /* Last resort: system libpython shared objects. */
    static const char *const libDirs[] = {"/usr/lib/x86_64-linux-gnu", "/usr/lib",
                                          "/usr/local/lib", "/lib/x86_64-linux-gnu",
                                          "/lib"};
    for (int i = 0; i < 5; ++i) {
        DIR *d = opendir(libDirs[i]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *nm = e->d_name;
            if (strncmp(nm, "libpython3", 10) != 0 || !strstr(nm, ".so")) continue;
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", libDirs[i], nm);
            if (py_try_lib(path, NULL)) { closedir(d); g_py.state = 1; return 1; }
        }
        closedir(d);
    }

    snprintf(g_py.error, sizeof g_py.error,
             "no usable libpython found (set A_PYTHON or A_PYTHON_LIB)");
    return 0;
}

/* Append dir to sys.path (GIL must be held). */
static void py_append_syspath(const char *dir) {
    if (!dir || !*dir) return;
    PyObj path = g_py.PySys_GetObject("path");  /* borrowed */
    if (!path) return;
    PyObj entry = g_py.PyUnicode_DecodeUTF8(dir, (PySsize)strlen(dir), "strict");
    if (entry) {
        g_py.PyList_Append(path, entry);
        g_py.Py_DecRef(entry);
    }
}

/* Project ./.venv / ./venv site-packages + A_PYTHONPATH (GIL held). */
static void py_extra_syspaths(void) {
    static const char *const venvs[] = {".venv", "venv"};
    for (int i = 0; i < 2; ++i) {
        char cfg[1200];
        snprintf(cfg, sizeof cfg, "%s/pyvenv.cfg", venvs[i]);
        if (access(cfg, F_OK) != 0) continue;
        char libdir[1200];
        snprintf(libdir, sizeof libdir, "%s/lib", venvs[i]);
        DIR *d = opendir(libdir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char site[1400];
            snprintf(site, sizeof site, "%s/%s/site-packages", libdir, e->d_name);
            struct stat st;
            if (stat(site, &st) == 0 && S_ISDIR(st.st_mode)) py_append_syspath(site);
        }
        closedir(d);
    }
    const char *pp = getenv("A_PYTHONPATH");
    if (pp && *pp) {
        char *copy = xstrdup(pp);
        for (char *s = copy, *tok; (tok = strtok(s, ":")); s = NULL)
            py_append_syspath(tok);
        free(copy);
    }
}

static void pybuf_release_all(void);
static void py_finalize(void);

/* One-time interpreter init after a successful dlopen. */
static void py_init_once(void) {
    if (g_py.initialized) return;
    const char *oldHome = getenv("PYTHONHOME");
    char savedHome[1024];
    savedHome[0] = 0;
    if (oldHome) snprintf(savedHome, sizeof savedHome, "%s", oldHome);
    if (g_py.py_home) setenv("PYTHONHOME", g_py.py_home, 1);
    g_py.Py_Initialize();
    atexit(py_finalize);
    if (g_py.py_home) {
        if (oldHome) setenv("PYTHONHOME", savedHome, 1);
        else unsetenv("PYTHONHOME");
    }
    int gil = g_py.PyGILState_Ensure();
    for (int i = 0; i < g_py.n_site_dirs; ++i) py_append_syspath(g_py.site_dirs[i]);
    py_extra_syspaths();
    g_py.PyGILState_Release(gil);
    g_py.initialized = 1;
}

static void py_finalize(void) {
    if (!g_py.Py_Finalize) return;
    pybuf_release_all();
    g_py.Py_Finalize();
}

static int py_ensure_ready(void) {
    if (!py_ensure()) return 0;
    py_init_once();
    return 1;
}

static int py_is(PyObj o, PyObj t) {
    int r = g_py.PyObject_IsInstance(o, t);
    if (r < 0) { g_py.PyErr_Clear(); return 0; }
    return r == 1;
}

/* GIL must be held. Takes a borrowed reference; never decrefs. */
static aval py_to_aval(PyObj o);
static void py_fail(const char *what);

/* Wrap an owned PyObject* reference (new ref, ownership transferred) as a
   T_PYOBJ aval. GIL must be held for the IncRef in the borrowed variant. */
static aval py_wrap_owned(PyObj o) {
    APyObj *p = (APyObj *)xmalloc(sizeof(APyObj));
    p->h.type = T_PYOBJ;
    p->h.len = 0;
    p->h.cap = 0;
    p->obj = o;
    return mkobj(&p->h);
}

static aval py_wrap_borrowed(PyObj o) {
    g_py.Py_IncRef(o);
    return py_wrap_owned(o);
}

static aval py_iter_to_list(PyObj it) {
    /* drain an iterator into an A list */
    aval *items = NULL;
    uint64_t n = 0, cap = 0;
    PyObj item;
    while ((item = g_py.PyIter_Next(it)) != NULL) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            items = (aval *)xrealloc(items, cap * sizeof(aval));
        }
        items[n++] = py_to_aval(item);
        g_py.Py_DecRef(item);
    }
    if (g_py.PyErr_Occurred()) py_fail("iteration");
    aval r = a_list_new(n, items);
    free(items);
    return r;
}

static aval py_to_aval(PyObj o) {
    if (!o || o == g_py.none) return A_NONE;
    if (o == g_py.py_true) return A_TRUE;
    if (o == g_py.py_false) return A_FALSE;
    if (py_is(o, g_py.t_bool)) return o == g_py.py_true ? A_TRUE : A_FALSE;
    if (py_is(o, g_py.t_long)) {
        int ovf = 0;
        long long v = g_py.PyLong_AsLongLongAndOverflow(o, &ovf);
        if (!ovf && !g_py.PyErr_Occurred()) return mkint_full(v);
        g_py.PyErr_Clear();
        double d = g_py.PyFloat_AsDouble(o);
        if (!g_py.PyErr_Occurred()) return mkdouble(d);
        g_py.PyErr_Clear();
    } else if (py_is(o, g_py.t_float)) {
        return mkdouble(g_py.PyFloat_AsDouble(o));
    } else if (py_is(o, g_py.t_unicode)) {
        PySsize len = 0;
        const char *s = g_py.PyUnicode_AsUTF8AndSize(o, &len);
        return a_str_new(s ? s : "", len < 0 ? 0 : (uint64_t)len);
    } else if (py_is(o, g_py.t_bytes)) {
        char *s = g_py.PyBytes_AsString(o);
        PySsize len = g_py.PyBytes_Size(o);
        return a_str_new(s ? s : "", len < 0 ? 0 : (uint64_t)len);
    } else if (py_is(o, g_py.t_list) || py_is(o, g_py.t_tuple)) {
        int isTuple = py_is(o, g_py.t_tuple);
        PySsize n = isTuple ? g_py.PyTuple_Size(o) : g_py.PyList_Size(o);
        if (n < 0) { g_py.PyErr_Clear(); n = 0; }
        aval *items = (aval *)xmalloc((size_t)(n ? n : 1) * sizeof(aval));
        for (PySsize i = 0; i < n; ++i) {
            PyObj item = isTuple ? g_py.PyTuple_GetItem(o, i)
                                 : g_py.PyList_GetItem(o, i);
            items[i] = py_to_aval(item);
        }
        aval r = isTuple ? a_tuple_new((uint64_t)n, items)
                         : a_list_new((uint64_t)n, items);
        free(items);
        return r;
    } else if (py_is(o, g_py.t_dict)) {
        aval d = a_dict_new();
        PySsize pos = 0;
        PyObj k = NULL, v = NULL;
        while (g_py.PyDict_Next(o, &pos, &k, &v)) {
            aval ak = py_to_aval(k), av = py_to_aval(v);
            a_dict_set(d, ak, av);
        }
        return d;
    } else if (py_is(o, g_py.t_set) || py_is(o, g_py.t_frozenset)) {
        aval s = a_set_new();
        PyObj it = g_py.PyObject_GetIter(o);
        if (it) {
            PyObj item;
            while ((item = g_py.PyIter_Next(it)) != NULL) {
                a_set_add(s, py_to_aval(item));
                g_py.Py_DecRef(item);
            }
            g_py.Py_DecRef(it);
        } else {
            g_py.PyErr_Clear();
        }
        return s;
    } else {
        /* generic iterable -> A list, but only when small enough to be
           reasonable. Large containers (tensors, datasets, big arrays)
           stay opaque PyObjects — draining them would materialize
           millions of wrappers, and they can still be passed back into
           Python calls. Mirrors Mojo keeping PythonObject opaque unless
           an explicit conversion is requested. */
        PySsize hint = g_py.PyObject_Length(o);
        if (hint > PY_OPAQUE_LEN_HINT) {
            /* fall through to the opaque wrap below */
        } else {
            if (hint < 0) g_py.PyErr_Clear();
            /* Buffer-protocol objects (numpy arrays, array.array,
               memoryview, bytearray, mmap, ...) stay opaque regardless of
               size so py_buffer() can hand out a zero-copy T_PYBUF view;
               draining them would copy every element into A values. */
            if (g_py.PyObject_CheckBuffer(o))
                return py_wrap_borrowed(o);
            PyObj it = g_py.PyObject_GetIter(o);
            if (it) {
                /* An object that is its own iterator (generators,
                   iterators, one-shot streams) stays opaque so `for` can
                   consume it lazily instead of draining it eagerly —
                   draining would also exhaust it. */
                if (it == o) {
                    g_py.Py_DecRef(it);
                    return py_wrap_borrowed(o);
                }
                aval r = py_iter_to_list(it);
                g_py.Py_DecRef(it);
                return r;
            }
            g_py.PyErr_Clear();
        }
        /* int-like via __index__, then float-like via __float__ */
        PyObj idx = g_py.PyNumber_Index(o);
        if (idx) {
            aval r = py_to_aval(idx);
            g_py.Py_DecRef(idx);
            return r;
        }
        g_py.PyErr_Clear();
        PyObj flt = g_py.PyNumber_Float(o);
        if (flt) {
            aval r = py_to_aval(flt);
            g_py.Py_DecRef(flt);
            return r;
        }
        g_py.PyErr_Clear();
    }
    /* Anything else (modules, functions, class instances) stays a live
       Python object so it can be passed back into py_call later. */
    return py_wrap_borrowed(o);
}

/* GIL must be held. Returns a new reference or NULL (error set). */
static PyObj aval_to_py(aval v) {
    if (a_is_none(v)) { g_py.Py_IncRef(g_py.none); return g_py.none; }
    if (v == A_TRUE) { g_py.Py_IncRef(g_py.py_true); return g_py.py_true; }
    if (v == A_FALSE) { g_py.Py_IncRef(g_py.py_false); return g_py.py_false; }
    if (a_is_int(v)) return g_py.PyLong_FromLongLong(a_int_val(v));
    if (a_is_double(v)) return g_py.PyFloat_FromDouble(a_double_val(v));
    AObj *o = obj_of(v);
    if (o) {
        switch (o->type) {
            case T_INT64:
                return g_py.PyLong_FromLongLong(((AInt64 *)o)->v);
            case T_STR:
            case T_CHAR: {
                uint64_t len = 0;
                const char *s = str_data(v, &len);
                return g_py.PyUnicode_DecodeUTF8(s, (PySsize)len, "strict");
            }
            case T_LIST:
            case T_TUPLE: {
                AArr *a = (AArr *)o;
                int isTuple = o->type == T_TUPLE;
                PyObj c;
                if (isTuple) c = g_py.PyTuple_New((PySsize)a->h.len);
                else c = g_py.PyList_New((PySsize)a->h.len);
                if (!c) return NULL;
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    /* arr_elem: packed-int lists keep elements in iitems
                       with items == NULL; reading items[] directly
                       segfaults. */
                    PyObj item = aval_to_py(arr_elem(a, i));
                    if (!item) { g_py.Py_DecRef(c); return NULL; }
                    int rc = isTuple ? g_py.PyTuple_SetItem(c, (PySsize)i, item)
                                     : g_py.PyList_SetItem(c, (PySsize)i, item);
                    if (rc < 0) { g_py.Py_DecRef(item); g_py.Py_DecRef(c); return NULL; }
                }
                return c;
            }
            case T_SET: {
                AArr *a = (AArr *)o;
                PyObj s = g_py.PySet_New(NULL);
                if (!s) return NULL;
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    PyObj item = aval_to_py(arr_elem(a, i));
                    if (!item) { g_py.Py_DecRef(s); return NULL; }
                    if (g_py.PySet_Add(s, item) < 0) {
                        g_py.Py_DecRef(item);
                        g_py.Py_DecRef(s);
                        return NULL;
                    }
                    g_py.Py_DecRef(item);
                }
                return s;
            }
            case T_DICT: {
                ADict *d = (ADict *)o;
                PyObj m = g_py.PyDict_New();
                if (!m) return NULL;
                for (uint64_t i = 0; i < d->h.len; ++i) {
                    PyObj k = aval_to_py(d->pairs[i].k);
                    PyObj val = k ? aval_to_py(d->pairs[i].v) : NULL;
                    if (!k || !val || g_py.PyDict_SetItem(m, k, val) < 0) {
                        if (k) g_py.Py_DecRef(k);
                        if (val) g_py.Py_DecRef(val);
                        g_py.Py_DecRef(m);
                        return NULL;
                    }
                    g_py.Py_DecRef(k);
                    g_py.Py_DecRef(val);
                }
                return m;
            }
            case T_RANGE: {
                ARange *r = (ARange *)o;
                /* count elements like the runtime iterator does */
                int64_t cnt = 0;
                if (r->step > 0) {
                    if (r->hi > r->lo) cnt = (r->hi - r->lo + r->step - 1) / r->step;
                } else if (r->step < 0) {
                    if (r->lo > r->hi) cnt = (r->lo - r->hi - r->step - 1) / (-r->step);
                }
                if (cnt < 0 || cnt > 10000000) return NULL;
                PyObj l = g_py.PyList_New((PySsize)cnt);
                if (!l) return NULL;
                int64_t cur = r->lo;
                for (PySsize i = 0; i < cnt; ++i) {
                    PyObj item = g_py.PyLong_FromLongLong(cur);
                    if (!item || g_py.PyList_SetItem(l, i, item) < 0) {
                        if (item) g_py.Py_DecRef(item);
                        g_py.Py_DecRef(l);
                        return NULL;
                    }
                    cur += r->step;
                }
                return l;
            }
            case T_VIEW: {
                AView *vw = (AView *)o;
                AArr *a = (AArr *)obj_of(vw->parent);
                if (!a || (a->h.type != T_LIST && a->h.type != T_TUPLE)) return NULL;
                PyObj l = g_py.PyList_New((PySsize)vw->len);
                if (!l) return NULL;
                for (PySsize i = 0; i < vw->len; ++i) {
                    /* arr_elem: the view's parent list may be packed. */
                    PyObj item = aval_to_py(arr_elem(a, (uint64_t)(vw->off + i)));
                    if (!item || g_py.PyList_SetItem(l, i, item) < 0) {
                        if (item) g_py.Py_DecRef(item);
                        g_py.Py_DecRef(l);
                        return NULL;
                    }
                }
                return l;
            }
            case T_RESULT: {
                AResult *r = (AResult *)o;
                return aval_to_py(r->value);
            }
            case T_PYOBJ: {
                APyObj *p = (APyObj *)o;
                g_py.Py_IncRef(p->obj);
                return p->obj;
            }
            case T_PYBUF: {
                /* Hand the original Python object back so a buffer view
                   round-trips as the exporter it was taken from. */
                APyBuf *b = (APyBuf *)o;
                if (b->owner) {
                    g_py.Py_IncRef((PyObj)b->owner);
                    return (PyObj)b->owner;
                }
                return NULL;
            }
            default:
                break;
        }
    }
    /* fallback: the value's repr as a Python string */
    aval rs = a_repr(v);
    uint64_t len = 0;
    const char *s = str_data(rs, &len);
    return g_py.PyUnicode_DecodeUTF8(s, (PySsize)len, "strict");
}

/* GIL must be held. Never returns. */
static void py_fail(const char *what) {
    char detail[384];
    detail[0] = 0;
    if (g_py.PyErr_Occurred()) {
        PyObj t = NULL, v = NULL, tb = NULL;
        g_py.PyErr_Fetch(&t, &v, &tb);
        g_py.PyErr_NormalizeException(&t, &v, &tb);
        if (v) {
            PyObj s = g_py.PyObject_Str(v);
            if (s) {
                const char *u = g_py.PyUnicode_AsUTF8AndSize(s, NULL);
                if (u) snprintf(detail, sizeof detail, "%s", u);
                g_py.Py_DecRef(s);
            }
        }
        if (t) g_py.Py_DecRef(t);
        if (v) g_py.Py_DecRef(v);
        if (tb) g_py.Py_DecRef(tb);
    }
    panicf("python %s failed%s%s", what, detail[0] ? ": " : "", detail);
}

/* py_call: argv = [module:str, member:str, nkw:int,
                   pos1..posP, (kname:str, kval)*nkw].
   member may be dotted. kwargs are passed to CPython as a real dict via
   PyObject_Call(fn, args, kwargs_dict) — same model as Mojo's
   PythonObject.__call__(**kwargs). */
aval bi_py_call(uint64_t n, aval *argv) {
    want_args("py_call", n, 3, UINT64_MAX);
    if (!a_is_int(argv[2])) panicf("py_call: bad kwargs count");
    int64_t nkw = a_int_val(argv[2]);
    if (nkw < 0 || (uint64_t)(3 + 2 * nkw) > n)
        panicf("py_call: malformed kwargs");
    uint64_t posc = n - 3 - (uint64_t)(2 * nkw);
    uint64_t mlen = 0, flen = 0;
    const char *modraw = str_data(argv[0], &mlen);
    const char *memraw = str_data(argv[1], &flen);
    /* Scratch copies are raw malloc (untracked by the conservative
       collector): the collector can never free them under CPython
       calls, so no explicit roots are needed and they are never
       mis-flagged as heap objects. Paired free() below is a_gc_free,
       which raw-frees untracked blocks. */
    char *mod = (char *)malloc((size_t)mlen + 1);
    if (!mod) panicf("out of memory");
    memcpy(mod, modraw, (size_t)mlen);
    mod[mlen] = '\0';
    char *mem = (char *)malloc((size_t)flen + 1);
    if (!mem) { free(mod); panicf("out of memory"); }
    memcpy(mem, memraw, (size_t)flen);
    mem[flen] = '\0';
    if (!py_ensure_ready()) {
        free(mod); free(mem);
        panicf("python interop unavailable: %s", g_py.error);
    }
    int gil = g_py.PyGILState_Ensure();
    PyObj m = g_py.PyImport_ImportModule(mod);
    if (!m) { free(mod); free(mem); py_fail("import"); }
    /* resolve (possibly dotted) member; fn ends as an owned reference */
    PyObj fn = m;
    char *save = NULL;
    for (char *seg = strtok_r(mem, ".", &save); seg;
         seg = strtok_r(NULL, ".", &save)) {
        PyObj next = g_py.PyObject_GetAttrString(fn, seg);
        g_py.Py_DecRef(fn);
        fn = next;
        if (!fn) { free(mod); free(mem); py_fail("attribute access"); }
    }
    PySsize nargs = (PySsize)posc;
    PyObj args = g_py.PyTuple_New(nargs);
    if (!args) { g_py.Py_DecRef(fn); free(mod); free(mem); py_fail("call setup"); }
    for (uint64_t i = 0; i < posc; ++i) {
        PyObj a = aval_to_py(argv[3 + i]);
        if (!a) {
            g_py.Py_DecRef(args); g_py.Py_DecRef(fn);
            free(mod); free(mem);
            py_fail("argument conversion");
        }
        g_py.PyTuple_SetItem(args, (PySsize)i, a); /* steals a */
    }
    PyObj kw = NULL;
    if (nkw > 0) {
        kw = g_py.PyDict_New();
        if (!kw) {
            g_py.Py_DecRef(args); g_py.Py_DecRef(fn);
            free(mod); free(mem); py_fail("kwargs setup");
        }
        for (int64_t k = 0; k < nkw; ++k) {
            aval kv = argv[3 + posc + 2 * (uint64_t)k];
            aval vv = argv[3 + posc + 2 * (uint64_t)k + 1];
            uint64_t knlen = 0;
            const char *kn = str_data(kv, &knlen);
            PyObj key = g_py.PyUnicode_DecodeUTF8(kn, (PySsize)knlen, "strict");
            PyObj val = aval_to_py(vv);
            if (!key || !val || g_py.PyDict_SetItem(kw, key, val) < 0) {
                if (key) g_py.Py_DecRef(key);
                if (val) g_py.Py_DecRef(val);
                g_py.Py_DecRef(kw); g_py.Py_DecRef(args); g_py.Py_DecRef(fn);
                free(mod); free(mem);
                py_fail("kwargs conversion");
            }
            g_py.Py_DecRef(key); /* SetItem incref'd it */
            g_py.Py_DecRef(val); /* SetItem incref'd it */
        }
    }
    PyObj res = g_py.PyObject_Call(fn, args, kw);
    if (kw) g_py.Py_DecRef(kw);
    g_py.Py_DecRef(args);
    g_py.Py_DecRef(fn);
    free(mod); free(mem);
    if (!res) py_fail("call");
    aval out = py_to_aval(res);
    g_py.Py_DecRef(res);
    g_py.PyGILState_Release(gil);
    return out;
}

/* ------------------------------------------------------------------ */
/* opaque-object bridge: T_PYOBJ values behave like Python objects.    */
/* Every entry point ensures the interpreter, takes the GIL, and      */
/* panicf's (via py_fail) with the Python error text on failure.      */
/* ------------------------------------------------------------------ */

static APyObj *pyobj_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_PYOBJ)
        panicf("%s must be a Python object, not '%s'", what, type_name_cstr(v));
    return (APyObj *)o;
}

static int py_ready_or_panic(void) {
    if (py_ensure_ready()) return 1;
    panicf("python interop unavailable: %s", g_py.error);
    return 0;
}

/* GIL held. Builds an args tuple of n positional avals; NULL on error. */
static PyObj py_args_tuple(uint64_t n, aval *argv) {
    PyObj args = g_py.PyTuple_New((PySsize)n);
    if (!args) return NULL;
    for (uint64_t i = 0; i < n; ++i) {
        PyObj a = aval_to_py(argv[i]);
        if (!a) { g_py.Py_DecRef(args); return NULL; }
        if (g_py.PyTuple_SetItem(args, (PySsize)i, a) < 0) {
            g_py.Py_DecRef(a);
            g_py.Py_DecRef(args);
            return NULL;
        }
    }
    return args;
}

/* GIL held. Builds a kwargs dict from nkw (name:str, value) aval pairs;
   NULL on error (Python error set). */
static PyObj py_kwargs_dict(uint64_t nkw, aval *pairs) {
    PyObj kw = g_py.PyDict_New();
    if (!kw) return NULL;
    for (uint64_t k = 0; k < nkw; ++k) {
        aval kv = pairs[2 * k];
        aval vv = pairs[2 * k + 1];
        uint64_t knlen = 0;
        const char *kn = str_data(kv, &knlen);
        PyObj key = g_py.PyUnicode_DecodeUTF8(kn, (PySsize)knlen, "strict");
        PyObj val = aval_to_py(vv);
        if (!key || !val || g_py.PyDict_SetItem(kw, key, val) < 0) {
            if (key) g_py.Py_DecRef(key);
            if (val) g_py.Py_DecRef(val);
            g_py.Py_DecRef(kw);
            return NULL;
        }
        g_py.Py_DecRef(key); /* SetItem incref'd it */
        g_py.Py_DecRef(val); /* SetItem incref'd it */
    }
    return kw;
}

/* GIL held. Calls fn(args..., ) with no kwargs; new ref or NULL. */
static PyObj py_call_pos(PyObj fn, uint64_t n, aval *argv) {
    PyObj args = py_args_tuple(n, argv);
    if (!args) return NULL;
    PyObj r = g_py.PyObject_Call(fn, args, NULL);
    g_py.Py_DecRef(args);
    return r;
}

/* Converts a call result and releases the GIL. Never returns on error. */
static aval py_take_result(PyObj r, int gil, const char *what) {
    if (!r) py_fail(what);
    aval out = py_to_aval(r);
    g_py.Py_DecRef(r);
    g_py.PyGILState_Release(gil);
    return out;
}

aval a_py_getattr(aval obj, const char *name) {
    APyObj *p = pyobj_of(obj, "attribute access target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj r = g_py.PyObject_GetAttrString(p->obj, name);
    return py_take_result(r, gil, "attribute access");
}

aval a_py_setattr(aval obj, const char *name, aval v) {
    APyObj *p = pyobj_of(obj, "attribute assignment target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj val = aval_to_py(v);
    if (!val) py_fail("attribute value conversion");
    int rc = g_py.PyObject_SetAttrString(p->obj, name, val);
    g_py.Py_DecRef(val); /* SetAttrString does not steal */
    if (rc < 0) py_fail("attribute assignment");
    g_py.PyGILState_Release(gil);
    return v;
}

aval a_py_getitem(aval obj, aval key) {
    APyObj *p = pyobj_of(obj, "subscript target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj k = aval_to_py(key);
    if (!k) py_fail("subscript key conversion");
    PyObj r = g_py.PyObject_GetItem(p->obj, k);
    g_py.Py_DecRef(k); /* GetItem does not steal the key */
    return py_take_result(r, gil, "subscript");
}

aval a_py_setitem(aval obj, aval key, aval v) {
    APyObj *p = pyobj_of(obj, "item assignment target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj k = aval_to_py(key);
    PyObj val = k ? aval_to_py(v) : NULL;
    if (!k || !val) py_fail("item assignment conversion");
    int rc = g_py.PyObject_SetItem(p->obj, k, val);
    g_py.Py_DecRef(k);   /* SetItem steals neither key nor value */
    g_py.Py_DecRef(val);
    if (rc < 0) py_fail("item assignment");
    g_py.PyGILState_Release(gil);
    return v;
}

aval a_py_len(aval obj) {
    APyObj *p = pyobj_of(obj, "len() argument");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PySsize n = g_py.PyObject_Length(p->obj);
    /* -1 is only an error when the interpreter says so; some types
       (e.g. custom __len__ edge cases) report -1 without an error. */
    if (n < 0 && g_py.PyErr_Occurred()) py_fail("len()");
    aval out = mkint_full(n < 0 ? 0 : (int64_t)n);
    g_py.PyGILState_Release(gil);
    return out;
}

aval a_py_contains(aval obj, aval item) {
    APyObj *p = pyobj_of(obj, "membership test container");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj k = aval_to_py(item);
    if (!k) py_fail("membership test value conversion");
    int r = g_py.PySequence_Contains(p->obj, k);
    g_py.Py_DecRef(k); /* Contains does not steal */
    if (r < 0) py_fail("membership test");
    aval out = bool_aval(r == 1);
    g_py.PyGILState_Release(gil);
    return out;
}

aval a_py_call(aval fn, uint64_t n, aval *argv) {
    APyObj *p = pyobj_of(fn, "call target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj r = py_call_pos(p->obj, n, argv);
    return py_take_result(r, gil, "call");
}

/* argv = n positional args followed by nkw (name:str, value) pairs,
   passed to CPython as a real kwargs dict (same model as bi_py_call). */
aval a_py_call_method(aval obj, const char *name, uint64_t n, uint64_t nkw,
                      aval *argv) {
    APyObj *p = pyobj_of(obj, "method call target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj m = g_py.PyObject_GetAttrString(p->obj, name);
    if (!m) py_fail("attribute access");
    PyObj args = py_args_tuple(n, argv);
    PyObj kw = NULL;
    if (args && nkw) {
        kw = py_kwargs_dict(nkw, argv + n);
        if (!kw) { g_py.Py_DecRef(args); args = NULL; }
    }
    PyObj r = args ? g_py.PyObject_Call(m, args, kw) : NULL;
    if (kw) g_py.Py_DecRef(kw);
    if (args) g_py.Py_DecRef(args);
    g_py.Py_DecRef(m);
    return py_take_result(r, gil, "method call");
}

aval a_py_iter_begin(aval v) {
    APyObj *p = pyobj_of(v, "iteration target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj it = g_py.PyObject_GetIter(p->obj);
    if (!it) py_fail("iteration");
    aval out = py_wrap_owned(it); /* takes the new reference */
    g_py.PyGILState_Release(gil);
    return out;
}

int a_py_iter_next(aval it, aval *out) {
    APyObj *p = pyobj_of(it, "iterator");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj item = g_py.PyIter_Next(p->obj);
    if (!item) {
        /* NULL + error set = real failure (propagate, unlike Mojo's
           _PyIter which conflates it with StopIteration); NULL with a
           clean error indicator = StopIteration. */
        if (g_py.PyErr_Occurred()) py_fail("iteration");
        g_py.PyGILState_Release(gil);
        return 0;
    }
    *out = py_to_aval(item);
    g_py.Py_DecRef(item);
    g_py.PyGILState_Release(gil);
    return 1;
}

static aval py_to_pystr(PyObj o, int use_repr) {
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj s = use_repr ? g_py.PyObject_Repr(o) : g_py.PyObject_Str(o);
    aval out = py_take_result(s, gil, use_repr ? "repr()" : "str()");
    return out;
}

aval a_py_str(aval obj) {
    APyObj *p = pyobj_of(obj, "str() argument");
    return py_to_pystr(p->obj, 0);
}

aval a_py_repr(aval obj) {
    APyObj *p = pyobj_of(obj, "repr() argument");
    return py_to_pystr(p->obj, 1);
}

int a_py_truthy(aval obj) {
    APyObj *p = pyobj_of(obj, "truthiness test");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    int r = g_py.PyObject_IsTrue(p->obj);
    if (r < 0) py_fail("truthiness test");
    g_py.PyGILState_Release(gil);
    return r == 1;
}

int a_py_equals(aval a, aval b) {
    APyObj *pa = pyobj_of(a, "equality operand");
    APyObj *pb = pyobj_of(b, "equality operand");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    /* Py_EQ == 2 (CPython rich-compare op ordering: LT, LE, EQ, NE, GT, GE) */
    int r = g_py.PyObject_RichCompareBool(pa->obj, pb->obj, 2);
    if (r < 0) py_fail("equality comparison");
    g_py.PyGILState_Release(gil);
    return r == 1;
}

/* ------------------------------------------------------------------ */
/* zero-copy buffer-protocol views (T_PYBUF)                           */
/* ------------------------------------------------------------------ */

static int pybuf_host_little(void) {
    const unsigned x = 1;
    return *(const unsigned char *)&x != 0;
}

/* Decode a PEP 3118 format string into an APYBUF_* kind; 0 = unsupported.
   `itemsize` disambiguates 'l'/'L' (C long is 4 or 8 bytes). Only single
   native/little/big-endian scalar formats are accepted — compound and
   repeated formats panic at view creation. */
static int pybuf_kind(const char *fmt, PySsize itemsize) {
    if (!fmt || !*fmt) return APYBUF_U8; /* NULL format means unsigned bytes */
    const char *p = fmt;
    int little = pybuf_host_little();
    if (*p == '@' || *p == '=') ++p;                 /* native byte order */
    else if (*p == '<') { if (!little) return 0; ++p; }
    else if (*p == '>' || *p == '!') { if (little) return 0; ++p; }
    long count = 0;
    int hasCount = 0;
    while (*p >= '0' && *p <= '9') {
        hasCount = 1;
        count = count * 10 + (*p - '0');
        if (count > 1000000) return 0;
        ++p;
    }
    if (hasCount && count != 1) return 0;
    char c = *p;
    if (!c || p[1] != '\0') return 0; /* trailing spec / nested format */
    switch (c) {
        case 'B': return APYBUF_U8;
        case 'b': case 'c': return APYBUF_I8;
        case '?': return APYBUF_BOOL;
        case 'H': return APYBUF_U16;
        case 'h': return APYBUF_I16;
        case 'I': return APYBUF_U32;
        case 'i': return APYBUF_I32;
        case 'L': return itemsize == 8 ? APYBUF_U64
                    : itemsize == 4 ? APYBUF_U32 : 0;
        case 'l': return itemsize == 8 ? APYBUF_I64
                    : itemsize == 4 ? APYBUF_I32 : 0;
        case 'Q': return APYBUF_U64;
        case 'q': return APYBUF_I64;
        case 'f': return APYBUF_F32;
        case 'd': return APYBUF_F64;
        default: return 0;
    }
}

/* Defensive C-contiguity check (we request PyBUF_C_CONTIGUOUS, so this
   should always pass; guards against exporter quirks). */
static int pybuf_c_contiguous(PyBufferView *v) {
    if (v->ndim <= 1) return 1;
    if (!v->shape || !v->strides) return 0;
    for (int i = 0; i < v->ndim; ++i)
        if (v->shape[i] == 0) return 1; /* empty: trivially contiguous */
    PySsize expect = v->itemsize;
    for (int i = v->ndim - 1; i >= 0; --i) {
        if (v->strides[i] != expect) return 0;
        if (i > 0) expect *= v->shape[i];
    }
    return 1;
}

/* Live views hold the exported buffer through a private PyBuffer copy.
   The GC calls a_py_release_obj() when the A view becomes unreachable,
   which releases that buffer (and the exporter reference it owns). */
typedef struct PyBufNode {
    APyBuf *buf;
    struct PyBufNode *next;
} PyBufNode;
static PyBufNode *g_pybufs;
static int g_pybuf_atexit_registered;

/* Bookkeeping (PyBufNode + PyBufferView copy) is raw malloc and
   INVISIBLE to the conservative collector. Rooting the list would
   pin every APyBuf alive forever (a leak); tracking it as GC objects
   would let sweep free the nodes without unlinking (a UAF). Lifetime
   is explicit: a_py_release_obj unlinks+frees when the A view dies,
   pybuf_release_all drains the remainder at exit. */
static void pybuf_release_all(void) {
    if (g_py.state != 1 || !g_py.initialized) return;
    if (!g_pybufs) return;
    int gil = g_py.PyGILState_Ensure();
    PyBufNode *n = g_pybufs;
    g_pybufs = NULL;
    while (n) {
        PyBufNode *next = n->next;
        APyBuf *b = n->buf;
        if (b && b->state) {
            g_py.PyBuffer_Release((PyBufferView *)b->state);
            free(b->state);
            b->state = NULL;
        }
        free(n);
        n = next;
    }
    g_py.PyGILState_Release(gil);
}

void a_py_release_obj(void *p) {
    AObj *o = (AObj *)p;
    if (!o || (o->type != T_PYOBJ && o->type != T_PYBUF)) return;
    if (g_py.state != 1 || !g_py.initialized) {
        if (o->type == T_PYOBJ) ((APyObj *)o)->obj = NULL;
        else ((APyBuf *)o)->state = NULL;
        return;
    }
    int gil = g_py.PyGILState_Ensure();
    if (o->type == T_PYOBJ) {
        APyObj *po = (APyObj *)o;
        if (po->obj) {
            g_py.Py_DecRef(po->obj);
            po->obj = NULL;
        }
    } else {
        APyBuf *b = (APyBuf *)o;
        PyBufNode **pp = &g_pybufs;
        while (*pp) {
            if ((*pp)->buf == b) {
                PyBufNode *node = *pp;
                *pp = node->next;
                free(node);
                break;
            }
            pp = &(*pp)->next;
        }
        if (b->state) {
            g_py.PyBuffer_Release((PyBufferView *)b->state);
            free(b->state);
            b->state = NULL;
        }
    }
    g_py.PyGILState_Release(gil);
}

aval a_py_buffer_view(aval obj) {
    AObj *o = obj_of(obj);
    if (o && o->type == T_PYBUF) return obj; /* already a view */
    APyObj *p = pyobj_of(obj, "py_buffer() argument");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyBufferView bv;
    memset(&bv, 0, sizeof bv);
    /* C-contiguous + format: accepts any C-order n-d buffer and reports
       the PEP 3118 element format; fails for strided/non-contiguous
       exporters (documented v1 limitation). */
    if (g_py.PyObject_GetBuffer(p->obj, &bv,
                                PYBUF_C_CONTIGUOUS | PYBUF_FORMAT) < 0)
        py_fail("buffer export");
    int kind = pybuf_kind(bv.format, bv.itemsize);
    if (!bv.buf || bv.itemsize <= 0 || bv.len < 0 || bv.len % bv.itemsize != 0 ||
        !kind || !pybuf_c_contiguous(&bv)) {
        /* bv.format is exporter-owned; copy it before releasing. */
        char fmtbuf[80];
        snprintf(fmtbuf, sizeof fmtbuf, "%s", bv.format ? bv.format : "(none)");
        g_py.PyBuffer_Release(&bv);
        g_py.PyGILState_Release(gil);
        if (!kind)
            panicf("py_buffer: unsupported buffer format '%s'", fmtbuf);
        if (!bv.buf || bv.itemsize <= 0 || bv.len < 0 ||
            bv.len % bv.itemsize != 0)
            panicf("py_buffer: malformed buffer export");
        panicf("py_buffer: buffer is not C-contiguous");
    }
    APyBuf *b = (APyBuf *)xmalloc(sizeof(APyBuf));
    a_gc_push_root(mkobj((void *)b));
    b->h.type = T_PYBUF;
    b->h.len = 0;
    b->h.cap = 0;
    b->count = bv.len / bv.itemsize;
    b->itemsize = bv.itemsize;
    b->kind = kind;
    b->readonly = bv.readonly != 0;
    b->data = (char *)bv.buf;
    b->owner = bv.obj;
    /* No extra IncRef: the Py_buffer copy kept in `state` already owns a
       reference on bv.obj until PyBuffer_Release at process exit, which is
       exactly the view's lifetime. aval_to_py() IncRefs again when handing
       the owner back to Python. */
    /* Keep a private copy of the Py_buffer for PyBuffer_Release. The
       exporter-owned pointers (shape/strides/format/internal) stay valid
       until release, which happens when a_py_release_obj unlinks the
       view or at process exit. Raw malloc (untracked): the collector
       can never free these without unlinking, and the live-view list
       therefore pins nothing. */
    PyBufferView *keep = (PyBufferView *)malloc(sizeof(PyBufferView));
    if (!keep) { g_py.PyBuffer_Release(&bv); g_py.PyGILState_Release(gil); panicf("out of memory"); }
    *keep = bv;
    b->state = keep;
    PyBufNode *node = (PyBufNode *)malloc(sizeof(PyBufNode));
    if (!node) { g_py.PyBuffer_Release(keep); free(keep); b->state = NULL; g_py.PyGILState_Release(gil); panicf("out of memory"); }
    node->buf = b;
    node->next = g_pybufs;
    g_pybufs = node;
    if (!g_pybuf_atexit_registered) {
        g_pybuf_atexit_registered = 1;
        atexit(pybuf_release_all);
    }
    g_py.PyGILState_Release(gil);
    a_gc_pop_root();
    return mkobj(&b->h);
}

aval bi_py_buffer(uint64_t n, aval *argv) {
    want_args("py_buffer", n, 1, 1);
    return a_py_buffer_view(argv[0]);
}

/* ---- extended py helpers (dynamic/explicit interop + Result variants) ---- */

static aval py_err_to_aval(const char *fallback) {
    char detail[512];
    detail[0] = 0;
    if (g_py.PyErr_Occurred()) {
        PyObj t = NULL, v = NULL, tb = NULL;
        g_py.PyErr_Fetch(&t, &v, &tb);
        g_py.PyErr_NormalizeException(&t, &v, &tb);
        if (v) {
            PyObj es = g_py.PyObject_Str(v);
            if (es) {
                PySsize elen = 0;
                const char *u = g_py.PyUnicode_AsUTF8AndSize(es, &elen);
                if (u) snprintf(detail, sizeof detail, "%s", u);
                g_py.Py_DecRef(es);
            }
        }
        if (t) g_py.Py_DecRef(t);
        if (v) g_py.Py_DecRef(v);
        if (tb) g_py.Py_DecRef(tb);
    } else if (fallback) {
        snprintf(detail, sizeof detail, "%s", fallback);
    }
    if (!detail[0] && fallback) snprintf(detail, sizeof detail, "%s", fallback);
    return a_str_new(detail, strlen(detail));
}

static aval py_ok(aval v) { return mkobj(result_new(1, 0, v)); }
static aval py_errv(aval msg) { return mkobj(result_new(0, 0, msg)); }
static aval py_err_cstr(const char *msg) { return py_errv(a_str_new(msg, strlen(msg))); }

static const char *aval_cstr_tmp(aval v, uint64_t *len, char *buf, size_t bufsz) {
    *len = 0;
    const char *s = str_data(v, len);
    if (!s) s = "";
    if (*len >= bufsz) *len = bufsz - 1;
    memcpy(buf, s, (size_t)*len);
    buf[*len] = 0;
    return buf;
}

aval a_py_import_module(const char *name) {
    if (!py_ensure_ready()) panicf("python interop unavailable: %s", g_py.error);
    int gil = g_py.PyGILState_Ensure();
    PyObj m = g_py.PyImport_ImportModule(name);
    if (!m) py_fail("import");
    aval out = py_wrap_owned(m);
    g_py.PyGILState_Release(gil);
    return out;
}

aval bi_py_import_module(uint64_t n, aval *argv) {
    want_args("py.import", n, 1, 1);
    uint64_t len = 0; char buf[1024];
    aval_cstr_tmp(argv[0], &len, buf, sizeof buf);
    return a_py_import_module(buf);
}

aval bi_py_try_import_module(uint64_t n, aval *argv) {
    want_args("py.try_import", n, 1, 1);
    uint64_t len = 0; char buf[1024];
    aval_cstr_tmp(argv[0], &len, buf, sizeof buf);
    if (!py_ensure_ready()) return py_err_cstr(g_py.error);
    int gil = g_py.PyGILState_Ensure();
    PyObj m = g_py.PyImport_ImportModule(buf);
    if (!m) { aval e = py_err_to_aval("import failed"); g_py.PyGILState_Release(gil); return py_errv(e); }
    aval out = py_wrap_owned(m);
    g_py.PyGILState_Release(gil);
    return py_ok(out);
}

aval a_py_hasattr(aval obj, const char *name) {
    APyObj *p = pyobj_of(obj, "hasattr() target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    int r = g_py.PyObject_HasAttrString ? g_py.PyObject_HasAttrString(p->obj, name) : 0;
    if (r < 0) { g_py.PyErr_Clear(); r = 0; }
    g_py.PyGILState_Release(gil);
    return bool_aval(r != 0);
}

aval bi_py_hasattr(uint64_t n, aval *argv) {
    want_args("py.hasattr", n, 2, 2);
    uint64_t len = 0; char buf[512];
    aval_cstr_tmp(argv[1], &len, buf, sizeof buf);
    return a_py_hasattr(argv[0], buf);
}

aval a_py_delattr(aval obj, const char *name) {
    APyObj *p = pyobj_of(obj, "delattr() target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    if (!g_py.PyObject_DelAttrString) { g_py.PyGILState_Release(gil); panicf("python delattr unavailable"); }
    int rc = g_py.PyObject_DelAttrString(p->obj, name);
    if (rc < 0) py_fail("delattr");
    g_py.PyGILState_Release(gil);
    return A_NONE;
}

aval bi_py_delattr(uint64_t n, aval *argv) {
    want_args("py.delattr", n, 2, 2);
    uint64_t len = 0; char buf[512];
    aval_cstr_tmp(argv[1], &len, buf, sizeof buf);
    return a_py_delattr(argv[0], buf);
}

aval a_py_delitem(aval obj, aval key) {
    APyObj *p = pyobj_of(obj, "delitem() target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj k = aval_to_py(key);
    if (!k) py_fail("delitem key conversion");
    if (!g_py.PyObject_DelItem) { g_py.Py_DecRef(k); g_py.PyGILState_Release(gil); panicf("python delitem unavailable"); }
    int rc = g_py.PyObject_DelItem(p->obj, k);
    g_py.Py_DecRef(k);
    if (rc < 0) py_fail("delitem");
    g_py.PyGILState_Release(gil);
    return A_NONE;
}

aval bi_py_delitem(uint64_t n, aval *argv) {
    want_args("py.delitem", n, 2, 2);
    return a_py_delitem(argv[0], argv[1]);
}

aval bi_py_getattr_builtin(uint64_t n, aval *argv) {
    want_args("py.getattr", n, 2, 3);
    uint64_t len = 0; char buf[512];
    aval_cstr_tmp(argv[1], &len, buf, sizeof buf);
    if (n == 2) return a_py_getattr(argv[0], buf);
    /* 3 args: default on missing attribute */
    APyObj *p = pyobj_of(argv[0], "getattr() target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj r = g_py.PyObject_GetAttrString(p->obj, buf);
    if (!r) {
        g_py.PyErr_Clear();
        g_py.PyGILState_Release(gil);
        return argv[2];
    }
    return py_take_result(r, gil, "attribute access");
}

aval bi_py_try_getattr_builtin(uint64_t n, aval *argv) {
    want_args("py.try_getattr", n, 2, 2);
    uint64_t len = 0; char buf[512];
    aval_cstr_tmp(argv[1], &len, buf, sizeof buf);
    if (!py_ensure_ready()) return py_err_cstr(g_py.error);
    APyObj *p = NULL;
    AObj *o = obj_of(argv[0]);
    if (!o || o->type != T_PYOBJ) return py_err_cstr("getattr() target must be a Python object");
    p = (APyObj *)o;
    int gil = g_py.PyGILState_Ensure();
    PyObj r = g_py.PyObject_GetAttrString(p->obj, buf);
    if (!r) { aval e = py_err_to_aval("attribute access failed"); g_py.PyGILState_Release(gil); return py_errv(e); }
    aval out = py_to_aval(r);
    g_py.Py_DecRef(r);
    g_py.PyGILState_Release(gil);
    return py_ok(out);
}

aval bi_py_setattr_builtin(uint64_t n, aval *argv) {
    want_args("py.setattr", n, 3, 3);
    uint64_t len = 0; char buf[512];
    aval_cstr_tmp(argv[1], &len, buf, sizeof buf);
    return a_py_setattr(argv[0], buf, argv[2]);
}

aval bi_py_getitem_builtin(uint64_t n, aval *argv) { want_args("py.getitem", n, 2, 2); return a_py_getitem(argv[0], argv[1]); }
aval bi_py_setitem_builtin(uint64_t n, aval *argv) { want_args("py.setitem", n, 3, 3); return a_py_setitem(argv[0], argv[1], argv[2]); }

/* Collects the aval elements of a list/tuple/set argument sequence.
   *out_owned is set when the returned array was freshly allocated (packed
   int storage cannot be borrowed as avals); the caller must free it. */
static int py_collect_args(aval seq, aval **out_items, uint64_t *out_n,
                           int *out_owned) {
    AObj *o = obj_of(seq);
    *out_items = NULL; *out_n = 0; *out_owned = 0;
    if (!o) return 0;
    if (o->type == T_LIST || o->type == T_TUPLE || o->type == T_SET) {
        AArr *a = (AArr *)o;
        if (arr_packed(a)) {
            /* Packed-int storage: items is NULL and elements live in
               iitems; materialize avals so callers can treat the result
               as a plain aval array. */
            aval *items = (aval *)xmalloc((size_t)(a->h.len ? a->h.len : 1) *
                                          sizeof(aval));
            for (uint64_t i = 0; i < a->h.len; ++i) items[i] = arr_elem(a, i);
            *out_items = items; *out_n = a->h.len; *out_owned = 1;
            return 1;
        }
        *out_items = a->items; *out_n = a->h.len; return 1;
    }
    return 0;
}

aval a_py_call_kw(aval fn, uint64_t n, uint64_t nkw, aval *argv) {
    APyObj *p = pyobj_of(fn, "call target");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    PyObj args = py_args_tuple(n, argv);
    PyObj kw = NULL;
    if (args && nkw) {
        kw = py_kwargs_dict(nkw, argv + n);
        if (!kw) { g_py.Py_DecRef(args); args = NULL; }
    }
    PyObj r = args ? g_py.PyObject_Call(p->obj, args, kw) : NULL;
    if (kw) g_py.Py_DecRef(kw);
    if (args) g_py.Py_DecRef(args);
    return py_take_result(r, gil, "call");
}

static aval py_call_obj_impl(aval fn, aval args_seq, aval kwargs_map, int as_result) {
    if (!py_ensure_ready()) {
        if (as_result) return py_err_cstr(g_py.error);
        panicf("python interop unavailable: %s", g_py.error);
    }
    AObj *fo = obj_of(fn);
    if (!fo || fo->type != T_PYOBJ) {
        if (as_result) return py_err_cstr("py.call: first argument must be a Python object");
        panicf("py.call: first argument must be a Python object, not '%s'", type_name_cstr(fn));
    }
    aval *aitems = NULL; uint64_t an = 0; int aitems_owned = 0;
    if (!a_is_none(args_seq) &&
        !py_collect_args(args_seq, &aitems, &an, &aitems_owned)) {
        if (as_result) return py_err_cstr("py.call: args must be a list or tuple");
        panicf("py.call: args must be a list or tuple, not '%s'", type_name_cstr(args_seq));
    }
    /* kwargs: dict/map -> (name,value) pairs */
    aval *kpairs = NULL; uint64_t kn = 0; aval *kflat = NULL;
    if (!a_is_none(kwargs_map)) {
        AObj *ko = obj_of(kwargs_map);
        if (ko && ko->type == T_DICT) {
            ADict *d = (ADict *)ko;
            kn = d->h.len;
            kflat = (aval *)xmalloc((size_t)(kn ? kn * 2 : 1) * sizeof(aval));
            for (uint64_t i = 0; i < kn; ++i) { kflat[2*i] = d->pairs[i].k; kflat[2*i+1] = d->pairs[i].v; }
            kpairs = kflat;
        } else if (as_result) {
            if (aitems_owned) free(aitems);
            return py_err_cstr("py.call: kwargs must be a dict/map or None");
        } else {
            if (aitems_owned) free(aitems);
            panicf("py.call: kwargs must be a dict/map or None, not '%s'", type_name_cstr(kwargs_map));
        }
    }
    int gil = g_py.PyGILState_Ensure();
    PyObj args = py_args_tuple(an, aitems ? aitems : (aval*)NULL);
    PyObj kw = NULL;
    if (args && kn) {
        kw = py_kwargs_dict(kn, kpairs);
        if (!kw) { g_py.Py_DecRef(args); args = NULL; }
    }
    PyObj r = args ? g_py.PyObject_Call(((APyObj*)fo)->obj, args, kw) : NULL;
    if (kw) g_py.Py_DecRef(kw);
    if (args) g_py.Py_DecRef(args);
    if (kflat) free(kflat);
    if (aitems_owned) free(aitems);
    if (!r) {
        if (as_result) { aval e = py_err_to_aval("call failed"); g_py.PyGILState_Release(gil); return py_errv(e); }
        py_fail("call");
    }
    aval out = py_to_aval(r);
    g_py.Py_DecRef(r);
    g_py.PyGILState_Release(gil);
    return as_result ? py_ok(out) : out;
}

aval bi_py_call_obj(uint64_t n, aval *argv) {
    want_args("py.call", n, 2, 3);
    return py_call_obj_impl(argv[0], argv[1], n >= 3 ? argv[2] : A_NONE, 0);
}
aval bi_py_try_call_obj(uint64_t n, aval *argv) {
    want_args("py.try_call", n, 2, 3);
    return py_call_obj_impl(argv[0], argv[1], n >= 3 ? argv[2] : A_NONE, 1);
}

static aval py_call_method_impl(aval obj, aval namev, aval args_seq, aval kwargs_map, int as_result) {
    if (!py_ensure_ready()) {
        if (as_result) return py_err_cstr(g_py.error);
        panicf("python interop unavailable: %s", g_py.error);
    }
    AObj *oo = obj_of(obj);
    if (!oo || oo->type != T_PYOBJ) {
        if (as_result) return py_err_cstr("py.call_method: target must be a Python object");
        panicf("py.call_method: target must be a Python object, not '%s'", type_name_cstr(obj));
    }
    uint64_t nlen = 0; char nbuf[512];
    aval_cstr_tmp(namev, &nlen, nbuf, sizeof nbuf);
    aval *aitems = NULL; uint64_t an = 0; int aitems_owned = 0;
    if (!a_is_none(args_seq) &&
        !py_collect_args(args_seq, &aitems, &an, &aitems_owned)) {
        if (as_result) return py_err_cstr("py.call_method: args must be a list or tuple");
        panicf("py.call_method: args must be a list or tuple");
    }
    aval *kflat = NULL; uint64_t kn = 0;
    if (!a_is_none(kwargs_map)) {
        AObj *ko = obj_of(kwargs_map);
        if (ko && ko->type == T_DICT) {
            ADict *d = (ADict *)ko;
            kn = d->h.len;
            kflat = (aval *)xmalloc((size_t)(kn ? kn * 2 : 1) * sizeof(aval));
            for (uint64_t i = 0; i < kn; ++i) { kflat[2*i] = d->pairs[i].k; kflat[2*i+1] = d->pairs[i].v; }
        } else if (as_result) {
            if (aitems_owned) free(aitems);
            return py_err_cstr("py.call_method: kwargs must be a dict/map or None");
        } else {
            if (aitems_owned) free(aitems);
            panicf("py.call_method: kwargs must be a dict/map or None");
        }
    }
    int gil = g_py.PyGILState_Ensure();
    PyObj m = g_py.PyObject_GetAttrString(((APyObj*)oo)->obj, nbuf);
    if (!m) {
        if (aitems_owned) free(aitems);
        if (kflat) free(kflat);
        if (as_result) { aval e = py_err_to_aval("attribute access failed"); g_py.PyGILState_Release(gil); return py_errv(e); }
        py_fail("attribute access");
    }
    PyObj args = py_args_tuple(an, aitems ? aitems : (aval*)NULL);
    PyObj kw = NULL;
    if (args && kn) {
        kw = py_kwargs_dict(kn, kflat);
        if (!kw) { g_py.Py_DecRef(args); args = NULL; }
    }
    PyObj r = args ? g_py.PyObject_Call(m, args, kw) : NULL;
    if (kw) g_py.Py_DecRef(kw);
    if (args) g_py.Py_DecRef(args);
    g_py.Py_DecRef(m);
    if (kflat) free(kflat);
    if (aitems_owned) free(aitems);
    if (!r) {
        if (as_result) { aval e = py_err_to_aval("method call failed"); g_py.PyGILState_Release(gil); return py_errv(e); }
        py_fail("method call");
    }
    aval out = py_to_aval(r);
    g_py.Py_DecRef(r);
    g_py.PyGILState_Release(gil);
    return as_result ? py_ok(out) : out;
}

aval bi_py_call_method_builtin(uint64_t n, aval *argv) {
    want_args("py.call_method", n, 3, 4);
    return py_call_method_impl(argv[0], argv[1], argv[2], n >= 4 ? argv[3] : A_NONE, 0);
}
aval bi_py_try_call_method_builtin(uint64_t n, aval *argv) {
    want_args("py.try_call_method", n, 3, 4);
    return py_call_method_impl(argv[0], argv[1], argv[2], n >= 4 ? argv[3] : A_NONE, 1);
}

aval bi_py_is_pyobject(uint64_t n, aval *argv) {
    want_args("py.is_pyobject", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    return bool_aval(o && o->type == T_PYOBJ);
}

aval bi_py_str_builtin(uint64_t n, aval *argv) { want_args("py.str", n, 1, 1); return a_py_str(argv[0]); }
aval bi_py_repr_builtin(uint64_t n, aval *argv) { want_args("py.repr", n, 1, 1); return a_py_repr(argv[0]); }
aval bi_py_len_builtin(uint64_t n, aval *argv) { want_args("py.len", n, 1, 1); return a_py_len(argv[0]); }
aval bi_py_contains_builtin(uint64_t n, aval *argv) { want_args("py.contains", n, 2, 2); return a_py_contains(argv[0], argv[1]); }
aval bi_py_truthy_builtin(uint64_t n, aval *argv) {
    want_args("py.truthy", n, 1, 1);
    return bool_aval(a_py_truthy(argv[0]));
}

aval bi_py_type_builtin(uint64_t n, aval *argv) {
    want_args("py.type", n, 1, 1);
    APyObj *p = pyobj_of(argv[0], "py.type() argument");
    py_ready_or_panic();
    int gil = g_py.PyGILState_Ensure();
    if (!g_py.PyObject_Type) { g_py.PyGILState_Release(gil); panicf("py.type unavailable"); }
    PyObj t = g_py.PyObject_Type(p->obj);
    if (!t) py_fail("type()");
    PyObj name = g_py.PyObject_GetAttrString(t, "__name__");
    g_py.Py_DecRef(t);
    if (!name) py_fail("type().__name__");
    PySsize len = 0;
    const char *s = g_py.PyUnicode_AsUTF8AndSize(name, &len);
    aval out = a_str_new(s ? s : "", len < 0 ? 0 : (uint64_t)len);
    g_py.Py_DecRef(name);
    g_py.PyGILState_Release(gil);
    return out;
}

aval bi_py_buffer_info(uint64_t n, aval *argv) {
    want_args("py.buffer_info", n, 1, 1);
    AObj *o = obj_of(argv[0]);
    if (!o || o->type != T_PYBUF)
        panicf("py.buffer_info() argument must be a py buffer, not '%s'", type_name_cstr(argv[0]));
    APyBuf *b = (APyBuf *)o;
    const char *kind = "unknown";
    switch (b->kind) {
        case APYBUF_U8: kind = "u8"; break; case APYBUF_I8: kind = "i8"; break;
        case APYBUF_BOOL: kind = "bool"; break; case APYBUF_U16: kind = "u16"; break;
        case APYBUF_I16: kind = "i16"; break; case APYBUF_U32: kind = "u32"; break;
        case APYBUF_I32: kind = "i32"; break; case APYBUF_U64: kind = "u64"; break;
        case APYBUF_I64: kind = "i64"; break; case APYBUF_F32: kind = "f32"; break;
        case APYBUF_F64: kind = "f64"; break; default: break;
    }
    aval d = a_dict_new();
    a_dict_set(d, a_str_new("kind", 4), a_str_new(kind, strlen(kind)));
    a_dict_set(d, a_str_new("itemsize", 8), mkint_full((int64_t)b->itemsize));
    a_dict_set(d, a_str_new("count", 5), mkint_full((int64_t)b->count));
    a_dict_set(d, a_str_new("readonly", 8), bool_aval(b->readonly));
    return d;
}

#else /* _WIN32: no dlopen; keep the symbols so the builtin table links */

aval bi_py_call(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("python interop is unavailable on Windows (libpython loading is not ported)");
    return A_NONE;
}

aval bi_py_buffer(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("python interop is unavailable on Windows (libpython loading is not ported)");
    return A_NONE;
}

static aval py_unsupported(void) {
    panicf("python interop is unavailable on Windows (libpython loading is not ported)");
    return A_NONE;
}
static aval py_unsupported_err(void) {
    const char *msg = "python interop is unavailable on Windows (libpython loading is not ported)";
    return mkobj(result_new(0, 0, a_str_new(msg, strlen(msg))));
}

aval a_py_getattr(aval obj, const char *name) { (void)obj; (void)name; return py_unsupported(); }
aval a_py_setattr(aval obj, const char *name, aval v) { (void)obj; (void)name; (void)v; return py_unsupported(); }
aval a_py_getitem(aval obj, aval key) { (void)obj; (void)key; return py_unsupported(); }
aval a_py_setitem(aval obj, aval key, aval v) { (void)obj; (void)key; (void)v; return py_unsupported(); }
aval a_py_len(aval obj) { (void)obj; return py_unsupported(); }
aval a_py_contains(aval obj, aval item) { (void)obj; (void)item; return py_unsupported(); }
aval a_py_call(aval fn, uint64_t n, aval *argv) { (void)fn; (void)n; (void)argv; return py_unsupported(); }
aval a_py_call_kw(aval fn, uint64_t n, uint64_t nkw, aval *argv) { (void)fn; (void)n; (void)nkw; (void)argv; return py_unsupported(); }
aval a_py_call_method(aval obj, const char *name, uint64_t n, uint64_t nkw, aval *argv) { (void)obj; (void)name; (void)n; (void)nkw; (void)argv; return py_unsupported(); }
aval a_py_iter_begin(aval v) { (void)v; return py_unsupported(); }
int a_py_iter_next(aval it, aval *out) { (void)it; (void)out; return py_unsupported(), 0; }
aval a_py_str(aval obj) { (void)obj; return py_unsupported(); }
aval a_py_repr(aval obj) { (void)obj; return py_unsupported(); }
int a_py_truthy(aval obj) { (void)obj; return py_unsupported(), 0; }
int a_py_equals(aval a, aval b) { (void)a; (void)b; return py_unsupported(), 0; }
aval a_py_buffer_view(aval obj) { (void)obj; return py_unsupported(); }
void a_py_release_obj(void *p) { (void)p; }
static void py_finalize(void) {}
static void pybuf_release_all(void) {}
aval a_py_hasattr(aval obj, const char *name) { (void)obj; (void)name; return py_unsupported(); }
aval a_py_delattr(aval obj, const char *name) { (void)obj; (void)name; return py_unsupported(); }
aval a_py_delitem(aval obj, aval key) { (void)obj; (void)key; return py_unsupported(); }
aval a_py_import_module(const char *name) { (void)name; return py_unsupported(); }

aval bi_py_import_module(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_try_import_module(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported_err(); }
aval bi_py_getattr_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_try_getattr_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported_err(); }
aval bi_py_setattr_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_hasattr(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_delattr(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_getitem_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_setitem_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_delitem(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_call_obj(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_try_call_obj(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported_err(); }
aval bi_py_call_method_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_try_call_method_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported_err(); }
aval bi_py_is_pyobject(uint64_t n, aval *argv) { (void)n; (void)argv; return bool_aval(0); }
aval bi_py_str_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_repr_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_len_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_contains_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_truthy_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_type_builtin(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }
aval bi_py_buffer_info(uint64_t n, aval *argv) { (void)n; (void)argv; return py_unsupported(); }

#endif /* !_WIN32 */

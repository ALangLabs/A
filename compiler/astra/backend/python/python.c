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

struct PyRt {
    void *handle;
    int state;          /* 0 = untried, 1 = ready, -1 = failed */
    int initialized;    /* Py_Initialize + sys.path setup done */
    char error[512];

    /* function pointers (all resolved with dlsym) */
    void (*Py_Initialize)(void);
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
    int (*PyObject_IsInstance)(PyObj, PyObj);
    void (*Py_IncRef)(PyObj);
    void (*Py_DecRef)(PyObj);
    PyObj (*PyNumber_Index)(PyObj);
    PyObj (*PyNumber_Float)(PyObj);
    char *(*PyBytes_AsString)(PyObj);
    PySsize (*PyBytes_Size)(PyObj);
    PyObj (*PySet_New)(PyObj);
    int (*PySet_Add)(PyObj, PyObj);

    /* data symbols: addresses of the real objects inside libpython */
    PyObj none, py_true, py_false;
    PyObj t_bool, t_long, t_float, t_unicode, t_bytes;
    PyObj t_list, t_tuple, t_dict, t_set, t_frozenset;

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
    A_PY_SYM(PyObject_IsInstance, "PyObject_IsInstance");
    A_PY_SYM(Py_IncRef, "Py_IncRef");
    A_PY_SYM(Py_DecRef, "Py_DecRef");
    A_PY_SYM(PyNumber_Index, "PyNumber_Index");
    A_PY_SYM(PyNumber_Float, "PyNumber_Float");
    A_PY_SYM(PyBytes_AsString, "PyBytes_AsString");
    A_PY_SYM(PyBytes_Size, "PyBytes_Size");
    A_PY_SYM(PySet_New, "PySet_New");
    A_PY_SYM(PySet_Add, "PySet_Add");
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

/* One-time interpreter init after a successful dlopen. */
static void py_init_once(void) {
    if (g_py.initialized) return;
    const char *oldHome = getenv("PYTHONHOME");
    char savedHome[1024];
    savedHome[0] = 0;
    if (oldHome) snprintf(savedHome, sizeof savedHome, "%s", oldHome);
    if (g_py.py_home) setenv("PYTHONHOME", g_py.py_home, 1);
    g_py.Py_Initialize();
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
            PyObj it = g_py.PyObject_GetIter(o);
            if (it) {
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
    APyObj *p = (APyObj *)xmalloc(sizeof(APyObj));
    p->h.type = T_PYOBJ;
    p->h.len = 0;
    p->h.cap = 0;
    p->obj = o;
    g_py.Py_IncRef(o);
    return mkobj(&p->h);
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
            case T_TUPLE:
            case T_SET: {
                AArr *a = (AArr *)o;
                int isTuple = o->type == T_TUPLE;
                PyObj c;
                if (isTuple) c = g_py.PyTuple_New((PySsize)a->h.len);
                else c = g_py.PyList_New((PySsize)a->h.len);
                if (!c) return NULL;
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    PyObj item = aval_to_py(a->items[i]);
                    if (!item) { g_py.Py_DecRef(c); return NULL; }
                    int rc = isTuple ? g_py.PyTuple_SetItem(c, (PySsize)i, item)
                                     : g_py.PyList_SetItem(c, (PySsize)i, item);
                    if (rc < 0) { g_py.Py_DecRef(item); g_py.Py_DecRef(c); return NULL; }
                }
                return c;
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
                    PyObj item = aval_to_py(a->items[(size_t)(vw->off + i)]);
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
    char *mod = xstrndup(modraw, (size_t)mlen);
    char *mem = xstrndup(memraw, (size_t)flen);
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

#else /* _WIN32: no dlopen; keep the symbol so the builtin table links */

aval bi_py_call(uint64_t n, aval *argv) {
    (void)n; (void)argv;
    panicf("python interop is not supported on this platform");
    return A_NONE;
}

#endif /* !_WIN32 */

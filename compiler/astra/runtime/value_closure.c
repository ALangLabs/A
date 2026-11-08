#include "a_runtime_internal.h"

static AFnon *fnon_of(aval v, const char *what) {
    AObj *o = obj_of(v);
    if (!o || o->type != T_FNON)
        panicf("%s must be callable, not '%s'", what, type_name_cstr(v));
    return (AFnon *)o;
}

aval fnon_new_raw(uint64_t entry, aval env, const char *name,
                         uint32_t flags, uint64_t nparams) {
    AFnon *f = (AFnon *)xmalloc(sizeof(AFnon));
    f->h.type = T_FNON;
    f->h.len = 0;
    f->h.cap = 0;
    f->entry = entry;
    f->env = env;
    f->nparams = nparams;
    f->flags = flags;
    f->name = name ? xstrdup(name) : NULL;
    f->sig = NULL;
    f->doc = NULL;
    return mkobj(&f->h);
}

aval a_fnon_new(uint64_t entry, aval env, const char *name) {
    return fnon_new_raw(entry, env, name, 0, NPARAMS_UNKNOWN);
}

aval a_fnon_new_ex(uint64_t entry, aval env, const char *name,
                   const char *sig, const char *doc) {
    aval v = fnon_new_raw(entry, env, name, 0, NPARAMS_UNKNOWN);
    AFnon *f = (AFnon *)obj_of(v);
    f->sig = sig && *sig ? xstrdup(sig) : NULL;
    f->doc = doc && *doc ? xstrdup(doc) : NULL;
    return v;
}

aval a_closure_env_new(uint64_t n) {
    AEnv *e = (AEnv *)xmalloc(sizeof(AEnv) + (size_t)(n ? n : 1) * sizeof(aval));
    e->h.type = T_ENV;
    e->h.len = (uint32_t)n;
    e->h.cap = n;
    for (uint64_t i = 0; i < n; ++i) e->slots[i] = A_NONE;
    return mkobj(&e->h);
}

aval a_env_set(aval env, uint64_t i, aval v) {
    AObj *o = obj_of(env);
    if (!o || o->type != T_ENV) panicf("environment expected");
    AEnv *e = (AEnv *)o;
    if (i >= e->h.cap) panicf("environment index out of range");
    e->slots[i] = v;
    return A_NONE;
}

aval a_env_get(aval env, uint64_t i) {
    AObj *o = obj_of(env);
    if (!o || o->type != T_ENV) panicf("environment expected");
    AEnv *e = (AEnv *)o;
    if (i >= e->h.cap) panicf("environment index out of range");
    return e->slots[i];
}

AResult *result_new(int ok, int is_option, aval value) {
    AResult *r = (AResult *)xmalloc(sizeof(AResult));
    r->h.type = T_RESULT;
    r->h.len = ok ? 1 : 0;
    r->h.cap = 0;
    r->ok = ok ? 1u : 0u;
    r->is_option = is_option ? 1u : 0u;
    r->value = value;
    return r;
}

aval a_some_new(aval value) {
    AResult *r = (AResult *)xmalloc(sizeof(AResult));
    r->h.type = T_RESULT;
    r->h.len = 1;
    r->h.cap = 0;
    r->ok = 1;
    r->is_option = 1;
    r->value = value;
    return mkobj(&r->h);
}

int is_result(aval v) {
    AObj *o = obj_of(v);
    return o && o->type == T_RESULT;
}

int a_is_option(aval v) {
    AObj *o = obj_of(v);
    return o && o->type == T_RESULT && ((AResult *)o)->is_option;
}

int result_ok(aval v) { return ((AResult *)obj_of(v))->ok != 0; }
static aval result_value(aval v) { return ((AResult *)obj_of(v))->value; }

aval a_call(aval fn, uint64_t n, aval *argv) {
    AObj *fo = obj_of(fn);
    if (fo && fo->type == T_PYOBJ) return a_py_call(fn, n, argv);
    AFnon *f = fnon_of(fn, "call target");
    if (f->flags & FNON_BUILTIN) {
        a_builtin_fn bf = (a_builtin_fn)(uintptr_t)f->entry;
        return bf(n, argv);
    }
    if (f->nparams != NPARAMS_UNKNOWN && n != f->nparams)
        panicf("expected %llu argument(s), got %llu",
               (unsigned long long)f->nparams, (unsigned long long)n);
    uint64_t total = f->nparams == NPARAMS_UNKNOWN ? n + 1 : f->nparams + 1;
    aval small[16];
    aval *av = small;
    if (total > 16) av = (aval *)xmalloc((size_t)total * sizeof(aval));
    av[0] = f->env;
    for (uint64_t i = 0; i < n; ++i) av[i + 1] = argv[i];
    for (uint64_t i = n + 1; i < total; ++i) av[i] = A_NONE;
    aval (*code)(aval *) = (aval (*)(aval *))(uintptr_t)f->entry;
    aval r = code(av);
    if (av != small) free(av);
    return r;
}

aval a_unwrap(aval v) {
    if (a_is_none(v)) panicf("Cannot unwrap None with '!'.");
    if (is_result(v)) {
        if (result_ok(v)) return result_value(v);
        panicf("called '!' on Err(...)");
    }
    return v;
}

aval a_try_result(aval v) {
    if (is_result(v)) {
        if (result_ok(v)) return result_value(v);
        return v; /* Err propagates as a result value */
    }
    return v;
}


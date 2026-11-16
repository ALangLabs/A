#include "a_runtime_internal.h"

/* ------------------------------------------------------------------ */
/* indexing / slicing / iteration                                      */
/* ------------------------------------------------------------------ */

int64_t normalize_index(int64_t idx, uint64_t len) {
    if (idx < 0) idx += (int64_t)len;
    if (idx < 0 || idx >= (int64_t)len) panicf("index out of range");
    return idx;
}

/* ------------------------------------------------------------------ */
/* T_PYBUF element access: raw reads/writes into the exported buffer.  */
/* No GIL is taken — the Py_buffer export keeps the exporter alive.    */
/* ------------------------------------------------------------------ */

aval pybuf_read(APyBuf *b, int64_t i) {
    const char *p = b->data + (size_t)i * (size_t)b->itemsize;
    switch (b->kind) {
        case APYBUF_U8: return mkint_full((unsigned char)p[0]);
        case APYBUF_I8: return mkint_full((signed char)p[0]);
        case APYBUF_BOOL: return bool_aval(p[0] != 0);
        case APYBUF_U16: { uint16_t x; memcpy(&x, p, 2); return mkint_full(x); }
        case APYBUF_I16: { int16_t x; memcpy(&x, p, 2); return mkint_full(x); }
        case APYBUF_U32: { uint32_t x; memcpy(&x, p, 4); return mkint_full(x); }
        case APYBUF_I32: { int32_t x; memcpy(&x, p, 4); return mkint_full(x); }
        case APYBUF_U64: {
            uint64_t x; memcpy(&x, p, 8);
            return x <= (uint64_t)INT64_MAX ? mkint_full((int64_t)x)
                                            : mkdouble((double)x);
        }
        case APYBUF_I64: { int64_t x; memcpy(&x, p, 8); return mkint_full(x); }
        case APYBUF_F32: { float f; memcpy(&f, p, 4); return mkdouble((double)f); }
        case APYBUF_F64: { double d; memcpy(&d, p, 8); return mkdouble(d); }
        default: panicf("internal: bad pybuffer element kind");
    }
    return A_NONE;
}

void pybuf_write(APyBuf *b, int64_t i, aval v) {
    if (b->readonly)
        panicf("pybuffer is read-only (the Python exporter did not grant write access)");
    char *p = b->data + (size_t)i * (size_t)b->itemsize;
    switch (b->kind) {
        case APYBUF_BOOL:
            p[0] = (char)(truthy(v) ? 1 : 0);
            return;
        case APYBUF_F32: {
            float f = (float)num_to_double(v);
            memcpy(p, &f, 4);
            return;
        }
        case APYBUF_F64: {
            double d = num_to_double(v);
            memcpy(p, &d, 8);
            return;
        }
        default: {
            int64_t x = expect_int64(v, "pybuffer element");
            switch (b->kind) {
                case APYBUF_U8: p[0] = (char)(uint8_t)x; return;
                case APYBUF_I8: p[0] = (char)(int8_t)x; return;
                case APYBUF_U16: { uint16_t t = (uint16_t)x; memcpy(p, &t, 2); return; }
                case APYBUF_I16: { int16_t t = (int16_t)x; memcpy(p, &t, 2); return; }
                case APYBUF_U32: { uint32_t t = (uint32_t)x; memcpy(p, &t, 4); return; }
                case APYBUF_I32: { int32_t t = (int32_t)x; memcpy(p, &t, 4); return; }
                case APYBUF_U64: { uint64_t t = (uint64_t)x; memcpy(p, &t, 8); return; }
                case APYBUF_I64: memcpy(p, &x, 8); return;
                default: panicf("internal: bad pybuffer element kind");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* NDArray (std.math) multi-dimensional indexing                        */
/*                                                                      */
/* NDArray is a class instance (T_STRUCT) with `data: [f64]` and        */
/* `shape: [i64]` fields. Bracket subscripts lower to this code:        */
/*   x[i]          -> a_getitem(obj, i)          (single int index)      */
/*   x[i, j, ...]  -> a_getitem(obj, (i, j, ...)) (tuple index)          */
/*   x[a..b, i]    -> tuple element is a 3-list [lo, hi, step] with      */
/*                    A_NONE for open bounds (slice spec)                */
/*   x[a..b]       -> a_slice(obj, lo, hi, step)  (axis-0 slice)         */
/* An all-integer full-rank spec returns the f64 element; anything else */
/* returns a new NDArray (a copy — slices do not alias the source).     */
/* ------------------------------------------------------------------ */

static int64_t slice_step(aval v);

typedef struct {
    int is_slice;
    int64_t idx;          /* integer component (raw, may be negative) */
    aval lo, hi, st;      /* slice component bounds (A_NONE = open)   */
} NdComp;

static aval nd_field(aval obj, const char *name, uint64_t len) {
    aval k = a_str_new(name, len);
    return a_dict_get(obj, k, A_NONE);
}

static int nd_is_ndarray(aval obj) {
    AObj *o = obj_of(obj);
    if (!o || o->type != T_STRUCT) return 0;
    aval nm = ((AStruct *)o)->name;
    if (kind_of(nm) != K_STR) return 0;
    uint64_t nl = 0;
    const char *np = str_data(nm, &nl);
    if (!np || nl != 7 || memcmp(np, "NDArray", 7) != 0) return 0;
    aval data = nd_field(obj, "data", 4);
    aval shape = nd_field(obj, "shape", 5);
    AObj *d = obj_of(data);
    AObj *s = obj_of(shape);
    return d && d->type == T_LIST && s && s->type == T_LIST;
}

/* Parse an index spec into per-axis components. `spec` is either a
   list/tuple of components or a single scalar index. Returns a malloc'd
   array; *out_n receives the component count. */
static NdComp *nd_parse_spec(aval spec, int64_t *out_n) {
    int64_t n;
    int single = 0;
    int k = kind_of(spec);
    if (k == K_LIST || k == K_TUPLE) {
        AArr *a = (AArr *)obj_of(spec);
        n = (int64_t)a->h.len;
    } else {
        n = 1;
        single = 1;
    }
    NdComp *comps = (NdComp *)xmalloc((size_t)(n ? n : 1) * sizeof(NdComp));
    for (int64_t i = 0; i < n; ++i) {
        aval e = single ? spec : arr_elem((AArr *)obj_of(spec), (uint64_t)i);
        comps[i].is_slice = 0;
        comps[i].idx = 0;
        comps[i].lo = comps[i].hi = comps[i].st = A_NONE;
        if (kind_of(e) == K_LIST) {
            AArr *sp = (AArr *)obj_of(e);
            if (sp->h.len != 3)
                panicf("unsupported NDArray index component (expected an "
                       "integer or a start..end..step slice)");
            comps[i].is_slice = 1;
            comps[i].lo = arr_elem(sp, 0);
            comps[i].hi = arr_elem(sp, 1);
            comps[i].st = arr_elem(sp, 2);
        } else {
            comps[i].idx = expect_int64(e, "NDArray index");
        }
    }
    *out_n = n;
    return comps;
}

static int64_t nd_shape_at(AArr *shape, int64_t a) {
    return expect_int64(arr_elem(shape, (uint64_t)a), "NDArray shape");
}

/* Resolve one axis to either a fixed index or a (start, step, count)
   range. `kind` is 0 for fixed, 1 for range. */
static void nd_resolve_axis(int64_t dim, const NdComp *c, int has_comp,
                            int *kind, int64_t *fixed, int64_t *start,
                            int64_t *step, int64_t *count) {
    if (has_comp && !c->is_slice) {
        *kind = 0;
        *fixed = normalize_index(c->idx, (uint64_t)dim);
        return;
    }
    *kind = 1;
    aval lo = A_NONE, hi = A_NONE, stv = A_NONE;
    if (has_comp && c->is_slice) {
        lo = c->lo;
        hi = c->hi;
        stv = c->st;
    }
    int64_t st = a_is_none(stv) ? 1 : slice_step(stv);
    int64_t s, e;
    if (st > 0) {
        s = slice_bound(lo, dim, 0, dim, 0);
        e = slice_bound(hi, dim, 0, dim, dim);
        *count = e > s ? (e - s + st - 1) / st : 0;
    } else {
        s = slice_bound(lo, dim, -1, dim - 1, dim - 1);
        e = slice_bound(hi, dim, -1, dim - 1, -1);
        *count = s > e ? (s - e - 1) / (-st) + 1 : 0;
    }
    *start = s;
    *step = st;
}

static aval nd_getitem(aval obj, aval spec) {
    aval data = nd_field(obj, "data", 4);
    aval shape = nd_field(obj, "shape", 5);
    AArr *darr = (AArr *)obj_of(data);
    AArr *shp = (AArr *)obj_of(shape);
    int64_t ndim = (int64_t)shp->h.len;

    int64_t ncomp = 0;
    NdComp *comps = nd_parse_spec(spec, &ncomp);
    if (ncomp > ndim)
        panicf("too many indices for %lld-dimensional NDArray",
               (long long)ndim);

    int64_t *stride = (int64_t *)xmalloc((size_t)(ndim ? ndim : 1) *
                                         sizeof(int64_t));
    int64_t *dims = (int64_t *)xmalloc((size_t)(ndim ? ndim : 1) *
                                       sizeof(int64_t));
    for (int64_t a = ndim - 1, st = 1; a >= 0; --a) {
        dims[a] = nd_shape_at(shp, a);
        stride[a] = st;
        st *= dims[a];
    }

    /* All-integer full-rank index -> scalar element. */
    int scalar = (ncomp == ndim);
    for (int64_t a = 0; a < ndim && scalar; ++a) {
        if (a < ncomp && comps[a].is_slice) scalar = 0;
    }
    if (scalar) {
        int64_t off = 0;
        for (int64_t a = 0; a < ndim; ++a)
            off += normalize_index(comps[a].idx, (uint64_t)dims[a]) * stride[a];
        free(comps);
        free(stride);
        free(dims);
        return arr_elem(darr, (uint64_t)off);
    }

    /* General case: build the result shape and copy elements.
       Per input axis: kind 0 = fixed index (axis dropped), kind 1 = range
       (axis kept with `count` positions). Axes past ncomp are kept whole. */
    int *kind = (int *)xmalloc((size_t)(ndim ? ndim : 1) * sizeof(int));
    int64_t *fixed = (int64_t *)xmalloc((size_t)(ndim ? ndim : 1) *
                                        sizeof(int64_t));
    int64_t *rstart = (int64_t *)xmalloc((size_t)(ndim ? ndim : 1) *
                                         sizeof(int64_t));
    int64_t *rstep = (int64_t *)xmalloc((size_t)(ndim ? ndim : 1) *
                                        sizeof(int64_t));
    int64_t *rcount = (int64_t *)xmalloc((size_t)(ndim ? ndim : 1) *
                                         sizeof(int64_t));
    int64_t outndim = 0;
    int64_t base = 0;
    for (int64_t a = 0; a < ndim; ++a) {
        nd_resolve_axis(dims[a], a < ncomp ? &comps[a] : NULL, a < ncomp,
                        &kind[a], &fixed[a], &rstart[a], &rstep[a],
                        &rcount[a]);
        if (kind[a] == 0) {
            base += fixed[a] * stride[a];
        } else {
            ++outndim;
        }
    }

    /* Map output axes (C-order) back to input axes. */
    int64_t *oaxis = (int64_t *)xmalloc((size_t)(outndim ? outndim : 1) *
                                        sizeof(int64_t));
    int64_t *odim = (int64_t *)xmalloc((size_t)(outndim ? outndim : 1) *
                                       sizeof(int64_t));
    {
        int64_t j = 0;
        for (int64_t a = 0; a < ndim; ++a) {
            if (kind[a] == 1) {
                oaxis[j] = a;
                odim[j] = rcount[a];
                ++j;
            }
        }
    }

    int64_t total = 1;
    for (int64_t j = 0; j < outndim; ++j) total *= odim[j];

    AVec outv = { NULL, 0, 0 };
    int64_t *coord = (int64_t *)xmalloc((size_t)(outndim ? outndim : 1) *
                                        sizeof(int64_t));
    for (int64_t p = 0; p < total; ++p) {
        int64_t rem = p;
        for (int64_t j = outndim - 1; j >= 0; --j) {
            coord[j] = odim[j] ? rem % odim[j] : 0;
            if (odim[j]) rem /= odim[j];
        }
        int64_t off = base;
        for (int64_t j = 0; j < outndim; ++j) {
            int64_t a = oaxis[j];
            off += (rstart[a] + coord[j] * rstep[a]) * stride[a];
        }
        avec_push(&outv, arr_elem(darr, (uint64_t)off));
    }

    /* Result shape: one entry per kept axis. */
    AVec shpv = { NULL, 0, 0 };
    for (int64_t j = 0; j < outndim; ++j)
        avec_push(&shpv, mkint_full(odim[j]));

    aval outData = list_from_vec(outv.p, (uint64_t)outv.len, 0);
    aval outShape = list_from_vec(shpv.p, (uint64_t)shpv.len, 0);
    free(outv.p);
    free(shpv.p);
    free(coord);
    free(oaxis);
    free(odim);
    free(rcount);
    free(rstep);
    free(rstart);
    free(fixed);
    free(kind);
    free(comps);
    free(stride);
    free(dims);

    AStruct *src = (AStruct *)obj_of(obj);
    aval nobj = a_struct_new(src->name, src->methods, src->methodFns);
    a_dict_set(nobj, a_str_new("data", 4), outData);
    a_dict_set(nobj, a_str_new("shape", 5), outShape);
    a_promote(nobj);
    return nobj;
}

static aval nd_setitem(aval obj, aval spec, aval val) {
    aval data = nd_field(obj, "data", 4);
    aval shape = nd_field(obj, "shape", 5);
    AArr *darr = (AArr *)obj_of(data);
    AArr *shp = (AArr *)obj_of(shape);
    int64_t ndim = (int64_t)shp->h.len;

    int64_t ncomp = 0;
    NdComp *comps = nd_parse_spec(spec, &ncomp);
    if (ncomp != ndim)
        panicf("NDArray assignment needs one integer index per axis "
               "(%lld given, %lld dimensions)",
               (long long)ncomp, (long long)ndim);
    for (int64_t a = 0; a < ndim; ++a) {
        if (comps[a].is_slice)
            panicf("NDArray slice assignment is not supported");
    }
    int64_t st = 1;
    int64_t off = 0;
    for (int64_t a = ndim - 1; a >= 0; --a) {
        int64_t dim = nd_shape_at(shp, a);
        int64_t i = normalize_index(comps[a].idx, (uint64_t)dim);
        off += i * st;
        st *= dim;
    }
    /* Store a canonical f64 element, mirroring list assignment barriers. */
    aval dv = mkdouble(num_to_double(val));
    if (arr_packed(darr)) {
        if (is_int_val(dv)) {
            darr->iitems[off] = int_val_full(dv);
        } else {
            darr->flags |= ARR_F_NOMIX;
            arr_ensure_aval(darr);
            a_region_store(darr, dv);
            darr->items[off] = dv;
        }
    } else {
        if ((darr->flags & ARR_F_F64) && !a_is_double(dv)) arr_f64_unmark(darr);
        a_region_store(darr, dv);
        darr->items[off] = dv;
    }
    free(comps);
    return val;
}

aval a_getitem(aval obj, aval idx) {
    AObj *oo = obj_of(obj);
    if (oo && oo->type == T_PYOBJ) return a_py_getitem(obj, idx);
    if (oo && oo->type == T_PYBUF) {
        APyBuf *b = (APyBuf *)oo;
        int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)b->count);
        return pybuf_read(b, i);
    }
    if (oo && oo->type == T_STRUCT && nd_is_ndarray(obj))
        return nd_getitem(obj, idx);
    switch (kind_of(obj)) {
        case K_STR: {
            AStr *s = astr_flat(obj);
            int64_t total = (int64_t)u8_count(s->data, s->h.len);
            int64_t i = normalize_index(expect_int64(idx, "string index"), (uint64_t)total);
            uint64_t boff = cp_to_byte(s->data, s->h.len, i);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            return a_str_new(s->data + boff, (uint64_t)l);
        }
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)vw->len);
            int64_t base;
            AArr *root = view_root(obj, &base);
            return root->items[(size_t)(base + i)];
        }
        case K_BYTES: {
            ABytes *b = (ABytes *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), b->h.len);
            return mkint_full((unsigned char)b->data[i]);
        }
        case K_CHAR: {
            panicf("object of type 'char' is not subscriptable");
            return A_NONE;
        }
        case K_LIST:
        case K_TUPLE: {
            AArr *a = (AArr *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), a->h.len);
            return arr_elem(a, (uint64_t)i);
        }
        case K_DICT: {
            ADict *d = (ADict *)obj_of(obj);
            int64_t i = dict_find(d, idx);
            if (i < 0) {
                uint64_t kl = 0;
                const char *kp = kind_of(idx) == K_STR ? str_data(idx, &kl) : NULL;
                if (kp)
                    panicf("Key %s is not in the map.", (char *)memcpy((char[256]){0}, kp, kl < 255 ? kl : 255));
                panicf("key not found");
            }
            return d->pairs[i].v;
        }
        default:
            panicf("object of type '%s' is not subscriptable", type_name_cstr(obj));
            return A_NONE;
    }
}

aval a_setitem(aval obj, aval idx, aval val) {
    AObj *so = obj_of(obj);
    if (so && so->type == T_PYOBJ) return a_py_setitem(obj, idx, val);
    if (so && so->type == T_STRUCT && nd_is_ndarray(obj))
        return nd_setitem(obj, idx, val);
    if (so && so->type == T_PYBUF) {
        APyBuf *b = (APyBuf *)so;
        int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)b->count);
        pybuf_write(b, i, val);
        return val;
    }
    switch (kind_of(obj)) {
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)vw->len);
            int64_t base;
            AArr *root = view_root(obj, &base);
            a_region_store(root, val);   /* write escapes into root's items */
            root->items[(size_t)(base + i)] = val;
            return val;
        }
        case K_LIST: {
            AArr *a = (AArr *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), a->h.len);
            if (arr_packed(a)) {
                if (is_int_val(val)) {
                    a->iitems[i] = int_val_full(val);
                    return val;
                }
                a->flags |= ARR_F_NOMIX;
                arr_ensure_aval(a);
            }
            if ((a->flags & ARR_F_F64) && !a_is_double(val)) arr_f64_unmark(a);
            a_region_store(a, val);   /* region write barrier */
            a->items[i] = val;
            return val;
        }
        case K_DICT: {
            dict_put((ADict *)obj_of(obj), idx, val);
            return val;
        }
        case K_BYTES: {
            ABytes *b = (ABytes *)obj_of(obj);
            int64_t i = normalize_index(expect_int64(idx, "index"), b->h.len);
            int64_t v = expect_int64(val, "a byte value");
            if (v < 0 || v > 255)
                panicf("byte value must be in 0..255, got %lld", (long long)v);
            b->data[i] = (char)v;
            return val;
        }
        default:
            panicf("object of type '%s' does not support item assignment",
                   type_name_cstr(obj));
            return A_NONE;
    }
}

static aval view_new(aval parent, int64_t off, int64_t len, int is_tuple) {
    AView *vw = (AView *)xmalloc(sizeof(AView));
    vw->h.type = T_VIEW;
    vw->h.len = (uint32_t)len;
    vw->h.cap = 0;
    /* An empty view can never alias: every element access is bounds-checked
       against len==0 and every mutator either panics or is a no-op, so the
       parent reference is unobservable. Dropping it keeps e.g. xs[a..a] of a
       huge list from pinning the whole backing store. view_root() returns
       NULL for such views; all callers either bounds-check len first or
       guard the NULL (see method_view/value_compare). */
    vw->parent = len ? parent : A_NONE;
    vw->off = off;
    vw->len = len;
    vw->is_tuple = is_tuple;
    /* The view aliases the parent's element buffer: pin its capacity so
       arr_shrink never reallocates it out from under the view's offsets. */
    if (len) arr_note_view(parent);
    return mkobj(&vw->h);
}
/* Resolve a view (or direct list) to root AArr + absolute offset.
   Packed-int roots are normalized to aval storage here: every caller
   reads or writes root->items, so conversion is the single choke point
   that keeps views semantics identical. */
AArr *view_root(aval v, int64_t *off) {
    AObj *o = obj_of(v);
    if (o && o->type == T_VIEW) {
        AView *vw = (AView *)o;
        int64_t base = 0;
        AArr *root = view_root(vw->parent, &base);
        *off = base + vw->off;
        return root;
    }
    *off = 0;
    AArr *r = (AArr *)o;
    if (r) arr_ensure_aval(r);
    return r;
}
static int64_t slice_step(aval v) {
    int64_t s = expect_int64(v, "slice step");
    if (s == 0) panicf("The slice step must be positive, found 0.");
    return s;
}

int64_t slice_bound(aval v, int64_t n, int64_t lower, int64_t upper,
                           int64_t dflt) {
    if (a_is_none(v)) return dflt;
    int64_t i = expect_int64(v, "slice index");
    if (i < 0) i += n;
    if (i < lower) i = lower;
    if (i > upper) i = upper;
    return i;
}

aval a_slice(aval obj, aval lo, aval hi, aval step) {
    AObj *so = obj_of(obj);
    if (so && so->type == T_STRUCT && nd_is_ndarray(obj)) {
        /* x[a..b] on an NDArray slices axis 0; route through the NDArray
           indexer with a single slice spec [lo, hi, step]. */
        aval inner[3] = { lo, hi, step };
        aval specList = a_list_new(3, inner);
        aval spec = a_list_new(1, &specList);
        return nd_getitem(obj, spec);
    }
    int k = kind_of(obj);
    if (k != K_STR && k != K_LIST && k != K_TUPLE && k != K_VIEW && k != K_BYTES)
        panicf("object of type '%s' is not sliceable", type_name_cstr(obj));
    AObj *o = obj_of(obj);
    uint64_t len;
    if (o->type == T_BYTES) len = ((ABytes *)o)->h.len;
    else if (o->type == T_STR || o->type == T_ROPE)
        len = a_str_cpcount(obj);
    else if (o->type == T_VIEW)
        len = (uint64_t)((AView *)o)->len;
    else
        len = ((AArr *)o)->h.len;
    int64_t n = (int64_t)len;
    int64_t st = a_is_none(step) ? 1 : slice_step(step);
    int64_t start, stop;
    if (st > 0) {
        start = slice_bound(lo, n, 0, n, 0);
        stop = slice_bound(hi, n, 0, n, n);
    } else {
        start = slice_bound(lo, n, -1, n - 1, n - 1);
        stop = slice_bound(hi, n, -1, n - 1, -1);
    }
    AVec v = { NULL, 0, 0 };
    if (o->type == T_BYTES) {
        /* Byte slice: a fresh Bytes (slicing is rare next to recv/decode, so
           copying keeps the object model simple and the payload independent). */
        ABytes *b = (ABytes *)o;
        int64_t cnt = st == 1 ? (stop - start > 0 ? stop - start : 0)
                              : (st > 0 ? (stop - start + st - 1) / st : (start - stop - st - 1) / -st);
        if (cnt < 0) cnt = 0;
        aval out = a_bytes_new((uint64_t)cnt);
        ABytes *dst = a_bytes_obj(out);
        int64_t w = 0;
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            dst->data[w++] = b->data[(uint64_t)i];
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        dst->h.len = (uint32_t)w;
        dst->data[w] = 0;
        return out;
    }
    if (o->type == T_STR || o->type == T_ROPE) {
        AStr *s = astr_flat(obj);
        Buf b;
        buf_init(&b);
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            uint64_t boff = cp_to_byte(s->data, s->h.len, i);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            if (boff < s->h.len) buf_addn(&b, s->data + boff, (size_t)l);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        return buf_take(&b);
    }
    /* Lists/tuples/views: contiguous step (1 or -1? only 1) returns a view
       that aliases the parent; stepped copies return a fresh list. */
    if (st == 1 && o->type != T_STR) {
        int64_t cnt = stop - start;
        if (cnt < 0) cnt = 0;
        if (o->type == T_VIEW) {
            AView *vw = (AView *)o;
            return view_new(vw->parent, vw->off + start, cnt, vw->is_tuple);
        }
        (void)v;
        return view_new(obj, start, cnt, o->type == T_TUPLE ? 1 : 0);
    }
    if (o->type == T_VIEW) {
        AView *vw = (AView *)o;
        int64_t base;
        AArr *root = view_root(obj, &base);
        (void)vw;
        for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
            avec_push(&v, root->items[(size_t)(base + i)]);
            if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
            else { if (i < INT64_MIN - st) break; i += st; }
        }
        aval r = list_from_vec(v.p, v.len, 0);
        free(v.p);
        arr_try_pack((AArr *)obj_of(r));
        return r;
    }
    AArr *a = (AArr *)o;
    /* arr_elem reads packed int64 storage too, so a stepped slice of a
       packed-int list no longer forces the whole parent to aval storage. */
    for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
        avec_push(&v, arr_elem(a, (uint64_t)i));
        if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
        else { if (i < INT64_MIN - st) break; i += st; }
    }
    aval r = list_from_vec(v.p, v.len, o->type == T_TUPLE);
    free(v.p);
    arr_try_pack((AArr *)obj_of(r));
    return r;
}

/* Iterator kind tags stored in AIter.has_lookahead for non-Python
   iterators (the field is only a lookahead flag when py_mode is set,
   and py_mode iterators return before the tag is ever consulted).
   Tagging at creation lets a_iter_next dispatch on one switch instead
   of re-deriving the source kind from scratch on every element. */
enum {
    ITAG_ARR = 1,   /* T_LIST / T_TUPLE / T_SET (AArr storage) */
    ITAG_DICT,      /* T_DICT / T_STRUCT (ADict pairs -> keys) */
    ITAG_VIEW,      /* T_VIEW */
    ITAG_STR,       /* T_STR / T_CHAR */
    ITAG_BYTES,     /* T_BYTES */
    ITAG_PYBUF,     /* T_PYBUF */
    ITAG_RANGE      /* K_RANGE */
};

aval a_iter_new(aval v) {
    AIter *it = (AIter *)xmalloc(sizeof(AIter));
    it->h.type = T_ITER;
    it->h.len = 0;
    it->h.cap = 0;
    it->src = v;
    it->idx = 0;
    it->cur = it->end = it->step = 0;
    it->py_mode = 0;
    it->has_lookahead = 0;
    it->lookahead = A_NONE;
    switch (kind_of(v)) {
        case K_VIEW:
            it->has_lookahead = ITAG_VIEW;
            return mkobj(&it->h);
        case K_LIST:
        case K_TUPLE:
        case K_SET:
            it->has_lookahead = ITAG_ARR;
            return mkobj(&it->h);
        case K_STR:
            it->has_lookahead = ITAG_STR;
            return mkobj(&it->h);
        case K_DICT:
            it->has_lookahead = ITAG_DICT;
            return mkobj(&it->h);
        case K_BYTES:
            it->has_lookahead = ITAG_BYTES;
            return mkobj(&it->h);
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            it->cur = r->lo;
            it->end = r->hi;
            it->step = r->step;
            it->has_lookahead = ITAG_RANGE;
            return mkobj(&it->h);
        }
        default: {
            AObj *vo = obj_of(v);
            if (vo && vo->type == T_PYBUF) {
                /* Index-based iteration over the raw buffer elements;
                   handled by the generic path in a_iter_next. */
                it->has_lookahead = ITAG_PYBUF;
                return mkobj(&it->h);
            }
            if (vo && vo->type == T_PYOBJ) {
                /* Python iterator: src becomes a T_PYOBJ wrapping the
                   PyObject* iterator; items are fetched lazily through
                   a_py_iter_next with a one-item lookahead. */
                it->src = a_py_iter_begin(v);
                it->py_mode = 1;
                return mkobj(&it->h);
            }
            free(it);
            panicf("object of type '%s' is not iterable", type_name_cstr(v));
            return A_NONE;
        }
    }
}

aval a_iter_next(aval it, aval *out) {
    AObj *o = obj_of(it);
    if (!o || o->type != T_ITER) panicf("argument is not an iterator");
    AIter *iter = (AIter *)o;
    /* src is reset to A_NONE once the iterator is exhausted (see the
       return-0 paths below) so a long-lived iterator object stops pinning
       the whole collection. A_NONE can never be a live src: a_iter_new
       rejects non-iterables before storing it. */
    if (a_is_none(iter->src)) return (aval)0;
    if (iter->py_mode) {
        if (!iter->has_lookahead) {
            if (!a_py_iter_next(iter->src, &iter->lookahead)) {
                iter->src = A_NONE;
                return (aval)0;
            }
            a_region_store(iter, iter->lookahead);  /* iter may have escaped */
            iter->has_lookahead = 1;
        }
        *out = iter->lookahead;
        iter->lookahead = A_NONE;
        iter->has_lookahead = 0;
        return (aval)1;
    }
    aval src = iter->src;
    AObj *so = obj_of(src);
    switch (iter->has_lookahead) {
        case ITAG_RANGE: {
            int64_t c = iter->cur, e = iter->end, s = iter->step;
            int more = s > 0 ? c < e : c > e;
            if (!more) { iter->src = A_NONE; return (aval)0; }
            *out = mkint_full(c);
            if (s > 0) {
                if (c > INT64_MAX - s) iter->cur = e;
                else iter->cur = c + s;
            } else {
                if (c < INT64_MIN - s) iter->cur = e;
                else iter->cur = c + s;
            }
            return (aval)1;
        }
        case ITAG_DICT: {
            ADict *d = (ADict *)so;
            if (iter->idx >= d->h.len) { iter->src = A_NONE; return (aval)0; }
            *out = d->pairs[iter->idx++].k;
            return (aval)1;
        }
        case ITAG_ARR: {
            AArr *a = (AArr *)so;
            if (iter->idx >= a->h.len) { iter->src = A_NONE; return (aval)0; }
            uint64_t i = iter->idx++;
            *out = arr_packed(a) ? mkint_full(a->iitems[i]) : a->items[i];
            return (aval)1;
        }
        case ITAG_VIEW: {
            AView *vw = (AView *)so;
            if (iter->idx >= (uint64_t)vw->len) { iter->src = A_NONE; return (aval)0; }
            /* The view's parent chain is fixed at creation, so resolve
               root+offset once and cache them: `cur` holds the absolute
               base offset, `end` the root AArr* (as intptr), and `step`
               is the resolved flag (unused for view iterators). arr_elem
               is used per element because the root can re-pack to
               int64 storage between next() calls. */
            AArr *root;
            int64_t base;
            if (iter->step) {
                root = (AArr *)(intptr_t)iter->end;
                base = iter->cur;
            } else {
                root = view_root(src, &base);
                iter->cur = base;
                iter->end = (int64_t)(intptr_t)root;
                iter->step = 1;
            }
            uint64_t i = iter->idx++;
            *out = root ? arr_elem(root, (uint64_t)(base + (int64_t)i)) : A_NONE;
            return (aval)1;
        }
        case ITAG_STR: {
            AStr *s = astr_flat(src);
            /* Code-point count is cached on the string (same scheme as
               a_len): without it every next() ran a full O(n) u8_count
               scan, making string iteration O(n^2). */
            uint32_t cp = astr_cp_cached(s);
            uint64_t len;
            if (cp) len = (uint64_t)cp - 1u;
            else {
                len = u8_count(s->data, s->h.len);
                astr_cp_cache(s, (uint32_t)len);
            }
            if (iter->idx >= len) { iter->src = A_NONE; return (aval)0; }
            /* `cur` is the byte offset of code point `idx`: it advances by
               exactly the yielded character's byte length each call, so
               string iteration is O(n) total instead of a cp_to_byte
               rescan per element. The clamping rule matches cp_to_byte's
               (u8_len truncated to 1 past the end). A consistency guard
               falls back to cp_to_byte if the cursor ever disagrees. */
            int64_t boff = iter->cur;
            if ((uint64_t)boff >= s->h.len)
                boff = (int64_t)cp_to_byte(s->data, s->h.len, (int64_t)iter->idx);
            int l = u8_len((unsigned char)s->data[boff]);
            if (boff + (uint64_t)l > s->h.len) l = 1;
            *out = a_char_new(s->data + boff, (uint64_t)l);
            iter->cur = boff + l;
            iter->idx++;
            return (aval)1;
        }
        case ITAG_BYTES: {
            ABytes *b = (ABytes *)so;
            if (iter->idx >= b->h.len) { iter->src = A_NONE; return (aval)0; }
            *out = mkint_full((unsigned char)b->data[iter->idx++]);
            return (aval)1;
        }
        case ITAG_PYBUF: {
            APyBuf *b = (APyBuf *)so;
            if (iter->idx >= (uint64_t)b->count) { iter->src = A_NONE; return (aval)0; }
            *out = pybuf_read(b, (int64_t)iter->idx++);
            return (aval)1;
        }
        default:
            break;
    }
    /* Untagged iterator (cannot be produced by a_iter_new today; kept as
       a defensive fallback that re-derives the source kind per call). */
    if (kind_of(src) == K_RANGE) {
        int64_t c = iter->cur, e = iter->end, s = iter->step;
        int more = s > 0 ? c < e : c > e;
        if (!more) { iter->src = A_NONE; return (aval)0; }
        *out = mkint_full(c);
        if (s > 0) {
            if (c > INT64_MAX - s) iter->cur = e;
            else iter->cur = c + s;
        } else {
            if (c < INT64_MIN - s) iter->cur = e;
            else iter->cur = c + s;
        }
        return (aval)1;
    }
    uint64_t len;
    aval *items = NULL;
    int64_t *iitems = NULL;
    ADict *d = NULL;
    AView *vw_src = NULL;
    APyBuf *pb_src = NULL;
    int64_t vw_base = 0;
    if (is_dict_t(so->type)) {
        d = (ADict *)so;
        len = d->h.len;
    } else if (so->type == T_PYBUF) {
        pb_src = (APyBuf *)so;
        len = (uint64_t)pb_src->count;
    } else if (so->type == T_VIEW) {
        vw_src = (AView *)so;
        len = (uint64_t)vw_src->len;
    } else if (so->type == T_STR || so->type == T_CHAR || so->type == T_ROPE) {
        AStr *s = astr_flat(src);
        len = u8_count(s->data, s->h.len);
    } else if (so->type == T_BYTES) {
        len = ((ABytes *)so)->h.len;
    } else {
        AArr *a = (AArr *)so;
        len = a->h.len;
        if (arr_packed(a)) iitems = a->iitems;
        else items = a->items;
    }
    if (iter->idx >= len) { iter->src = A_NONE; return (aval)0; }
    uint64_t i = iter->idx++;
    if (d) *out = d->pairs[i].k;
    else if (pb_src) *out = pybuf_read(pb_src, (int64_t)i);
    else if (vw_src) {
        AArr *root = view_root(src, &vw_base);
        *out = root->items[(size_t)(vw_base + (int64_t)i)];
    }
    else if (so->type == T_BYTES) {
        *out = mkint_full((unsigned char)((ABytes *)so)->data[i]);
    }
    else if (so->type == T_STR || so->type == T_CHAR || so->type == T_ROPE) {
        AStr *s = astr_flat(src);
        uint64_t boff = cp_to_byte(s->data, s->h.len, (int64_t)i);
        int l = u8_len((unsigned char)s->data[boff]);
        if (boff + (uint64_t)l > s->h.len) l = 1;
        *out = a_char_new(s->data + boff, (uint64_t)l);
    }
    else if (iitems) *out = mkint_full(iitems[i]);
    else *out = items[i];
    return (aval)1;
}


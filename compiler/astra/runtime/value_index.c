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

aval a_getitem(aval obj, aval idx) {
    AObj *oo = obj_of(obj);
    if (oo && oo->type == T_PYOBJ) return a_py_getitem(obj, idx);
    if (oo && oo->type == T_PYBUF) {
        APyBuf *b = (APyBuf *)oo;
        int64_t i = normalize_index(expect_int64(idx, "index"), (uint64_t)b->count);
        return pybuf_read(b, i);
    }
    switch (kind_of(obj)) {
        case K_STR: {
            AStr *s = (AStr *)obj_of(obj);
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
            if ((a->flags & ARR_F_F64) && !a_is_double(val)) a->flags &= ~ARR_F_F64;
            a->items[i] = val;
            return val;
        }
        case K_DICT: {
            dict_put((ADict *)obj_of(obj), idx, val);
            return val;
        }
        default:
            panicf("object of type '%s' does not support item assignment",
                   type_name_cstr(obj));
            return A_NONE;
    }
}

/* Bulk fill for the `for i in lo..hi[..step]: xs[i] = v` idiom emitted by
 * the codegen fill-loop fast path. Applies a_setitem's K_LIST/K_VIEW
 * element stores for the whole index range: normalize_index panics
 * "index out of range" at the first bad index, exactly like the
 * interpreted loop, and len/root are resolved once up front (a plain
 * item store cannot resize the container). The index advances with
 * unsigned wrap to mirror the int64 slot arithmetic of the counted
 * loop. Returns val (the loop discards it). */
aval a_list_fill_range(aval obj, int64_t lo, int64_t hi, int64_t step,
                       aval val) {
    AArr *root;
    int64_t base = 0;
    uint64_t len;
    switch (kind_of(obj)) {
        case K_VIEW: {
            AView *vw = (AView *)obj_of(obj);
            len = (uint64_t)vw->len;
            root = view_root(obj, &base);
            break;
        }
        case K_LIST: {
            AArr *a = (AArr *)obj_of(obj);
            len = a->h.len;
            root = a;
            break;
        }
        default:
            panicf("object of type '%s' does not support item assignment",
                   type_name_cstr(obj));
            return A_NONE;
    }
    /* Packed-int storage: an all-int fill stays packed; a non-int value
       converts the root to aval storage first (mirrors a_setitem). */
    int pok = 0;
    int64_t pv = 0;
    if (arr_packed(root)) {
        if (is_int_val(val)) {
            pok = 1;
            pv = int_val_full(val);
        } else {
            root->flags |= ARR_F_NOMIX;
            arr_ensure_aval(root);
        }
    }
    int64_t k = lo;
    if (step >= 0) {
        while (k < hi) {
            int64_t i = normalize_index(k, len);
            if (pok) root->iitems[(size_t)(base + i)] = pv;
            else {
                if ((root->flags & ARR_F_F64) && !a_is_double(val)) root->flags &= ~ARR_F_F64;
                root->items[(size_t)(base + i)] = val;
            }
            k = (int64_t)((uint64_t)k + (uint64_t)step);
        }
    } else {
        while (k > hi) {
            int64_t i = normalize_index(k, len);
            if (pok) root->iitems[(size_t)(base + i)] = pv;
            else {
                if ((root->flags & ARR_F_F64) && !a_is_double(val)) root->flags &= ~ARR_F_F64;
                root->items[(size_t)(base + i)] = val;
            }
            k = (int64_t)((uint64_t)k + (uint64_t)step);
        }
    }
    return val;
}

static aval view_new(aval parent, int64_t off, int64_t len, int is_tuple) {
    AView *vw = (AView *)xmalloc(sizeof(AView));
    vw->h.type = T_VIEW;
    vw->h.len = (uint32_t)len;
    vw->h.cap = 0;
    vw->parent = parent;
    vw->off = off;
    vw->len = len;
    vw->is_tuple = is_tuple;
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
    int k = kind_of(obj);
    if (k != K_STR && k != K_LIST && k != K_TUPLE && k != K_VIEW)
        panicf("object of type '%s' is not sliceable", type_name_cstr(obj));
    AObj *o = obj_of(obj);
    uint64_t len;
    if (o->type == T_STR)
        len = u8_count(((AStr *)o)->data, ((AStr *)o)->h.len);
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
    if (o->type == T_STR) {
        AStr *s = (AStr *)o;
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
    arr_ensure_aval(a);
    for (int64_t i = start; st > 0 ? i < stop : i > stop;) {
        avec_push(&v, a->items[i]);
        if (st > 0) { if (i > INT64_MAX - st) break; i += st; }
        else { if (i < INT64_MIN - st) break; i += st; }
    }
    aval r = list_from_vec(v.p, v.len, o->type == T_TUPLE);
    free(v.p);
    arr_try_pack((AArr *)obj_of(r));
    return r;
}

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
        case K_LIST:
        case K_TUPLE:
        case K_SET:
        case K_STR:
        case K_DICT:
            return mkobj(&it->h);
        case K_RANGE: {
            ARange *r = (ARange *)obj_of(v);
            it->cur = r->lo;
            it->end = r->hi;
            it->step = r->step;
            return mkobj(&it->h);
        }
        default: {
            AObj *vo = obj_of(v);
            if (vo && vo->type == T_PYBUF) {
                /* Index-based iteration over the raw buffer elements;
                   handled by the generic path in a_iter_next. */
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
    if (iter->py_mode) {
        if (!iter->has_lookahead) {
            if (!a_py_iter_next(iter->src, &iter->lookahead)) return (aval)0;
            iter->has_lookahead = 1;
        }
        *out = iter->lookahead;
        iter->lookahead = A_NONE;
        iter->has_lookahead = 0;
        return (aval)1;
    }
    aval src = iter->src;
    if (kind_of(src) == K_RANGE) {
        int64_t c = iter->cur, e = iter->end, s = iter->step;
        int more = s > 0 ? c < e : c > e;
        if (!more) return (aval)0;
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
    AObj *so = obj_of(src);
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
    } else if (so->type == T_STR || so->type == T_CHAR) {
        len = u8_count(((AStr *)so)->data, ((AStr *)so)->h.len);
    } else {
        AArr *a = (AArr *)so;
        len = a->h.len;
        if (arr_packed(a)) iitems = a->iitems;
        else items = a->items;
    }
    if (iter->idx >= len) return (aval)0;
    uint64_t i = iter->idx++;
    if (d) *out = d->pairs[i].k;
    else if (pb_src) *out = pybuf_read(pb_src, (int64_t)i);
    else if (vw_src) {
        AArr *root = view_root(src, &vw_base);
        *out = root->items[(size_t)(vw_base + (int64_t)i)];
    }
    else if (so->type == T_STR || so->type == T_CHAR) {
        AStr *s = (AStr *)so;
        uint64_t boff = cp_to_byte(s->data, s->h.len, (int64_t)i);
        int l = u8_len((unsigned char)s->data[boff]);
        if (boff + (uint64_t)l > s->h.len) l = 1;
        *out = a_char_new(s->data + boff, (uint64_t)l);
    }
    else if (iitems) *out = mkint_full(iitems[i]);
    else *out = items[i];
    return (aval)1;
}


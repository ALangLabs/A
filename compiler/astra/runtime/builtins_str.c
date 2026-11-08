#include "a_runtime_internal.h"

static int join_scalar_kind(int k) {
    return k == K_NONE || k == K_BOOL || k == K_INT || k == K_FLOAT ||
           k == K_STR || k == K_CHAR || k == K_RANGE || k == K_FNON;
}

static int arr_join_scalar_safe(const AArr *a) {
    if (arr_packed(a)) return 1;
    for (uint64_t i = 0; i < a->h.len; ++i)
        if (!join_scalar_kind(kind_of(a->items[i]))) return 0;
    return 1;
}

static void join_buf_array(Buf *b, const char *sep, uint64_t sn,
                           const AArr *a) {
    if (arr_packed(a)) {
        for (uint64_t i = 0; i < a->h.len; ++i) {
            if (i) buf_addn(b, sep, (size_t)sn);
            buf_addi64(b, a->iitems[i]);
        }
        return;
    }
    int all_str = a->h.len != 0;
    uint64_t total = (a->h.len ? (a->h.len - 1) * sn : 0);
    for (uint64_t i = 0; i < a->h.len; ++i) {
        aval v = a->items[i];
        if (kind_of(v) != K_STR) { all_str = 0; break; }
        total += (uint64_t)((const AStr *)obj_of(v))->h.len;
    }
    if (all_str) {
        buf_reserve(b, total);
        char *out = b->p + b->len;
        for (uint64_t i = 0; i < a->h.len; ++i) {
            if (i) {
                memcpy(out, sep, (size_t)sn);
                out += sn;
            }
            const AStr *s = (const AStr *)obj_of(a->items[i]);
            memcpy(out, s->data, (size_t)s->h.len);
            out += s->h.len;
        }
        b->len = (size_t)(out - b->p);
        b->p[b->len] = '\0';
        return;
    }
    for (uint64_t i = 0; i < a->h.len; ++i) {
        if (i) buf_addn(b, sep, (size_t)sn);
        format_into(b, arr_elem(a, i), 0);
    }
}

aval bi_join(uint64_t n, aval *argv) {
    want_args("join", n, 2, 2);
    if (kind_of(argv[1]) != K_STR) panicf("join() separator must be str");
    uint64_t sn;
    const char *sep = str_data(argv[1], &sn);
    Buf b;
    buf_init(&b);
    AObj *src = obj_of(argv[0]);
    if (src && (src->type == T_LIST || src->type == T_TUPLE ||
                src->type == T_SET) &&
        arr_join_scalar_safe((const AArr *)src)) {
        const AArr *a = (const AArr *)src;
        if (!arr_packed(a) && a->h.len != 0) {
            uint64_t total = (a->h.len - 1) * sn;
            int all_str = 1;
            for (uint64_t i = 0; i < a->h.len; ++i) {
                aval v = a->items[i];
                AObj *o = a_is_ptr(v) ? (AObj *)(uintptr_t)A_PAYLOAD(v) : NULL;
                if (!o || o->type != T_STR) { all_str = 0; break; }
                total += (uint64_t)((const AStr *)o)->h.len;
            }
            if (all_str && total <= 0xFFFFFFFFull) {
                aval outv = a_str_new_uninit(total);
                AStr *out = (AStr *)obj_of(outv);
                char *p = out->data;
                for (uint64_t i = 0; i < a->h.len; ++i) {
                    if (i) { memcpy(p, sep, (size_t)sn); p += sn; }
                    const AStr *s = (const AStr *)(uintptr_t)A_PAYLOAD(a->items[i]);
                    memcpy(p, s->data, (size_t)s->h.len);
                    p += s->h.len;
                }
                return outv;
            }
        }
        join_buf_array(&b, sep, sn, a);
        return buf_take(&b);
    }
    aval tmp = collect_iterable(argv[0]);
    AArr *a = (AArr *)obj_of(tmp);
    join_buf_array(&b, sep, sn, a);
    return buf_take(&b);
}

aval a_join_len(aval values, aval sep) {
    AObj *so = a_is_ptr(sep) ? (AObj *)(uintptr_t)A_PAYLOAD(sep) : NULL;
    AObj *src = a_is_ptr(values) ? (AObj *)(uintptr_t)A_PAYLOAD(values) : NULL;
    if (so && so->type == T_STR && src &&
        (src->type == T_LIST || src->type == T_TUPLE || src->type == T_SET) &&
        !arr_packed((const AArr *)src)) {
        const AArr *a = (const AArr *)src;
        uint64_t n = a->h.len;
        const AStr *ss = (const AStr *)so;
        uint64_t sep_cp = u8_count(ss->data, ss->h.len);
        uint64_t total = n ? (n - 1) * sep_cp : 0;
        int all_str = 1;
        for (uint64_t i = 0; i < n; ++i) {
            aval v = a->items[i];
            AObj *o = a_is_ptr(v) ? (AObj *)(uintptr_t)A_PAYLOAD(v) : NULL;
            if (!o || o->type != T_STR) { all_str = 0; break; }
            const AStr *s = (const AStr *)o;
            total += u8_count(s->data, s->h.len);
        }
        if (all_str) return mkint_full((int64_t)total);
    }
    aval argv[2] = { values, sep };
    return a_len(bi_join(2, argv));
}


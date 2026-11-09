/* region.c — deterministic young-generation regions for the A runtime.
 *
 * Complement to the conservative mark-sweep GC in gc.c. Every generated
 * function prologue calls a_frame_push() and every return path calls
 * a_frame_pop(); those hooks drive a per-thread stack of "young" regions:
 *
 *   - mkobj() reports each new heap object to the innermost region
 *     (a_region_note_obj), which records its pointer and stamps the GC
 *     registry block with the region's stack depth.
 *   - Escape analysis is a write barrier, not a trace: whenever an aval
 *     is stored into a heap container (dict/struct pair, list/set item,
 *     env slot, iterator lookahead) or into storage that outlives the
 *     frame (globals, file-static tables, closure envs, return values),
 *     a_region_store()/a_promote() flags the child object GC_F_OLD in
 *     the GC registry. Promotion is TRANSITIVE: a_promote walks the
 *     object's own aval fields so an escaped container keeps everything
 *     it references.
 *   - a_region_leave() (from a_frame_pop) bulk-frees every object born
 *     in the leaving region that was never promoted, releasing interior
 *     buffers and delisting from the GC registry via a_gc_release_obj.
 *     Promoted objects stay registered and remain GC-managed, so the
 *     mark-sweep collector is still the safety net for old-generation
 *     garbage, cycles, and anything allocated outside a region (C
 *     temporaries, pool workers, pre-init objects, native-emit targets
 *     that emit no frame hooks).
 *
 * Escape rule for a_region_store(container, child): the child must
 * outlive the container's region. Regions are a stack, so with both
 * regions still active this reduces to a depth compare: promote the
 * child iff the container is OLD/untracked (region depth 0) or sits in
 * a strictly shallower region. Same-region stores need no barrier —
 * when the container later escapes, transitive promotion reaches the
 * child anyway; when it does not, both die together at region leave.
 *
 * The "<module>" frame (program/module init, which establishes globals
 * through direct .bss stores the barrier cannot see) is immortal: its
 * region never leaves, so top-level temporaries are reclaimed by the GC
 * exactly as before regions existed.
 *
 * Knob: A_REGIONS=0|off|no|false disables the layer entirely (every
 * entry point becomes a no-op; behaviour is pure mark-sweep GC).
 */
#include "a_runtime_internal.h"

typedef struct ARegion {
    void **objs;            /* growable list of object pointers born here */
    uint32_t n, cap;
    uint32_t depth;         /* region stack depth at enter time */
    int immortal;           /* module frame: never swept */
    struct ARegion *next;   /* active-stack link / pool link */
} ARegion;

static A_RT_TLS ARegion *g_region;       /* innermost active region */
static A_RT_TLS ARegion *g_region_pool;  /* retired regions kept for reuse */
static A_RT_TLS uint32_t g_region_depth;
static int g_regions = -1;
static int g_region_dbg = -1;
static int region_dbg(void) {
    if (g_region_dbg < 0) {
        const char *e = getenv("A_REGION_DEBUG");
        g_region_dbg = (e && *e) ? 1 : 0;
    }
    return g_region_dbg;
}

static int regions_on(void) {
    if (g_regions < 0) {
        const char *e = getenv("A_REGIONS");
        g_regions = !(e && (!strcmp(e, "0") || !strcmp(e, "off") ||
                            !strcmp(e, "no") || !strcmp(e, "false") ||
                            !*e));
    }
    return g_regions;
}

/* ------------------------------------------------------------------ */
/* region stack (driven by a_frame_push / a_frame_pop)                 */
/* ------------------------------------------------------------------ */

void a_region_enter(const char *name) {
    if (!regions_on()) return;
    ARegion *r;
    if (g_region_pool) {
        r = g_region_pool;
        g_region_pool = r->next;
    } else {
        r = (ARegion *)xmalloc(sizeof(ARegion));
        r->objs = NULL;
        r->cap = 0;
    }
    r->n = 0;
    r->depth = ++g_region_depth;
    r->immortal = name && !strcmp(name, "<module>");
    r->next = g_region;
    g_region = r;
    if (region_dbg())
        fprintf(stderr, "[region] enter %s depth=%u\n",
                name ? name : "?", r->depth);
}

void a_region_leave(void) {
    if (!regions_on()) return;
    ARegion *r = g_region;
    if (!r) return;
    g_region = r->next;
    --g_region_depth;
    if (region_dbg())
        fprintf(stderr, "[region] leave depth=%u n=%u immortal=%d\n",
                r->depth, r->n, r->immortal);
    if (!r->immortal) {
        for (uint32_t i = 0; i < r->n; ++i) {
            if (region_dbg() && a_gc_release_obj(r->objs[i]) && region_dbg())
                fprintf(stderr, "[region]   freed %p\n", r->objs[i]);
        }
    }
    r->n = 0;
    r->next = g_region_pool;
    g_region_pool = r;
}

/* ------------------------------------------------------------------ */
/* allocation tracking                                                 */
/* ------------------------------------------------------------------ */

/* Bound on tracked objects per region: a frame that allocates more than
   this falls back to pure-GC reclamation for the overflow (those objects
   simply keep region depth 0 and are never region-swept). Keeps the
   per-frame pointer list — and its sweep cost — bounded on
   allocation-heavy long-running frames, where the GC is the effective
   collector anyway. */
#define REGION_TRACK_CAP (1u << 16)   /* 64K objects = 512 KiB of pointers */

void a_region_note_obj(void *p) {
    if (!regions_on() || !g_region || g_region->immortal) return;
    ARegion *r = g_region;
    if (r->n >= REGION_TRACK_CAP) return;
    if (!a_gc_track_region(p, r->depth)) return;  /* arena / pinned / untracked */
    if (r->n == r->cap) {
        uint32_t nc = r->cap ? r->cap * 2 : 64;
        /* Untracked on purpose: a GC-scanned pointer list would pin every
           young object merely because they are tracked in this list. */
        r->objs = (void **)realloc(r->objs, (size_t)nc * sizeof(void *));
        r->cap = nc;
    }
    r->objs[r->n++] = p;
}

/* ------------------------------------------------------------------ */
/* promotion (escape)                                                  */
/* ------------------------------------------------------------------ */

/* Push every aval field of `ob` that is a heap object onto the worklist.
   Mirrors the object graph coverage of gc_free_obj_interior plus the
   aval-carrying fields (env slots, result value, iter src/lookahead,
   view parent, closure env, struct name/methods/methodFns). */
static void region_push_children(AObj *ob, void ***wl, size_t *n, size_t *cap) {
    aval v = A_NONE;
    uint32_t len = 0;
    switch (ob->type) {
        case T_LIST: case T_TUPLE: case T_SET: {
            AArr *a = (AArr *)ob;
            if (a->items)
                for (uint32_t i = 0; i < a->h.len; ++i) {
                    v = a->items[i];
                    if (a_is_ptr(v)) {
                        if (*n == *cap) {
                            size_t nc = *cap ? *cap * 2 : 64;
                            void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                            *wl = nw; *cap = nc;
                        }
                        (*wl)[(*n)++] = obj_of(v);
                    }
                }
            break;
        }
        case T_DICT: case T_STRUCT: {
            ADict *d = (ADict *)ob;
            for (uint64_t i = 0; i < d->h.len; ++i) {
                aval pair[2] = { d->pairs[i].k, d->pairs[i].v };
                for (int j = 0; j < 2; ++j) {
                    v = pair[j];
                    if (a_is_ptr(v)) {
                        if (*n == *cap) {
                            size_t nc = *cap ? *cap * 2 : 64;
                            void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                            *wl = nw; *cap = nc;
                        }
                        (*wl)[(*n)++] = obj_of(v);
                    }
                }
            }
            if (ob->type == T_STRUCT) {
                AStruct *s = (AStruct *)ob;
                aval extra[3] = { s->name, s->methods, s->methodFns };
                for (int j = 0; j < 3; ++j) {
                    v = extra[j];
                    if (a_is_ptr(v)) {
                        if (*n == *cap) {
                            size_t nc = *cap ? *cap * 2 : 64;
                            void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                            *wl = nw; *cap = nc;
                        }
                        (*wl)[(*n)++] = obj_of(v);
                    }
                }
            }
            break;
        }
        case T_ENV: {
            AEnv *e = (AEnv *)ob;
            len = e->h.len;
            for (uint32_t i = 0; i < len; ++i) {
                v = e->slots[i];
                if (a_is_ptr(v)) {
                    if (*n == *cap) {
                        size_t nc = *cap ? *cap * 2 : 64;
                        void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                        *wl = nw; *cap = nc;
                    }
                    (*wl)[(*n)++] = obj_of(v);
                }
            }
            break;
        }
        case T_RESULT:
            v = ((AResult *)ob)->value;
            if (a_is_ptr(v)) {
                if (*n == *cap) {
                    size_t nc = *cap ? *cap * 2 : 64;
                    void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                    *wl = nw; *cap = nc;
                }
                (*wl)[(*n)++] = obj_of(v);
            }
            break;
        case T_ITER: {
            AIter *it = (AIter *)ob;
            aval pair[2] = { it->src, it->lookahead };
            for (int j = 0; j < 2; ++j) {
                v = pair[j];
                if (a_is_ptr(v)) {
                    if (*n == *cap) {
                        size_t nc = *cap ? *cap * 2 : 64;
                        void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                        *wl = nw; *cap = nc;
                    }
                    (*wl)[(*n)++] = obj_of(v);
                }
            }
            break;
        }
        case T_VIEW:
            v = ((AView *)ob)->parent;
            if (a_is_ptr(v)) {
                if (*n == *cap) {
                    size_t nc = *cap ? *cap * 2 : 64;
                    void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                    *wl = nw; *cap = nc;
                }
                (*wl)[(*n)++] = obj_of(v);
            }
            break;
        case T_FNON:
            v = ((AFnon *)ob)->env;
            if (a_is_ptr(v)) {
                if (*n == *cap) {
                    size_t nc = *cap ? *cap * 2 : 64;
                    void **nw = (void **)xrealloc(*wl, nc * sizeof(void *));
                    *wl = nw; *cap = nc;
                }
                (*wl)[(*n)++] = obj_of(v);
            }
            break;
        default:
            break;  /* T_STR/T_CHAR/T_RANGE/T_INT64/T_BYTES/T_ARENA/T_PYOBJ/T_PYBUF */
    }
}

/* Mark `v`'s object (and everything reachable through its aval fields)
   GC_F_OLD so no region sweep reclaims it. Iterative worklist: the OLD
   flag doubles as the visited marker, so cycles terminate. */
void a_promote(aval v) {
    if (!regions_on() || !a_is_ptr(v)) return;
    AObj *root = obj_of(v);
    if (!root) return;
    void **wl = NULL;
    size_t n = 0, cap = 0;
    wl = (void **)xmalloc(sizeof(void *));
    cap = 1;
    wl[n++] = root;
    while (n) {
        AObj *ob = (AObj *)wl[--n];
        if (!a_gc_regionable(ob)) continue;   /* arena/untracked: immortal */
        if (a_gc_obj_old(ob)) continue;       /* already promoted: visited */
        a_gc_promote_block(ob);
        region_push_children(ob, &wl, &n, &cap);
    }
    free(wl);
}

/* Write barrier: `child` aval is being stored into heap object
   `container` (the object header pointer, == the registered block start
   for every runtime object type). Promotes the child iff the container
   can outlive the child's region. */
void a_region_store(void *container, aval child) {
    if (!regions_on() || !a_is_ptr(child) || !container) return;
    uint32_t cr = a_gc_obj_region(container);
    if (cr == 0 || cr < g_region_depth || a_gc_obj_old(container))
        a_promote(child);
}

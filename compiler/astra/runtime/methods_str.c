#include "methods_internal.h"

aval method_str(aval obj, const char *name, uint64_t n, aval *argv) {
        AStr *s = (AStr *)obj_of(obj);
        if (!strcmp(name, "upper")) { want_args(name, n, 0, 0); return str_case_map(obj, 1); }
        if (!strcmp(name, "lower")) { want_args(name, n, 0, 0); return str_case_map(obj, 0); }
        if (!strcmp(name, "capitalize")) { want_args(name, n, 0, 0); return str_case_map(obj, 2); }
        if (!strcmp(name, "title")) { want_args(name, n, 0, 0); return str_case_map(obj, 3); }
        if (!strcmp(name, "casefold")) { want_args(name, n, 0, 0); return str_case_map(obj, 0); }
        if (!strcmp(name, "swapcase")) { want_args(name, n, 0, 0); return str_case_map(obj, 4); }
        if (!strcmp(name, "strip") || !strcmp(name, "lstrip") || !strcmp(name, "rstrip")) {
            int left = name[0] != 'r';
            int right = strcmp(name, "lstrip") != 0;
            if (n == 0) return str_strip_impl(obj, left, right);
            want_args(name, n, 1, 1);
            /* Strip chars arg: trim those bytes (ASCII set). */
            uint64_t cn;
            const char *cp = str_data(argv[0], &cn);
            uint32_t a = 0, b = s->h.len;
            int inSet = 0;
            if (left) {
                while (a < b) {
                    inSet = 0;
                    for (uint64_t q = 0; q < cn; ++q) if (s->data[a] == cp[q]) { inSet = 1; break; }
                    if (!inSet) break;
                    ++a;
                }
            }
            if (right) {
                while (b > a) {
                    inSet = 0;
                    for (uint64_t q = 0; q < cn; ++q) if (s->data[b - 1] == cp[q]) { inSet = 1; break; }
                    if (!inSet) break;
                    --b;
                }
            }
            return a_str_new(s->data + a, (uint64_t)(b - a));
        }
        if (!strcmp(name, "split") || !strcmp(name, "rsplit")) {
            want_args(name, n, 0, 2);
            aval sep = n >= 1 ? argv[0] : A_NONE;
            int64_t ms = -1;
            if (n >= 2) ms = expect_int64(argv[1], "maxsplit");
            AVec parts = str_split_impl(obj, sep, ms, name[0] == 'r');
            aval r = list_from_vec(parts.p, parts.len, 0);
            free(parts.p);
            return r;
        }
        if (!strcmp(name, "join")) {
            want_args(name, n, 1, 1);
            aval tmp = collect_iterable(argv[0]);
            AArr *a = (AArr *)obj_of(tmp);
            arr_ensure_aval(a);
            for (uint64_t i = 0; i < a->h.len; ++i)
                if (kind_of(a->items[i]) != K_STR && kind_of(a->items[i]) != K_CHAR)
                    panicf("sequence item %llu: expected str instance, %s found",
                           (unsigned long long)i, type_name_cstr(a->items[i]));
            aval r = a_str_joinsep(obj, a->h.len, a->items);
            return r;
        }
        if (!strcmp(name, "replace")) {
            want_args(name, n, 2, 3);
            int64_t count = n == 3 ? expect_int64(argv[2], "count") : -1;
            return str_replace(obj, argv[0], argv[1], count);
        }
        if (!strcmp(name, "startswith") || !strcmp(name, "endswith")) {
            want_args(name, n, 1, 3);
            int64_t st = 0, en = (int64_t)s->h.len;
            if (n >= 2) st = expect_int64(argv[1], "start");
            if (n >= 3) en = expect_int64(argv[2], "end");
            if (st < 0) st += (int64_t)s->h.len;
            if (en < 0) en += (int64_t)s->h.len;
            if (st < 0) st = 0;
            if (en > (int64_t)s->h.len) en = (int64_t)s->h.len;
            uint64_t pn;
            const char *pp = str_data(argv[0], &pn);
            if ((uint64_t)(en - st) < pn) return A_FALSE;
            int c = !strncmp(s->data + st, pp, (size_t)pn);
            if (!strcmp(name, "endswith")) {
                int64_t pos = en - (int64_t)pn;
                c = pos >= st && !strncmp(s->data + pos, pp, (size_t)pn);
            }
            return bool_aval(c);
        }
        if (!strcmp(name, "find") || !strcmp(name, "index") || !strcmp(name, "rindex") || !strcmp(name, "rfind")) {
            want_args(name, n, 1, 3);
            int64_t st = 0;
            if (n >= 2) st = expect_int64(argv[1], "start");
            int64_t en = (int64_t)s->h.len;
            if (n >= 3) {
                en = expect_int64(argv[2], "end");
                /* approximate by clamping search to prefix */
                if (en < (int64_t)s->h.len) {
                    aval tmp = a_str_new(s->data, (uint64_t)en);
                    int64_t r = str_find(tmp, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
                    if ((r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex"))) ||
                        (r >= 0 && en < st)) r = -1;
                    if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                        panicf("Substring not found.");
                    return mkint_full(r);
                }
            }
            int64_t r = str_find(obj, argv[0], st, !strcmp(name, "rindex") || !strcmp(name, "rfind"));
            if (r < 0 && (!strcmp(name, "index") || !strcmp(name, "rindex")))
                panicf("Substring not found.");
            return mkint_full(r);
        }
        if (!strcmp(name, "count")) {
            want_args(name, n, 1, 3);
            int64_t st = n >= 2 ? expect_int64(argv[1], "start") : 0;
            int64_t en = n >= 3 ? expect_int64(argv[2], "end") : (int64_t)s->h.len;
            aval hay = obj;
            if (en < (int64_t)s->h.len) hay = a_str_new(s->data, (uint64_t)en);
            uint64_t hn, nn;
            const char *h = str_data(hay, &hn), *nd = str_data(argv[0], &nn);
            int64_t count = 0, pos = st < 0 ? st + (int64_t)hn : st;
            if (pos < 0) pos = 0;
            if (nn == 0) return mkint_full((int64_t)hn + 1 > pos ? (int64_t)hn - pos + 1 : 0);
            while ((uint64_t)pos + nn <= hn) {
                size_t p;
                if (!find_bytes(h + pos, (size_t)(hn - (uint64_t)pos), nd, (size_t)nn, 0, &p)) break;
                ++count;
                pos += (int64_t)p + (int64_t)nn;
            }
            return mkint_full(count);
        }
        if (!strcmp(name, "isdigit")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 0)); }
        if (!strcmp(name, "isalpha")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 1)); }
        if (!strcmp(name, "isspace")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 2)); }
        if (!strcmp(name, "isalnum")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 3)); }
        if (!strcmp(name, "islower")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 4)); }
        if (!strcmp(name, "isupper")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 5)); }
        if (!strcmp(name, "istitle")) { want_args(name, n, 0, 0); return bool_aval(str_pred(obj, 6)); }
        if (!strcmp(name, "isascii")) {
            want_args(name, n, 0, 0);
            for (uint64_t q = 0; q < s->h.len; ++q) if ((unsigned char)s->data[q] >= 128) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isdecimal") || !strcmp(name, "isnumeric")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_FALSE;
            for (uint64_t q = 0; q < s->h.len; ++q) if (!isdigit((unsigned char)s->data[q])) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isidentifier")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_FALSE;
            if (!isalpha((unsigned char)s->data[0]) && s->data[0] != '_') return A_FALSE;
            for (uint64_t q = 1; q < s->h.len; ++q)
                if (!isalnum((unsigned char)s->data[q]) && s->data[q] != '_') return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "isprintable")) {
            want_args(name, n, 0, 0);
            if (!s->h.len) return A_TRUE;
            for (uint64_t q = 0; q < s->h.len; ++q) if (!isprint((unsigned char)s->data[q])) return A_FALSE;
            return A_TRUE;
        }
        if (!strcmp(name, "expandtabs")) {
            want_args(name, n, 0, 1);
            int64_t tab = n == 1 ? expect_int64(argv[0], "tabsize") : 8;
            if (tab <= 0) tab = 8;
            Buf b;
            buf_init(&b);
            int64_t col = 0;
            for (uint64_t q = 0; q < s->h.len; ++q) {
                char c = s->data[q];
                if (c == '\t') {
                    int64_t spaces = tab - (col % tab);
                    for (int64_t k = 0; k < spaces; ++k) buf_addc(&b, ' ');
                    col += spaces;
                } else if (c == '\n' || c == '\r') {
                    buf_addc(&b, c);
                    col = 0;
                } else {
                    buf_addc(&b, c);
                    ++col;
                }
            }
            return buf_take(&b);
        }
        if (!strcmp(name, "removeprefix") || !strcmp(name, "removesuffix")) {
            want_args(name, n, 1, 1);
            uint64_t pn;
            const char *pp = str_data(argv[0], &pn);
            if (!strcmp(name, "removeprefix")) {
                if (s->h.len >= pn && !memcmp(s->data, pp, (size_t)pn))
                    return a_str_new(s->data + pn, s->h.len - pn);
                return obj;
            }
            if (s->h.len >= pn && !memcmp(s->data + s->h.len - pn, pp, (size_t)pn))
                return a_str_new(s->data, s->h.len - pn);
            return obj;
        }
        if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) {
            want_args(name, n, 1, 1);
            int rev = !strcmp(name, "rpartition");
            int64_t r = str_find(obj, argv[0], 0, rev);
            aval head, sep, tail;
            if (r < 0) {
                if (rev) head = obj, tail = a_str_new("", 0);
                else head = obj, tail = a_str_new("", 0);
                sep = a_str_new("", 0);
                /* Python: not found -> (original, '', '') */
                head = obj;
            } else {
                uint64_t pn;
                str_data(argv[0], &pn);
                head = a_str_new(s->data, (uint64_t)r);
                sep = a_str_new(s->data + r, pn);
                tail = a_str_new(s->data + r + pn, s->h.len - (uint64_t)r - pn);
            }
            aval items[3] = { head, sep, tail };
            return a_tuple_new(3, items);
        }
        if (!strcmp(name, "splitlines")) {
            want_args(name, n, 0, 1);
            int keep = n == 1 && truthy(argv[0]);
            aval lst = a_list_new(0, NULL);
            uint64_t start = 0;
            uint64_t i = 0;
            while (i < s->h.len) {
                char c = s->data[i];
                if (c == '\n' || c == '\r') {
                    uint64_t br_end = i + 1;
                    if (c == '\r' && br_end < s->h.len && s->data[br_end] == '\n') br_end++;
                    uint64_t end = keep ? br_end : i;
                    a_list_push(lst, a_str_new(s->data + start, end - start));
                    i = br_end;
                    start = br_end;
                } else {
                    ++i;
                }
            }
            if (start < s->h.len)
                a_list_push(lst, a_str_new(s->data + start, s->h.len - start));
            else if (start == s->h.len && s->h.len == 0) {
                /* empty string -> [] */
            }
            return lst;
        }
        if (!strcmp(name, "zfill") || !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust")) {
            want_args(name, n, 1, 2);
            int64_t w = expect_int64(argv[0], "width");
            aval fill = n == 2 ? argv[1] : A_NONE;
            int mode = !strcmp(name, "ljust") ? 0 : !strcmp(name, "rjust") ? 1 : !strcmp(name, "center") ? 2 : 3;
            return str_affix(obj, w, fill, mode);
        }
        if (!strcmp(name, "chars")) {
            want_args(name, n, 0, 0);
            aval lst = a_list_new(0, NULL);
            uint64_t i = 0;
            while (i < s->h.len) {
                uint64_t start = i;
                unsigned char c = (unsigned char)s->data[i];
                if (c < 0x80) i += 1;
                else if ((c & 0xE0) == 0xC0) i += 2;
                else if ((c & 0xF0) == 0xE0) i += 3;
                else if ((c & 0xF8) == 0xF0) i += 4;
                else i += 1;
                if (i > s->h.len) i = s->h.len;
                a_list_push(lst, a_char_new(s->data + start, i - start));
            }
            return lst;
        }
        if (!strcmp(name, "repeat")) {
            want_args(name, n, 1, 1);
            int64_t cnt = expect_int64(argv[0], "count");
            if (cnt < 0) cnt = 0;
            Buf b; buf_init(&b);
            for (int64_t i = 0; i < cnt; ++i) buf_addn(&b, s->data, (size_t)s->h.len);
            return buf_take(&b);
        }
        if (!strcmp(name, "contains")) {
            want_args(name, n, 1, 1);
            if (kind_of(argv[0]) != K_STR && kind_of(argv[0]) != K_CHAR) return a_bool(0);
            AStr *hay = (AStr *)obj_of(obj);
            AStr *needle = (AStr *)obj_of(argv[0]);
            if (needle->h.len == 0) return a_bool(1);
            if (needle->h.len > hay->h.len) return a_bool(0);
            for (uint64_t i = 0; i + needle->h.len <= hay->h.len; ++i) {
                if (memcmp(hay->data + i, needle->data, (size_t)needle->h.len) == 0)
                    return a_bool(1);
            }
            return a_bool(0);
        }
        return method_no_method(obj, name);
}

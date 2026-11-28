#define _POSIX_C_SOURCE 200809L
#include "space-lsp.h"
#include "space_rewrite.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- small growing buffer ---------- */

typedef struct { char *data; size_t len, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (!n) return;
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->len + n + 1) c *= 2;
        char *p = realloc(b->data, c);
        if (!p) return;
        b->data = p; b->cap = c;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void bputc(Buf *b, char c) { bput(b, &c, 1); }
static char *bdup(const Buf *b) {
    char *p = malloc(b->len + 1);
    if (!p) return 0;
    if (b->len) memcpy(p, b->data, b->len);
    p[b->len] = 0;
    return p;
}
static char *dupn(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) { if (n) memcpy(p, s, n); p[n] = 0; }
    return p;
}
static void bfree(Buf *b) { free(b->data); b->data = 0; b->len = b->cap = 0; }

/* ---------- minimal JSON tree ---------- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    JType type;
    union {
        int boolean;
        char *num;                 /* original spelling, replaceable */
        struct { char *s; size_t n; } str; /* decoded UTF-8 */
        struct { JVal **v; size_t n, cap; } arr;
        struct { char **k; JVal **v; size_t n, cap; } obj;
    } u;
};
static JVal *jnew(JType t) {
    JVal *v = calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}
static JVal *jnum_new(long long n) {
    JVal *v = jnew(J_NUM);
    if (!v) return 0;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", n);
    v->u.num = dupn(buf, strlen(buf));
    return v;
}
static JVal *jstr_new(const char *s, size_t n) {
    JVal *v = jnew(J_STR);
    if (!v) return 0;
    v->u.str.s = dupn(s, n);
    v->u.str.n = n;
    return v;
}
static void jfree(JVal *v) {
    if (!v) return;
    if (v->type == J_NUM) free(v->u.num);
    else if (v->type == J_STR) free(v->u.str.s);
    else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->u.arr.n; ++i) jfree(v->u.arr.v[i]);
        free(v->u.arr.v);
    } else if (v->type == J_OBJ) {
        for (size_t i = 0; i < v->u.obj.n; ++i) {
            free(v->u.obj.k[i]);
            jfree(v->u.obj.v[i]);
        }
        free(v->u.obj.k); free(v->u.obj.v);
    }
    free(v);
}
static JVal *jget(JVal *v, const char *key) {
    if (!v || v->type != J_OBJ) return 0;
    for (size_t i = 0; i < v->u.obj.n; ++i)
        if (!strcmp(v->u.obj.k[i], key)) return v->u.obj.v[i];
    return 0;
}
static void jset(JVal *obj, const char *key, JVal *val) {
    if (!obj || obj->type != J_OBJ || !val) { jfree(val); return; }
    for (size_t i = 0; i < obj->u.obj.n; ++i) {
        if (!strcmp(obj->u.obj.k[i], key)) {
            jfree(obj->u.obj.v[i]);
            obj->u.obj.v[i] = val;
            return;
        }
    }
    char **k = realloc(obj->u.obj.k, (obj->u.obj.n + 1) * sizeof *k);
    JVal **v = realloc(obj->u.obj.v, (obj->u.obj.n + 1) * sizeof *v);
    if (!k || !v) { free(k); free(v); jfree(val); return; }
    obj->u.obj.k = k; obj->u.obj.v = v;
    obj->u.obj.k[obj->u.obj.n] = dupn(key, strlen(key));
    obj->u.obj.v[obj->u.obj.n] = val;
    ++obj->u.obj.n;
}
static void jpush(JVal *arr, JVal *val) {
    if (!arr || arr->type != J_ARR || !val) { jfree(val); return; }
    JVal **v = realloc(arr->u.arr.v, (arr->u.arr.n + 1) * sizeof *v);
    if (!v) { jfree(val); return; }
    arr->u.arr.v = v;
    arr->u.arr.v[arr->u.arr.n++] = val;
}
static long long jint(JVal *v) {
    return (v && v->type == J_NUM && v->u.num) ? strtoll(v->u.num, 0, 10) : -1;
}
static void jsetint(JVal *v, long long n) {
    if (!v || v->type != J_NUM) return;
    char buf[32];
    int len = snprintf(buf, sizeof buf, "%lld", n);
    char *p = dupn(buf, (size_t)len);
    if (!p) return;
    free(v->u.num); v->u.num = p;
}
static const char *jstring(JVal *v, size_t *n) {
    if (!v || v->type != J_STR) return 0;
    if (n) *n = v->u.str.n;
    return v->u.str.s;
}

/* JSON parser */
typedef struct { const char *s; size_t n, p; int err; } JP;
static void jws(JP *p) {
    while (p->p < p->n) {
        char c = p->s[p->p];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p->p;
        else break;
    }
}
static int hex4(JP *p) {
    if (p->p + 4 > p->n) return -1;
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)p->s[p->p++];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}
static void utf8(Buf *b, unsigned cp) {
    if (cp <= 0x7f) bputc(b, (char)cp);
    else if (cp <= 0x7ff) {
        bputc(b, (char)(0xc0 | (cp >> 6)));
        bputc(b, (char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        bputc(b, (char)(0xe0 | (cp >> 12)));
        bputc(b, (char)(0x80 | ((cp >> 6) & 0x3f)));
        bputc(b, (char)(0x80 | (cp & 0x3f)));
    } else {
        bputc(b, (char)(0xf0 | (cp >> 18)));
        bputc(b, (char)(0x80 | ((cp >> 12) & 0x3f)));
        bputc(b, (char)(0x80 | ((cp >> 6) & 0x3f)));
        bputc(b, (char)(0x80 | (cp & 0x3f)));
    }
}
static char *jstring_read(JP *p, size_t *outlen) {
    if (p->p >= p->n || p->s[p->p] != '"') { p->err = 1; return 0; }
    ++p->p;
    Buf b = {0};
    while (p->p < p->n) {
        unsigned char c = (unsigned char)p->s[p->p++];
        if (c == '"') {
            char *s = bdup(&b);
            if (outlen) *outlen = b.len;
            bfree(&b);
            return s;
        }
        if (c != '\\') { bputc(&b, (char)c); continue; }
        if (p->p >= p->n) break;
        char e = p->s[p->p++];
        if (e == 'u') {
            int cp = hex4(p);
            if (cp < 0) { p->err = 1; break; }
            if (cp >= 0xd800 && cp <= 0xdbff && p->p + 1 < p->n &&
                p->s[p->p] == '\\' && p->s[p->p + 1] == 'u') {
                p->p += 2;
                int lo = hex4(p);
                if (lo >= 0xdc00 && lo <= 0xdfff)
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                else cp = 0xfffd;
            }
            utf8(&b, (unsigned)cp);
        } else if (e == 'b') bputc(&b, '\b');
        else if (e == 'f') bputc(&b, '\f');
        else if (e == 'n') bputc(&b, '\n');
        else if (e == 'r') bputc(&b, '\r');
        else if (e == 't') bputc(&b, '\t');
        else bputc(&b, e);
    }
    p->err = 1;
    bfree(&b);
    return 0;
}
static JVal *jvalue(JP *p);
static JVal *jobject(JP *p) {
    JVal *v = jnew(J_OBJ);
    if (!v) { p->err = 1; return 0; }
    ++p->p; /* { */
    jws(p);
    if (p->p < p->n && p->s[p->p] == '}') { ++p->p; return v; }
    for (;;) {
        jws(p);
        size_t kl = 0;
        char *k = jstring_read(p, &kl);
        if (!k) { p->err = 1; break; }
        jws(p);
        if (p->p >= p->n || p->s[p->p] != ':') { free(k); p->err = 1; break; }
        ++p->p; jws(p);
        JVal *val = jvalue(p);
        if (!val) { free(k); break; }
        char **kk = realloc(v->u.obj.k, (v->u.obj.n + 1) * sizeof *kk);
        JVal **vv = realloc(v->u.obj.v, (v->u.obj.n + 1) * sizeof *vv);
        if (!kk || !vv) { free(kk); free(vv); free(k); jfree(val); p->err = 1; break; }
        v->u.obj.k = kk; v->u.obj.v = vv;
        v->u.obj.k[v->u.obj.n] = k;
        v->u.obj.v[v->u.obj.n] = val;
        ++v->u.obj.n;
        jws(p);
        if (p->p < p->n && p->s[p->p] == ',') { ++p->p; continue; }
        if (p->p < p->n && p->s[p->p] == '}') { ++p->p; return v; }
        p->err = 1;
        break;
    }
    jfree(v);
    return 0;
}
static JVal *jarray(JP *p) {
    JVal *v = jnew(J_ARR);
    if (!v) { p->err = 1; return 0; }
    ++p->p; /* [ */
    jws(p);
    if (p->p < p->n && p->s[p->p] == ']') { ++p->p; return v; }
    for (;;) {
        JVal *item = jvalue(p);
        if (!item) break;
        jpush(v, item);
        jws(p);
        if (p->p < p->n && p->s[p->p] == ',') { ++p->p; continue; }
        if (p->p < p->n && p->s[p->p] == ']') { ++p->p; return v; }
        p->err = 1;
        break;
    }
    jfree(v);
    return 0;
}
static JVal *jvalue(JP *p) {
    jws(p);
    if (p->p >= p->n) { p->err = 1; return 0; }
    char c = p->s[p->p];
    if (c == '{') return jobject(p);
    if (c == '[') return jarray(p);
    if (c == '"') {
        size_t n = 0;
        char *s = jstring_read(p, &n);
        if (!s) return 0;
        JVal *v = jnew(J_STR);
        if (!v) { free(s); p->err = 1; return 0; }
        v->u.str.s = s; v->u.str.n = n;
        return v;
    }
    if (!strncmp(p->s + p->p, "true", 4)) {
        p->p += 4;
        JVal *v = jnew(J_BOOL);
        if (v) v->u.boolean = 1;
        return v;
    }
    if (!strncmp(p->s + p->p, "false", 5)) {
        p->p += 5;
        JVal *v = jnew(J_BOOL);
        if (v) v->u.boolean = 0;
        return v;
    }
    if (!strncmp(p->s + p->p, "null", 4)) { p->p += 4; return jnew(J_NULL); }
    size_t b = p->p;
    if (c == '-' || (c >= '0' && c <= '9')) {
        while (p->p < p->n && strchr("-+.eE0123456789", p->s[p->p])) ++p->p;
        JVal *v = jnew(J_NUM);
        if (!v) { p->err = 1; return 0; }
        v->u.num = dupn(p->s + b, p->p - b);
        return v;
    }
    p->err = 1;
    return 0;
}
static JVal *jparse(const char *s, size_t n) {
    JP p = {s, n, 0, 0};
    JVal *v = jvalue(&p);
    if (!v) return 0;
    jws(&p);
    if (p.err || p.p != p.n) { jfree(v); return 0; }
    return v;
}

/* JSON writer */
static void jwrite_string(Buf *b, const char *s, size_t n) {
    bputc(b, '"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') bput(b, "\\\"", 2);
        else if (c == '\\') bput(b, "\\\\", 2);
        else if (c == '\b') bput(b, "\\b", 2);
        else if (c == '\f') bput(b, "\\f", 2);
        else if (c == '\n') bput(b, "\\n", 2);
        else if (c == '\r') bput(b, "\\r", 2);
        else if (c == '\t') bput(b, "\\t", 2);
        else if (c < 0x20) {
            char esc[7];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            bput(b, esc, 6);
        } else bputc(b, (char)c);
    }
    bputc(b, '"');
}
static void jwrite(Buf *b, JVal *v) {
    if (!v) { bput(b, "null", 4); return; }
    switch (v->type) {
    case J_NULL: bput(b, "null", 4); break;
    case J_BOOL:
        if (v->u.boolean) bput(b, "true", 4);
        else bput(b, "false", 5);
        break;
    case J_NUM: bput(b, v->u.num ? v->u.num : "0",
                     v->u.num ? strlen(v->u.num) : 1); break;
    case J_STR: jwrite_string(b, v->u.str.s, v->u.str.n); break;
    case J_ARR:
        bputc(b, '[');
        for (size_t i = 0; i < v->u.arr.n; ++i) {
            if (i) bputc(b, ',');
            jwrite(b, v->u.arr.v[i]);
        }
        bputc(b, ']');
        break;
    case J_OBJ:
        bputc(b, '{');
        for (size_t i = 0; i < v->u.obj.n; ++i) {
            if (i) bputc(b, ',');
            jwrite_string(b, v->u.obj.k[i], strlen(v->u.obj.k[i]));
            bputc(b, ':');
            jwrite(b, v->u.obj.v[i]);
        }
        bputc(b, '}');
        break;
    }
}
static char *jdump(JVal *v, size_t *n) {
    Buf b = {0};
    jwrite(&b, v);
    *n = b.len;
    return b.data ? b.data : dupn("", 0);
}

/* First `uri` string anywhere in a message. */
static const char *find_uri(JVal *v, size_t *n) {
    if (!v) return 0;
    if (v->type == J_OBJ) {
        for (size_t i = 0; i < v->u.obj.n; ++i) {
            if (!strcmp(v->u.obj.k[i], "uri")) {
                const char *s = jstring(v->u.obj.v[i], n);
                if (s) return s;
            }
        }
        for (size_t i = 0; i < v->u.obj.n; ++i) {
            const char *s = find_uri(v->u.obj.v[i], n);
            if (s) return s;
        }
    } else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->u.arr.n; ++i) {
            const char *s = find_uri(v->u.arr.v[i], n);
            if (s) return s;
        }
    }
    return 0;
}

/* ---------- LSP framing ---------- */

typedef struct { char *data; size_t len, cap; } MBuf;
static void mappend(MBuf *m, const char *s, size_t n) {
    if (m->len + n > m->cap) {
        size_t c = m->cap ? m->cap : 4096;
        while (c < m->len + n) c *= 2;
        char *p = realloc(m->data, c);
        if (!p) return;
        m->data = p; m->cap = c;
    }
    memcpy(m->data + m->len, s, n);
    m->len += n;
}
static void mconsume(MBuf *m, size_t n) {
    if (n >= m->len) { m->len = 0; return; }
    memmove(m->data, m->data + n, m->len - n);
    m->len -= n;
}
static int mtake(MBuf *m, char **json, size_t *len) {
    size_t sep = 0;
    while (sep + 3 < m->len) {
        if (!memcmp(m->data + sep, "\r\n\r\n", 4)) break;
        ++sep;
    }
    if (sep + 3 >= m->len) return 0;
    long long cl = -1;
    size_t line = 0;
    while (line + 1 < sep) {
        size_t end = line;
        while (end + 1 < sep && m->data[end] != '\n') ++end;
        size_t l = end - line;
        if (l > 15 && !strncasecmp(m->data + line, "Content-Length:", 15)) {
            size_t k = line + 15;
            while (k < end && (m->data[k] == ' ' || m->data[k] == '\t')) ++k;
            cl = strtoll(m->data + k, 0, 10);
        }
        line = end + 1;
    }
    if (cl < 0) { mconsume(m, sep + 4); return 0; }
    size_t body = sep + 4;
    if ((unsigned long long)cl > (unsigned long long)(m->len - body)) return 0;
    *json = dupn(m->data + body, (size_t)cl);
    *len = (size_t)cl;
    mconsume(m, body + (size_t)cl);
    return 1;
}
static void mframe(MBuf *m, const char *json, size_t len) {
    char hdr[80];
    int n = snprintf(hdr, sizeof hdr, "Content-Length: %zu\r\n\r\n", len);
    if (n > 0) mappend(m, hdr, (size_t)n);
    mappend(m, json, len);
}

/* ---------- documents and offset maps ---------- */

typedef struct {
    char *uri;
    char *orig; size_t orig_len;
    char *rew;  size_t rew_len;
    size_t *o2r, *r2o; /* byte offsets: original -> rewritten and back */
} Doc;

typedef struct {
    Doc **docs; size_t n, cap;
    space_options *opts;
    struct { char *rew, *orig; size_t rew_at, orig_at; } *names;
    size_t nnames, capnames;
    struct { long long id; char *uri; } *reqs; size_t nreqs, capreqs;
} LspCtx;

static int is_id(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_idc(unsigned char c) { return is_id(c) || (c >= '0' && c <= '9'); }

/* Replies (completion, code actions, ...) usually omit the document URI, so
 * associate each request id with the URI it was sent for. */
static void remember_req(LspCtx *c, JVal *root, const char *uri, size_t n) {
    JVal *id = jget(root, "id");
    if (!uri || !id || id->type != J_NUM) return;
    long long x = jint(id);
    for (size_t i = 0; i < c->nreqs; ++i) {
        if (c->reqs[i].id == x) {
            free(c->reqs[i].uri);
            c->reqs[i].uri = dupn(uri, n);
            return;
        }
    }
    if (c->nreqs == c->capreqs) {
        size_t cap = c->capreqs ? c->capreqs * 2 : 16;
        void *p = realloc(c->reqs, cap * sizeof *c->reqs);
        if (!p) return;
        c->reqs = p; c->capreqs = cap;
    }
    c->reqs[c->nreqs].id = x;
    c->reqs[c->nreqs].uri = dupn(uri, n);
    ++c->nreqs;
}
static const char *forget_req(LspCtx *c, JVal *root) {
    JVal *id = jget(root, "id");
    if (!id || id->type != J_NUM) return 0;
    long long x = jint(id);
    for (size_t i = 0; i < c->nreqs; ++i) {
        if (c->reqs[i].id == x) {
            char *uri = c->reqs[i].uri;
            c->reqs[i].uri = 0;
            return uri;
        }
    }
    return 0;
}

/* Remember how each rewritten identifier was spelled in the source so edits
 * coming back from clangd can be turned back into custom space/:: syntax. */
static void add_name(LspCtx *c, const char *rew, const char *orig,
                     size_t rew_at, size_t orig_at) {
    if (!rew || !orig || !strcmp(rew, orig)) return;
    for (size_t i = 0; i < c->nnames; ++i)
        if (!strcmp(c->names[i].rew, rew) && !strcmp(c->names[i].orig, orig)) return;
    if (c->nnames == c->capnames) {
        size_t cap = c->capnames ? c->capnames * 2 : 32;
        void *p = realloc(c->names, cap * sizeof *c->names);
        if (!p) return;
        c->names = p; c->capnames = cap;
    }
    c->names[c->nnames].rew = dupn(rew, strlen(rew));
    c->names[c->nnames].orig = dupn(orig, strlen(orig));
    c->names[c->nnames].rew_at = rew_at;
    c->names[c->nnames].orig_at = orig_at;
    ++c->nnames;
}
static void collect_names(LspCtx *c, Doc *d) {
    if (!d->r2o) return;
    for (size_t i = 0; i < d->rew_len;) {
        if (!is_id((unsigned char)d->rew[i])) { ++i; continue; }
        size_t j = i;
        while (j < d->rew_len && is_idc((unsigned char)d->rew[j])) ++j;
        size_t a = d->r2o[i], b = d->r2o[j];
        while (a < b && (d->orig[a] == ' ' || d->orig[a] == '\t' ||
                         d->orig[a] == '\n' || d->orig[a] == '\r')) ++a;
        while (b > a && (d->orig[b - 1] == ' ' || d->orig[b - 1] == '\t' ||
                         d->orig[b - 1] == '\n' || d->orig[b - 1] == '\r')) --b;
        if (b > a && b <= d->orig_len) {
            char *rew = dupn(d->rew + i, j - i);
            char *orig = dupn(d->orig + a, b - a);
            add_name(c, rew, orig, i, a);
            free(rew); free(orig);
        }
        i = j;
    }
}
/* `hint` is the rewritten byte offset of the edit's position; it picks the
 * spelling that was used at that spot (bare inside a space, qualified at a
 * use site). With no hint, prefer the shortest spelling. */
static char *unrewrite_text(LspCtx *c, const char *s, size_t n,
                            size_t hint, int have_hint) {
    Buf b = {0};
    for (size_t i = 0; i < n;) {
        const char *best = 0;
        size_t bestn = 0, bestrank = (size_t)-1;
        for (size_t k = 0; k < c->nnames; ++k) {
            size_t len = strlen(c->names[k].rew);
            if (!len || i + len > n) continue;
            if (memcmp(s + i, c->names[k].rew, len)) continue;
            if (i && is_idc((unsigned char)s[i - 1])) continue;
            if (i + len < n && is_idc((unsigned char)s[i + len])) continue;
            size_t at = c->names[k].rew_at;
            size_t rank = have_hint ? (at > hint ? at - hint : hint - at)
                                    : strlen(c->names[k].orig);
            if (!best || rank < bestrank ||
                (rank == bestrank && strlen(c->names[k].orig) < strlen(best))) {
                best = c->names[k].orig; bestn = len; bestrank = rank;
            }
        }
        if (best) { bput(&b, best, strlen(best)); i += bestn; }
        else { bputc(&b, s[i]); ++i; }
    }
    char *out = b.data ? b.data : dupn(s, n);
    return out;
}

static Doc *doc_find(LspCtx *c, const char *uri, size_t n) {
    if (!uri) return 0;
    for (size_t i = 0; i < c->n; ++i)
        if (strlen(c->docs[i]->uri) == n && !memcmp(c->docs[i]->uri, uri, n))
            return c->docs[i];
    return 0;
}
static Doc *doc_get(LspCtx *c, const char *uri, size_t n) {
    Doc *d = doc_find(c, uri, n);
    if (d) return d;
    d = calloc(1, sizeof *d);
    if (!d) return 0;
    d->uri = dupn(uri, n);
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 8;
        Doc **p = realloc(c->docs, cap * sizeof *p);
        if (!p) { free(d->uri); free(d); return 0; }
        c->docs = p; c->cap = cap;
    }
    c->docs[c->n++] = d;
    return d;
}
static void doc_free(Doc *d) {
    free(d->uri); free(d->orig); free(d->rew);
    free(d->o2r); free(d->r2o);
    free(d);
}
static void diag(void *ctx, const char *file, unsigned line, unsigned col,
                 const char *id, const char *msg) {
    (void)ctx;
    fprintf(stderr, "okc lsp: %s:%u:%u: %s: %s\n", file, line, col, id, msg);
}
/* Fallback for a very long line: align a common prefix and suffix only. */
static void map_line_simple(const char *a, size_t n1, const char *b, size_t n2,
                            size_t o1, size_t o2, Doc *d) {
    size_t p = 0;
    while (p < n1 && p < n2 && a[p] == b[p]) ++p;
    size_t s = 0;
    while (s < n1 - p && s < n2 - p && a[n1 - 1 - s] == b[n2 - 1 - s]) ++s;
    for (size_t i = 0; i < p; ++i) { d->o2r[o1 + i] = o2 + i; d->r2o[o2 + i] = o1 + i; }
    for (size_t i = 0; i < s; ++i) {
        d->o2r[o1 + n1 - 1 - i] = o2 + n2 - 1 - i;
        d->r2o[o2 + n2 - 1 - i] = o1 + n1 - 1 - i;
    }
    for (size_t i = p; i < n1 - s; ++i) d->o2r[o1 + i] = o2 + p;
    for (size_t i = p; i < n2 - s; ++i) d->r2o[o2 + i] = o1 + p;
}
static void map_line(const char *a, size_t n1, const char *b, size_t n2,
                     size_t o1, size_t o2, Doc *d) {
    if (!n1 || !n2) return;
    if (n1 * n2 > 1024UL * 1024UL) { map_line_simple(a, n1, b, n2, o1, o2, d); return; }
    size_t w = n2 + 1;
    unsigned short *dp = calloc((n1 + 1) * w, sizeof *dp);
    if (!dp) { map_line_simple(a, n1, b, n2, o1, o2, d); return; }
    for (size_t i = n1 + 1; i-- > 0;)
        for (size_t j = n2 + 1; j-- > 0;) {
            if (i < n1 && j < n2 && a[i] == b[j])
                dp[i * w + j] = (unsigned short)(dp[(i + 1) * w + j + 1] + 1);
            else {
                unsigned short x = i < n1 ? dp[(i + 1) * w + j] : 0;
                unsigned short y = j < n2 ? dp[i * w + j + 1] : 0;
                dp[i * w + j] = x > y ? x : y;
            }
        }
    size_t i = 0, j = 0;
    while (i < n1 || j < n2) {
        if (i < n1 && j < n2 && a[i] == b[j]) {
            d->o2r[o1 + i] = o2 + j;
            d->r2o[o2 + j] = o1 + i;
            ++i; ++j;
        } else if (j >= n2 || (i < n1 && dp[(i + 1) * w + j] >= dp[i * w + j + 1])) {
            if (i < n1) d->o2r[o1 + i] = o2 + j;
            ++i;
        } else {
            if (j < n2) d->r2o[o2 + j] = o1 + i;
            ++j;
        }
    }
    d->o2r[o1 + n1] = o2 + n2;
    d->r2o[o2 + n2] = o1 + n1;
    free(dp);
}
static void build_map(Doc *d) {
    free(d->o2r); free(d->r2o);
    d->o2r = malloc((d->orig_len + 1) * sizeof *d->o2r);
    d->r2o = malloc((d->rew_len + 1) * sizeof *d->r2o);
    if (!d->o2r || !d->r2o) {
        free(d->o2r); free(d->r2o); d->o2r = d->r2o = 0;
        return;
    }
    for (size_t i = 0; i <= d->orig_len; ++i)
        d->o2r[i] = i < d->rew_len ? i : d->rew_len;
    for (size_t i = 0; i <= d->rew_len; ++i)
        d->r2o[i] = i < d->orig_len ? i : d->orig_len;
    size_t i = 0, j = 0;
    while (i < d->orig_len || j < d->rew_len) {
        size_t ie = i, je = j;
        while (ie < d->orig_len && d->orig[ie] != '\n') ++ie;
        if (ie < d->orig_len) ++ie;
        while (je < d->rew_len && d->rew[je] != '\n') ++je;
        if (je < d->rew_len) ++je;
        map_line(d->orig + i, ie - i, d->rew + j, je - j, i, j, d);
        i = ie; j = je;
    }
}
static void doc_rewrite(LspCtx *c, Doc *d) {
    size_t len = 0;
    char *out = space_rewrite(d->orig, d->orig_len, c->opts, diag, 0, &len);
    if (!out) {
        out = dupn(d->orig ? d->orig : "", d->orig_len);
        len = d->orig_len;
    }
    free(d->rew);
    d->rew = out; d->rew_len = len;
    build_map(d);
    collect_names(c, d);
}
static void doc_set_orig(Doc *d, const char *text, size_t len) {
    free(d->orig);
    d->orig = dupn(text ? text : "", len);
    d->orig_len = len;
}
static size_t line_start(const char *s, size_t len, long long line) {
    if (line <= 0) return 0;
    size_t p = 0;
    for (long long l = 0; l < line && p < len; ++l) {
        while (p < len && s[p] != '\n') ++p;
        if (p < len) ++p;
    }
    return p;
}
static size_t line_end(const char *s, size_t len, size_t start) {
    size_t p = start;
    while (p < len && s[p] != '\n') ++p;
    return p;
}
static size_t unit_to_byte(const char *s, size_t end, long long unit) {
    size_t p = 0;
    long long u = 0;
    while (p < end && u < unit) {
        unsigned char c = (unsigned char)s[p];
        size_t step = 1;
        if (c >= 0x80)
            while (p + step < end && ((unsigned char)s[p + step] & 0xc0) == 0x80) ++step;
        p += step; ++u;
    }
    return p;
}
static long long byte_to_unit(const char *s, size_t from, size_t to) {
    size_t p = from;
    long long u = 0;
    while (p < to) {
        unsigned char c = (unsigned char)s[p];
        size_t step = 1;
        if (c >= 0x80)
            while (p + step < to && ((unsigned char)s[p + step] & 0xc0) == 0x80) ++step;
        p += step; ++u;
    }
    return u;
}
static size_t pos_offset(Doc *d, JVal *pos) {
    long long line = jint(jget(pos, "line"));
    long long ch = jint(jget(pos, "character"));
    if (line < 0) line = 0;
    if (ch < 0) ch = 0;
    size_t a = line_start(d->orig, d->orig_len, line);
    return a + unit_to_byte(d->orig, line_end(d->orig, d->orig_len, a), ch);
}
static long long map_char(Doc *d, long long line, long long ch, int to_rew) {
    if (!d || !d->o2r || !d->r2o) return ch;
    const char *src = to_rew ? d->orig : d->rew;
    size_t sl = to_rew ? d->orig_len : d->rew_len;
    const char *dst = to_rew ? d->rew : d->orig;
    size_t dl = to_rew ? d->rew_len : d->orig_len;
    if (line < 0) line = 0;
    size_t ss = line_start(src, sl, line);
    size_t se = line_end(src, sl, ss);
    size_t off = ss + unit_to_byte(src, se, ch);
    size_t mapped = to_rew ? d->o2r[off] : d->r2o[off];
    size_t ds = line_start(dst, dl, line);
    size_t de = line_end(dst, dl, ds);
    if (mapped < ds) mapped = ds;
    if (mapped > de) mapped = de;
    return byte_to_unit(dst, ds, mapped);
}
static void map_tree(JVal *v, Doc *d, int to_rew) {
    if (!v || !d) return;
    if (v->type == J_OBJ) {
        if (jget(v, "line") && jget(v, "character")) {
            JVal *c = jget(v, "character");
            jsetint(c, map_char(d, jint(jget(v, "line")), jint(c), to_rew));
            return;
        }
        for (size_t i = 0; i < v->u.obj.n; ++i) map_tree(v->u.obj.v[i], d, to_rew);
    } else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->u.arr.n; ++i) map_tree(v->u.arr.v[i], d, to_rew);
    }
}

/* Apply incremental didChange edits to the stored original text. */
static void apply_changes(Doc *d, JVal *changes) {
    if (!changes || changes->type != J_ARR || !d->orig) return;
    for (size_t i = 0; i < changes->u.arr.n; ++i) {
        JVal *ch = changes->u.arr.v[i];
        size_t nl = 0;
        const char *t = jstring(jget(ch, "text"), &nl);
        if (!t) continue;
        JVal *range = jget(ch, "range");
        Buf b = {0};
        if (!range) {
            bput(&b, t, nl);
        } else {
            size_t a = pos_offset(d, jget(range, "start"));
            size_t z = pos_offset(d, jget(range, "end"));
            if (a > d->orig_len) a = d->orig_len;
            if (z < a) z = a;
            if (z > d->orig_len) z = d->orig_len;
            bput(&b, d->orig, a);
            bput(&b, t, nl);
            bput(&b, d->orig + z, d->orig_len - z);
        }
        free(d->orig);
        d->orig = b.data ? b.data : dupn("", 0);
        d->orig_len = b.len;
    }
}

/* ---------- message translation ---------- */

static char *client_message(LspCtx *c, const char *json, size_t len, size_t *outlen) {
    JVal *root = jparse(json, len);
    if (!root) return dupn(json, len);
    *outlen = len;
    size_t ul = 0;
    const char *uri = find_uri(root, &ul);
    if (uri) remember_req(c, root, uri, ul);
    Doc *d = uri ? doc_get(c, uri, ul) : 0;
    size_t ml = 0;
    const char *method = jstring(jget(root, "method"), &ml);
    if (d && method && ml == strlen("textDocument/didOpen") &&
        !memcmp(method, "textDocument/didOpen", ml)) {
        JVal *td = jget(jget(root, "params"), "textDocument");
        JVal *text = jget(td, "text");
        if (text && text->type == J_STR) {
            c->opts->filename = d->uri;
            doc_set_orig(d, text->u.str.s, text->u.str.n);
            doc_rewrite(c, d);
            JVal *nv = jstr_new(d->rew, d->rew_len);
            jset(td, "text", nv);
        }
    } else if (d && method && ml == strlen("textDocument/didChange") &&
               !memcmp(method, "textDocument/didChange", ml)) {
        JVal *params = jget(root, "params");
        JVal *changes = jget(params, "contentChanges");
        if (changes && changes->type == J_ARR) {
            if (!d->orig) {
                JVal *first = changes->u.arr.n ? changes->u.arr.v[0] : 0;
                size_t nl = 0;
                const char *t = jstring(jget(first, "text"), &nl);
                if (t) doc_set_orig(d, t, nl);
            } else apply_changes(d, changes);
            c->opts->filename = d->uri;
            doc_rewrite(c, d);
            JVal *one = jnew(J_OBJ);
            jset(one, "text", jstr_new(d->rew, d->rew_len));
            JVal *arr = jnew(J_ARR);
            jpush(arr, one);
            jset(params, "contentChanges", arr);
        }
    } else if (d) {
        map_tree(root, d, 1);
    }
    char *out = jdump(root, outlen);
    jfree(root);
    return out;
}
/* Fields in clangd replies that contain source text to insert or display. */
static int is_text_field(const char *k) {
    static const char *keys[] = {"newText", "insertText", "newName", "label",
                                 "filterText", "sortText", "placeholder",
                                 "title", "detail", "value", "text", 0};
    for (int i = 0; keys[i]; ++i) if (!strcmp(k, keys[i])) return 1;
    return 0;
}
static size_t rew_offset(Doc *d, JVal *pos) {
    if (!d || !pos) return 0;
    long long line = jint(jget(pos, "line"));
    long long ch = jint(jget(pos, "character"));
    if (line < 0) line = 0;
    if (ch < 0) ch = 0;
    size_t a = line_start(d->rew, d->rew_len, line);
    return a + unit_to_byte(d->rew, line_end(d->rew, d->rew_len, a), ch);
}
/* Find the position inside a subtree so text fields are converted using the
 * spelling appropriate to that location, regardless of JSON key order. */
static void find_hint(JVal *v, Doc *d, size_t *hint) {
    if (!v) return;
    if (v->type == J_OBJ) {
        if (jget(v, "line") && jget(v, "character")) {
            *hint = rew_offset(d, v);
            return;
        }
        for (size_t i = 0; i < v->u.obj.n; ++i) find_hint(v->u.obj.v[i], d, hint);
    } else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->u.arr.n; ++i) find_hint(v->u.arr.v[i], d, hint);
    }
}
static void unrewrite_strings(JVal *v, LspCtx *c, Doc *d, size_t *hint) {
    if (!v) return;
    if (v->type == J_OBJ) {
        size_t local = *hint;
        find_hint(v, d, &local);
        for (size_t i = 0; i < v->u.obj.n; ++i) {
            const char *key = v->u.obj.k[i];
            JVal *child = v->u.obj.v[i];
            if (child && child->type == J_STR && is_text_field(key)) {
                size_t n = 0;
                const char *s = jstring(child, &n);
                char *out = unrewrite_text(c, s, n, local, d != 0);
                jset(v, key, jstr_new(out, strlen(out)));
                free(out);
            } else {
                unrewrite_strings(child, c, d, &local);
            }
        }
    } else if (v->type == J_ARR) {
        for (size_t i = 0; i < v->u.arr.n; ++i)
            unrewrite_strings(v->u.arr.v[i], c, d, hint);
    }
}
static char *server_message(LspCtx *c, const char *json, size_t len, size_t *outlen) {
    JVal *root = jparse(json, len);
    if (!root) return dupn(json, len);
    *outlen = len;
    JVal *caps = jget(jget(root, "result"), "capabilities");
    /* Full sync keeps the proxy's document copy authoritative. */
    if (caps) {
        JVal *sync = jget(caps, "textDocumentSync");
        if (sync && sync->type == J_NUM) jsetint(sync, 1);
        else jset(caps, "textDocumentSync", jnum_new(1));
    }
    size_t ul = 0;
    const char *uri = find_uri(root, &ul);
    if (!uri) {
        uri = forget_req(c, root);
        if (uri) ul = strlen(uri);
    }
    Doc *d = uri ? doc_find(c, uri, ul) : 0;
    size_t hint = 0;
    unrewrite_strings(root, c, d, &hint);
    if (d) map_tree(root, d, 0);
    char *out = jdump(root, outlen);
    jfree(root);
    return out;
}

/* ---------- clangd process ---------- */

static void collect_defines(space_options *o, char **argv) {
    for (int i = 1; argv[i]; ++i) {
        const char *a = argv[i];
        const char *v = 0;
        if ((!strcmp(a, "-D") || !strcmp(a, "-U")) && argv[i + 1]) v = argv[++i];
        else if (a[0] == '-' && (a[1] == 'D' || a[1] == 'U') && a[2]) v = a + 2;
        if (!v) continue;
        size_t n = 0;
        while (v[n] && v[n] != '=' && v[n] != '(') ++n;
        char **defs = (char **)o->defines;
        char **p = realloc(defs, (o->ndefines + 1) * sizeof *p);
        if (!p) continue;
        defs = p;
        o->defines = (const char *const *)defs;
        defs[o->ndefines++] = dupn(v, n);
    }
}
static int spawn_clangd(const char *clangd, char **args, int *infd, int *outfd, pid_t *pid) {
    int to[2], from[2];
    if (pipe(to) || pipe(from)) return -1;
    *pid = fork();
    if (*pid < 0) { close(to[0]); close(to[1]); close(from[0]); close(from[1]); return -1; }
    if (*pid == 0) {
        dup2(to[0], STDIN_FILENO);
        dup2(from[1], STDOUT_FILENO);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        execvp(clangd, args);
        _exit(127);
    }
    close(to[0]); close(from[1]);
    *infd = to[1]; *outfd = from[0];
    int flags = fcntl(*infd, F_GETFL, 0);
    if (flags >= 0) fcntl(*infd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(*outfd, F_GETFL, 0);
    if (flags >= 0) fcntl(*outfd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}
static ssize_t read_some(int fd, char *buf, size_t cap) {
    ssize_t n = read(fd, buf, cap);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return n;
}
static void usage(void) {
    fprintf(stderr,
            "usage: okc lsp [--clangd=PATH] [clangd args...]\n"
            "       SPACE_CLANGD=PATH selects the underlying clangd (default: clangd)\n");
}

int space_lsp_main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
    const char *clangd = getenv("SPACE_CLANGD");
    if (!clangd || !*clangd) clangd = "clangd";
    char **args = calloc((size_t)argc + 2, sizeof *args);
    if (!args) return 1;
    int nargs = 0;
    args[nargs++] = (char *)clangd;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--clangd=", 9)) {
            clangd = argv[i] + 9;
            args[0] = (char *)clangd;
        } else if (!strcmp(argv[i], "--clangd") && i + 1 < argc) {
            clangd = argv[++i];
            args[0] = (char *)clangd;
        } else {
            args[nargs++] = argv[i];
        }
    }
    args[nargs] = 0;

    space_options opts = {0};
    collect_defines(&opts, args);
    LspCtx ctx = {0};
    ctx.opts = &opts;

    signal(SIGPIPE, SIG_IGN);
    int to_child = -1, from_child = -1;
    pid_t pid = 0;
    if (spawn_clangd(clangd, args, &to_child, &from_child, &pid)) {
        perror("okc lsp: spawn clangd");
        return 1;
    }

    MBuf in_client = {0}, in_child = {0}, to_c = {0}, out_c = {0};
    char buf[65536];
    int client_open = 1, child_out_open = 1, child_in_open = 1, stdout_open = 1;
    while (client_open || child_out_open || child_in_open || stdout_open) {
        struct pollfd fds[4];
        int idx[4], nfds = 0;
        if (client_open) { fds[nfds].fd = STDIN_FILENO; fds[nfds].events = POLLIN; idx[nfds++] = 0; }
        if (child_out_open) { fds[nfds].fd = from_child; fds[nfds].events = POLLIN; idx[nfds++] = 1; }
        if (child_in_open && to_c.len) { fds[nfds].fd = to_child; fds[nfds].events = POLLOUT; idx[nfds++] = 2; }
        if (stdout_open && out_c.len) { fds[nfds].fd = STDOUT_FILENO; fds[nfds].events = POLLOUT; idx[nfds++] = 3; }
        if (!nfds) {
            if (to_c.len && child_in_open) continue;
            if (out_c.len && stdout_open) continue;
            break;
        }
        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR) continue;
            perror("okc lsp: poll");
            break;
        }
        for (int k = 0; k < nfds; ++k) {
            short re = fds[k].revents;
            int kind = idx[k];
            if (kind == 0 && (re & (POLLIN | POLLHUP))) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
                if (n > 0) mappend(&in_client, buf, (size_t)n);
                else if (n == 0) client_open = 0;
                else if (errno != EAGAIN && errno != EINTR) client_open = 0;
            } else if (kind == 1 && (re & (POLLIN | POLLHUP))) {
                ssize_t n = read_some(from_child, buf, sizeof buf);
                if (n > 0) mappend(&in_child, buf, (size_t)n);
                else if (n == 0) child_out_open = 0;
                else if (re & POLLHUP) child_out_open = 0;
            } else if (kind == 2 && (re & POLLOUT)) {
                ssize_t n = write(to_child, to_c.data, to_c.len);
                if (n > 0) mconsume(&to_c, (size_t)n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    close(to_child); child_in_open = 0; to_child = -1;
                }
            } else if (kind == 3 && (re & POLLOUT)) {
                ssize_t n = write(STDOUT_FILENO, out_c.data, out_c.len);
                if (n > 0) mconsume(&out_c, (size_t)n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR) stdout_open = 0;
            }
        }
        char *json;
        size_t len;
        while (in_client.len && mtake(&in_client, &json, &len)) {
            size_t nl = 0;
            char *out = client_message(&ctx, json, len, &nl);
            mframe(&to_c, out, nl);
            free(out); free(json);
        }
        while (in_child.len && mtake(&in_child, &json, &len)) {
            size_t nl = 0;
            char *out = server_message(&ctx, json, len, &nl);
            mframe(&out_c, out, nl);
            free(out); free(json);
        }
        if (!client_open && !to_c.len && child_in_open) {
            close(to_child); child_in_open = 0; to_child = -1;
        }
        if (!child_out_open && !out_c.len && stdout_open) {
            close(STDOUT_FILENO); stdout_open = 0;
        }
        if (!client_open && !child_out_open && !to_c.len && !out_c.len) break;
    }
    if (child_in_open) close(to_child);
    if (child_out_open) close(from_child);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    free(args);
    for (size_t i = 0; i < ctx.n; ++i) doc_free(ctx.docs[i]);
    free(ctx.docs);
    for (size_t i = 0; i < ctx.nnames; ++i) {
        free(ctx.names[i].rew);
        free(ctx.names[i].orig);
    }
    free(ctx.names);
    for (size_t i = 0; i < ctx.nreqs; ++i) free(ctx.reqs[i].uri);
    free(ctx.reqs);
    for (size_t i = 0; i < opts.ndefines; ++i) free((char *)opts.defines[i]);
    free((char *)opts.defines);
    free(in_client.data); free(in_child.data); free(to_c.data); free(out_c.data);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

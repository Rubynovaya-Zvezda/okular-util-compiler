#define _POSIX_C_SOURCE 200809L
#include "space_rewrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAXS 256
#define MAXC 64
typedef struct { char *data; size_t len, cap; } Buf;
static void ap(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->len + n + 1) c *= 2;
        b->data = realloc(b->data, c); b->cap = c;
    }
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
}
static void ac(Buf *b, char c) { ap(b, &c, 1); }
typedef struct { char *name, *mapped; } Pair;
typedef struct { char *name; Pair cs[MAXC]; size_t n; } Type;
typedef struct {
    const char *src; size_t len, pos; Buf out;
    unsigned line, col;
    int in_space, depth;
    char *prefix, *pending, *expect;
    Type types[MAXS]; size_t ntypes;
    Pair consts[MAXS]; size_t nconsts;
    Pair vars[MAXS]; size_t nvars;
    const space_options *opts;
    space_diag_fn diag; void *ctx;
} Sc;
static char *dupn(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}
static void report(Sc *s, const char *id, const char *msg, unsigned line, unsigned col) {
    if (s->diag) s->diag(s->ctx, s->opts->filename, line, col, id, msg);
}
static int ids(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int idc(unsigned char c) { return ids(c) || (c >= '0' && c <= '9'); }
static size_t iend(const char *x, size_t n, size_t p) {
    while (p < n && idc((unsigned char)x[p])) ++p;
    return p;
}
static void hs(const char *x, size_t n, size_t *p) {
    while (*p < n && (x[*p] == ' ' || x[*p] == '\t' || x[*p] == '\r')) ++*p;
}
static int word(const char *x, size_t a, size_t b, const char *w) {
    size_t n = strlen(w);
    return b - a == n && memcmp(x + a, w, n) == 0;
}
static int kw(const char *x, size_t a, size_t b) {
    static const char *k[] = {"auto","break","case","char","const","continue",
      "default","do","double","else","enum","extern","float","for","goto","if",
      "inline","int","long","register","restrict","return","short","signed",
      "sizeof","static","struct","switch","typedef","union","unsigned","void",
      "volatile","while","_Alignas","_Alignof","_Atomic","_Bool","_Complex",
      "_Generic","_Imaginary","_Noreturn","_Static_assert","_Thread_local",
      "_BitInt","alignas","alignof","bool","constexpr","false","nullptr",
      "static_assert","thread_local","true","typeof","typeof_unqual","space",0};
    for (int i = 0; k[i]; ++i) if (word(x, a, b, k[i])) return 1;
    return 0;
}
static Type *find_type(Sc *s, const char *n) {
    for (size_t i = 0; i < s->ntypes; ++i)
        if (!strcmp(s->types[i].name, n)) return &s->types[i];
    return 0;
}
static char *find_pair(Pair *p, size_t n, const char *name, size_t nl) {
    for (size_t i = 0; i < n; ++i)
        if (strlen(p[i].name) == nl && !memcmp(p[i].name, name, nl)) return p[i].mapped;
    return 0;
}
static void add_pair(Pair *p, size_t *n, const char *a, size_t al, const char *b) {
    if (*n < MAXS) { p[*n].name = dupn(a, al); p[*n].mapped = dupn(b, strlen(b)); ++*n; }
}
typedef struct { size_t end, na, nb; } Open;
static int space_open(Sc *s, size_t p, Open *o) {
    const char *x = s->src;
    size_t q = p + 5;
    hs(x, s->len, &q);
    if (q >= s->len || !ids((unsigned char)x[q])) return 0;
    o->na = q;
    o->nb = iend(x, s->len, q);
    q = o->nb; hs(x, s->len, &q);
    if (q >= s->len || x[q] != '{') return 0;
    o->end = q + 1;
    return 1;
}
typedef struct { size_t end, a1, b1, a2, b2, a3, b3; } Qual;
static int qualified(Sc *s, size_t p, Qual *m) {
    const char *x = s->src;
    m->a1 = p; m->b1 = iend(x, s->len, p);
    if (m->b1 == p) return 0;
    size_t q = m->b1; hs(x, s->len, &q);
    if (q + 1 >= s->len || x[q] != ':' || x[q + 1] != ':') return 0;
    q += 2; hs(x, s->len, &q);
    if (q >= s->len || !ids((unsigned char)x[q])) return 0;
    m->a2 = q; m->b2 = iend(x, s->len, q);
    q = m->b2; hs(x, s->len, &q);
    m->a3 = m->b3 = q;
    m->end = m->b2;
    if (q < s->len && x[q] == '.') {
        ++q; hs(x, s->len, &q);
        if (q >= s->len || !ids((unsigned char)x[q])) return 0;
        m->a3 = q; m->b3 = iend(x, s->len, q);
        m->end = m->b3;
        size_t r = m->b3; hs(x, s->len, &r);
        if (r < s->len && x[r] == '.') return 0;
    }
    return 1;
}
static void copy_span(Sc *s, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        char c = s->src[i]; ac(&s->out, c);
        if (c == '\n') { ++s->line; s->col = 1; } else ++s->col;
    }
    s->pos = to;
}
static void copy_struct(Sc *s, size_t p) {
    size_t q = p;
    while (q < s->len && s->src[q] != '{') ++q;
    if (q >= s->len) { copy_span(s, p, s->len); return; }
    int d = 0; size_t e = q;
    for (; e < s->len; ++e) {
        if (s->src[e] == '{') ++d;
        else if (s->src[e] == '}' && --d == 0) { ++e; break; }
    }
    copy_span(s, p, e);
}
static void do_enum(Sc *s, size_t p) {
    const char *x = s->src;
    size_t q = p + 4; hs(x, s->len, &q);
    if (q >= s->len || x[q] != '{') return;
    int d = 0; size_t close = q;
    for (; close < s->len; ++close) {
        if (x[close] == '{') ++d;
        else if (x[close] == '}' && --d == 0) break;
    }
    if (close >= s->len) close = s->len;
    size_t da = close + 1, db = da; hs(x, s->len, &da);
    if (da < s->len && ids((unsigned char)x[da])) db = iend(x, s->len, da);
    char base[512], ep[520], typ[512];
    if (s->in_space) snprintf(base, sizeof base, "%s_%.*s", s->prefix, (int)(db-da), x+da);
    else snprintf(base, sizeof base, "%.*s", (int)(db-da), x+da);
    snprintf(ep, sizeof ep, "%s_", base);
    snprintf(typ, sizeof typ, "%s", base);
    copy_span(s, p, q + 1);
    int want = 1;
    Type *t = 0;
    if (db > da && s->ntypes < MAXS) {
        t = &s->types[s->ntypes++];
        memset(t, 0, sizeof *t);
        t->name = dupn(typ, strlen(typ));
    }
    for (size_t i = q + 1; i < close; ++i) {
        char c = x[i];
        if (want && ids((unsigned char)c)) {
            size_t e = iend(x, s->len, i);
            if (e > close) e = close;
            char full[1024];
            snprintf(full, sizeof full, "%s%.*s", ep, (int)(e-i), x+i);
            ap(&s->out, full, strlen(full));
            s->col += (unsigned)(e - i);
            if (t && t->n < MAXC) {
                t->cs[t->n].name = dupn(x+i, e-i);
                t->cs[t->n].mapped = dupn(full, strlen(full));
                ++t->n;
            }
            add_pair(s->consts, &s->nconsts, x+i, e-i, full);
            want = 0; i = e - 1;
        } else {
            ac(&s->out, c);
            if (c == ',') want = 1;
            if (c == '\n') { ++s->line; s->col = 1; } else ++s->col;
        }
    }
    if (close < s->len) ac(&s->out, '}'), ++s->col;
    s->pos = close + 1;
    (void)typ;
}
static void do_qual(Sc *s, size_t p, Qual *m) {
    const char *x = s->src;
    Buf *b = &s->out;
    ap(b, x + m->a1, m->b1 - m->a1); ac(b, '_');
    ap(b, x + m->a2, m->b2 - m->a2);
    char first[600];
    int n = snprintf(first, sizeof first, "%.*s_%.*s",
        (int)(m->b1-m->a1), x+m->a1, (int)(m->b2-m->a2), x+m->a2);
    if (m->b3 > m->a3) { ac(b, '_'); ap(b, x + m->a3, m->b3 - m->a3); }
    size_t r = m->end; hs(x, s->len, &r);
    if (r + 1 < s->len && x[r] == ':' && x[r+1] == ':')
        report(s, "space-bad-form", "only two-part qualified names are allowed", s->line, s->col);
    if (n > 0 && find_type(s, first)) {
        free(s->pending); s->pending = dupn(first, (size_t)n);
    }
    s->col += (unsigned)(m->end - p); s->pos = m->end;
}
static void do_ident(Sc *s, size_t p, size_t e) {
    const char *x = s->src;
    size_t n = e - p;
    if (s->pending) {
        add_pair(s->vars, &s->nvars, x+p, n, s->pending);
        free(s->expect); s->expect = dupn(s->pending, strlen(s->pending));
        free(s->pending); s->pending = 0;
    } else {
        char *v = find_pair(s->vars, s->nvars, x+p, n);
        if (v) { free(s->expect); s->expect = dupn(v, strlen(v)); }
    }
    char *mapped = 0;
    if (s->expect) {
        Type *t = find_type(s, s->expect);


        if (t) mapped = find_pair(t->cs, t->n, x+p, n);
    }
    if (!mapped) {
        char *g = find_pair(s->consts, s->nconsts, x+p, n);
        int count = 0;
        for (size_t i = 0; i < s->nconsts; ++i)
            if (strlen(s->consts[i].name) == n && !memcmp(s->consts[i].name, x+p, n)) ++count;
        if (count == 1) mapped = g;
    }
    if (mapped) { ap(&s->out, mapped, strlen(mapped)); s->pos = e; s->col += (unsigned)n; return; }
    int pf = s->in_space && !kw(x,p,e) && x[p] != '_';
    if (pf && n > strlen(s->prefix) &&
        !memcmp(x+p, s->prefix, strlen(s->prefix)) && x[p+strlen(s->prefix)] == '_') pf = 0;
    if (pf) {
        ap(&s->out, s->prefix, strlen(s->prefix)); ac(&s->out, '_');
    }
    ap(&s->out, x+p, n);
    s->pos = e; s->col += (unsigned)n;
}
char *space_rewrite(const char *text, size_t len, const space_options *opts,
                    space_diag_fn diag, void *ctx, size_t *out_len) {
    Sc s; memset(&s, 0, sizeof s);
    s.src = text; s.len = len; s.line = 1; s.col = 1;
    s.opts = opts; s.diag = diag; s.ctx = ctx;
    while (s.pos < s.len) {
        size_t p = s.pos;
        char c = s.src[p];
        if (c == '\n') { ac(&s.out, c); ++s.pos; ++s.line; s.col = 1; continue; }
        if (c == '/' && p+1 < s.len && s.src[p+1] == '*') {
            size_t e = p+2;
            while (e+1 < s.len && !(s.src[e]=='*' && s.src[e+1]=='/')) ++e;
            if (e+1 < s.len) e += 2;
            copy_span(&s, p, e); continue;
        }
        if (c == '/' && p+1 < s.len && s.src[p+1] == '/') {
            size_t e = p;
            while (e < s.len && s.src[e] != '\n') ++e;
            copy_span(&s, p, e); continue;
        }
        if (c == '"' || c == '\'') {
            size_t e = p+1;
            while (e < s.len && s.src[e] != '\n') {
                if (s.src[e] == '\\' && e+1 < s.len) { e += 2; continue; }
                if (s.src[e] == c) { ++e; break; }
                ++e;
            }
            copy_span(&s, p, e); continue;
        }
        if (ids((unsigned char)c)) {
            size_t e = iend(s.src, s.len, p);
            Open o;
            if (word(s.src, p, e, "space") && space_open(&s, p, &o)) {
                if (s.in_space) {
                    report(&s, "space-nested", "nested space block is not allowed", s.line, s.col);
                } else {
                    s.in_space = 1; s.depth = 0;
                    free(s.prefix);
                    s.prefix = dupn(s.src + o.na, o.nb - o.na);
                    for (size_t i = p; i < o.end; ++i) ac(&s.out, ' ');
                    s.col += (unsigned)(o.end - p); s.pos = o.end;
                    continue;
                }
            }
            size_t q = e; hs(s.src, s.len, &q);
            if ((word(s.src,p,e,"struct") || word(s.src,p,e,"union")) &&
                q < s.len && s.src[q] == '{') { copy_struct(&s, p); continue; }
            if (word(s.src,p,e,"enum") && q < s.len && s.src[q] == '{') {
                do_enum(&s, p); continue;
            }
            Qual m;
            if (qualified(&s, p, &m)) { do_qual(&s, p, &m); continue; }
            do_ident(&s, p, e); continue;
        }
        if (c == '{' && s.in_space) { ++s.depth; ac(&s.out, c); ++s.pos; ++s.col; continue; }
        if (c == '}' && s.in_space) {
            if (s.depth > 0) { --s.depth; ac(&s.out, c); ++s.pos; ++s.col; continue; }
            ac(&s.out, ' '); ++s.pos; ++s.col;
            s.in_space = 0; free(s.prefix); s.prefix = 0;
            continue;
        }
        ac(&s.out, c); ++s.pos; ++s.col;
    }
    if (s.in_space) report(&s, "space-unclosed", "space block is not closed", s.line, s.col);
    free(s.prefix); free(s.pending); free(s.expect);
    for (size_t i = 0; i < s.ntypes; ++i) {
        free(s.types[i].name);
        for (size_t j = 0; j < s.types[i].n; ++j) {
            free(s.types[i].cs[j].name); free(s.types[i].cs[j].mapped);
        }
    }
    for (size_t i = 0; i < s.nconsts; ++i) { free(s.consts[i].name); free(s.consts[i].mapped); }
    for (size_t i = 0; i < s.nvars; ++i) { free(s.vars[i].name); free(s.vars[i].mapped); }
    if (out_len) *out_len = s.out.len;
    return s.out.data;
}

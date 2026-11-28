#define _POSIX_C_SOURCE 200809L
#include "space_rewrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* All variable-length tables grow on demand: no silent truncation on large
 * inputs or deep qualified chains. */
static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 8;
    while (c < need) c *= 2;
    void *q = realloc(p, c * elem);
    if (!q) return p;
    *cap = c;
    return q;
}
typedef struct { char *name, *mapped; } Pair;
typedef struct { char *name; Pair *cs; size_t n, cap; int is_enum; } Type;
typedef struct { char *owner, *name, *mapped; } Const;
/* One open `space` block: its fully composed prefix and the brace depth at
 * which its body lives. A stack allows nested spaces to compose prefixes. */
typedef struct { char *prefix; size_t depth; } Frame;
/* One active function: parameters shadow outer scopes, which stay visible. */
typedef struct {
    char **args; size_t nargs, capargs;
    size_t depth, frame;          /* body brace depth and space frame index */
    size_t pop_at;                /* `)` ending a prototype; 0 for a body */
} ArgScope;
/* Parts of `a::b::c` plus an optional `.member`. Kept in the scanner so long
 * chains reuse the same storage instead of allocating per token. */
typedef struct {
    size_t *a, *b; size_t n, capa, capb;
    size_t end, ma, mb;
} Qual;
typedef struct {
    const char *src; size_t len, pos; Buf out;
    unsigned line, col;
    int in_space;
    size_t depth;                 /* unmatched braces inside space blocks */
    Frame *frames; size_t nframes, capframes;
    ArgScope *scopes; size_t nscopes, capscopes;
    char **names; size_t nnames, capnames; /* fully rewritten names seen so far */
    char **macros; size_t nmacros, capmacros; /* macro names declared in spaces */
    char *pending, *expect;
    Qual qual;
    Type *types; size_t ntypes, captypes;
    Const *consts; size_t nconsts, capconsts;
    Pair *vars; size_t nvars, capvars;
    const space_options *opts;
    space_diag_fn diag; void *ctx;
} Sc;
static char *dupn(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}
static const char *cur_prefix(Sc *s) {
    return s->nframes ? s->frames[s->nframes - 1].prefix : "";
}
static int push_frame(Sc *s, char *prefix) {
    if (s->nframes == s->capframes) {
        size_t c = s->capframes ? s->capframes * 2 : 8;
        Frame *f = realloc(s->frames, c * sizeof *f);
        if (!f) { free(prefix); return 0; }
        s->frames = f; s->capframes = c;
    }
    s->frames[s->nframes].prefix = prefix;
    s->frames[s->nframes].depth = s->depth;
    ++s->nframes;
    return 1;
}
/* Do not prefix a name that already starts with any open space prefix. */
static int already_prefixed(Sc *s, const char *name, size_t n) {
    for (size_t i = 0; i < s->nframes; ++i) {
        const char *p = s->frames[i].prefix;
        size_t pn = strlen(p);
        if (n > pn && !memcmp(name, p, pn) && name[pn] == '_') return 1;
    }
    return 0;
}
/* Build the fully prefixed name of a declaration in the current space. */
static void compose(Sc *s, const char *name, size_t n, Buf *b) {
    if (s->in_space) {
        const char *pfx = cur_prefix(s);
        ap(b, pfx, strlen(pfx)); ac(b, '_');
    }
    ap(b, name, n);
}
/* Remember emitted full names so qualified chains can be resolved relative to
 * the innermost space that actually declares them. */
static int is_known(Sc *s, const char *name, size_t n) {
    if (!name || !n) return 0;
    for (size_t i = 0; i < s->nnames; ++i)
        if (strlen(s->names[i]) == n && !memcmp(s->names[i], name, n)) return 1;
    return 0;
}
static void remember(Sc *s, const char *name, size_t n) {
    if (!name || !n || is_known(s, name, n)) return;
    if (s->nnames == s->capnames) {
        size_t c = s->capnames ? s->capnames * 2 : 32;
        char **p = realloc(s->names, c * sizeof *p);
        if (!p) return;
        s->names = p; s->capnames = c;
    }
    s->names[s->nnames++] = dupn(name, n);
}
static int is_macro(Sc *s, const char *name, size_t n) {
    for (size_t i = 0; i < s->nmacros; ++i)
        if (strlen(s->macros[i]) == n && !memcmp(s->macros[i], name, n)) return 1;
    return 0;
}
static void remember_macro(Sc *s, const char *name, size_t n) {
    if (!name || !n || is_macro(s, name, n)) return;
    if (s->nmacros == s->capmacros) {
        size_t c = s->capmacros ? s->capmacros * 2 : 16;
        char **p = realloc(s->macros, c * sizeof *p);
        if (!p) return;
        s->macros = p; s->capmacros = c;
    }
    s->macros[s->nmacros++] = dupn(name, n);
}
static char *find_const(Sc *s, const char *owner, const char *name, size_t nl) {
    for (size_t i = 0; i < s->nconsts; ++i)
        if (!strcmp(s->consts[i].owner, owner) && strlen(s->consts[i].name) == nl &&
            !memcmp(s->consts[i].name, name, nl)) return s->consts[i].mapped;
    return 0;
}
/* True when the identifier follows a comma in a declaration list rather than
 * in a brace initializer (`x::e a = one, b;` vs `x::e v = { one, two };`). */
static int after_decl_comma(Sc *s, size_t p) {
    const char *x = s->src;
    size_t i = p;
    while (i && (x[i-1] == ' ' || x[i-1] == '\t' || x[i-1] == '\n' ||
                 x[i-1] == '\r')) --i;
    if (!i || x[i-1] != ',') return 0;
    int braces = 0;
    for (size_t j = i - 1; j-- > 0;) {
        char c = x[j];
        if (c == '}') ++braces;
        else if (c == '{') {
            if (braces) --braces;
            else return 0;
        } else if (!braces && (c == ';' || c == '=')) return 1;
    }
    return 1;
}
/* Constants of enums declared under `pfx` (e.g. owner `x_y_e` under `x_y`). */
static char *find_const_under(Sc *s, const char *pfx, const char *name, size_t nl) {
    size_t pl = strlen(pfx);
    char *found = 0;
    for (size_t i = 0; i < s->nconsts; ++i) {
        Const *c = &s->consts[i];
        if (strlen(c->name) != nl || memcmp(c->name, name, nl)) continue;
        if (pl && (strncmp(c->owner, pfx, pl) || c->owner[pl] != '_')) continue;
        if (found && strcmp(found, c->mapped)) return 0; /* ambiguous */
        found = c->mapped;
    }
    return found;
}
static Type *new_type(Sc *s) {
    s->types = grow(s->types, &s->captypes, s->ntypes + 1, sizeof *s->types);
    if (s->ntypes >= s->captypes) return 0;
    Type *t = &s->types[s->ntypes++];
    memset(t, 0, sizeof *t);
    return t;
}
static void add_const(Sc *s, const char *owner, const char *name, size_t nl,
                      const char *mapped) {
    s->consts = grow(s->consts, &s->capconsts, s->nconsts + 1, sizeof *s->consts);
    if (s->nconsts >= s->capconsts) return;
    Const *c = &s->consts[s->nconsts++];
    c->owner = dupn(owner ? owner : "", strlen(owner ? owner : ""));
    c->name = dupn(name, nl);
    c->mapped = dupn(mapped, strlen(mapped));
}
static void add_type(Sc *s, const char *name, size_t n, int is_enum) {
    if (!name || !n) return;
    Buf b = {0};
    compose(s, name, n, &b);
    if (!b.data) return;
    Type *t = new_type(s);
    if (!t) { free(b.data); return; }
    t->name = b.data; t->is_enum = is_enum;
    remember(s, t->name, strlen(t->name));
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
static Type *find_type_n(Sc *s, const char *name, size_t n) {
    for (size_t i = 0; i < s->ntypes; ++i)
        if (strlen(s->types[i].name) == n && !memcmp(s->types[i].name, name, n))
            return &s->types[i];
    return 0;
}
static Type *find_type(Sc *s, const char *n) {
    return find_type_n(s, n, strlen(n));
}
static char *find_pair(Pair *p, size_t n, const char *name, size_t nl) {
    /* Search newest first: an inner declaration must shadow an outer one. */
    for (size_t i = n; i > 0; --i)
        if (strlen(p[i - 1].name) == nl && !memcmp(p[i - 1].name, name, nl))
            return p[i - 1].mapped;
    return 0;
}
static void add_pair(Pair **p, size_t *n, size_t *cap, const char *a, size_t al,
                     const char *b) {
    *p = grow(*p, cap, *n + 1, sizeof **p);
    if (*n >= *cap) return;
    (*p)[*n].name = dupn(a, al);
    (*p)[*n].mapped = dupn(b, strlen(b));
    ++*n;
}
/* A declarator of a qualified type gets the current space prefix, just like
 * any other declaration in a space (parameters are the exception). Outside a
 * space the bare name is already the resolved spelling. */
static void add_decl_pair(Sc *s, size_t p, size_t n, const char *type) {
    if (!s->in_space) {
        add_pair(&s->vars, &s->nvars, &s->capvars, s->src + p, n, type);
        return;
    }
    Buf full = {0};
    compose(s, s->src + p, n, &full);
    add_pair(&s->vars, &s->nvars, &s->capvars, s->src + p, n,
             full.data ? full.data : "");
    free(full.data);
}
static char *find_arg(Sc *s, const char *name, size_t n) {
    for (size_t k = s->nscopes; k > 0; --k) {
        ArgScope *sc = &s->scopes[k - 1];
        for (size_t i = 0; i < sc->nargs; ++i)
            if (strlen(sc->args[i]) == n && !memcmp(sc->args[i], name, n)) return sc->args[i];
    }
    return 0;
}
static void push_scope(Sc *s) {
    if (s->nscopes == s->capscopes) {
        size_t c = s->capscopes ? s->capscopes * 2 : 8;
        ArgScope *p = realloc(s->scopes, c * sizeof *p);
        if (!p) return;
        s->scopes = p; s->capscopes = c;
    }
    ArgScope *sc = &s->scopes[s->nscopes++];
    memset(sc, 0, sizeof *sc);
    sc->depth = s->depth + 1;
    sc->frame = s->nframes;
    sc->pop_at = 0;
}
static void pop_scope(Sc *s) {
    if (!s->nscopes) return;
    ArgScope *sc = &s->scopes[--s->nscopes];
    for (size_t i = 0; i < sc->nargs; ++i) free(sc->args[i]);
    free(sc->args);
}
static void add_arg(Sc *s, const char *name, size_t n) {
    if (!s->nscopes) return;
    ArgScope *sc = &s->scopes[s->nscopes - 1];
    sc->args = grow(sc->args, &sc->capargs, sc->nargs + 1, sizeof *sc->args);
    if (sc->nargs >= sc->capargs) return;
    sc->args[sc->nargs++] = dupn(name, n);
}
static int space_comment(const char *x, size_t n, size_t p, size_t *end) {
    if (p + 1 < n && x[p] == '/' && x[p + 1] == '*') {
        size_t q = p + 2;
        while (q + 1 < n && !(x[q] == '*' && x[q + 1] == '/')) ++q;
        *end = q + 1 < n ? q + 2 : n;
        return 1;
    }
    if (p + 1 < n && x[p] == '/' && x[p + 1] == '/') {
        size_t q = p;
        while (q < n && x[q] != '\n') ++q;
        *end = q;
        return 1;
    }
    return 0;
}
static int skip_space_comments(const char *x, size_t n, size_t *p) {
    for (;;) {
        hs(x, n, p);
        if (*p < n && x[*p] == '\n') { ++*p; continue; }
        size_t end;
        if (space_comment(x, n, *p, &end)) { *p = end; continue; }
        return 1;
    }
}
static void skip_literal(const char *x, size_t n, size_t *p) {
    char q = x[(*p)++];
    while (*p < n && x[*p] != '\n') {
        if (x[*p] == '\\' && *p + 1 < n) { *p += 2; continue; }
        if (x[*p] == q) { ++*p; break; }
        ++*p;
    }
}
/* Whitespace, comments and `\\`-newline inside a preprocessor directive. */
static void skip_directive_space(const char *x, size_t n, size_t *p) {
    for (;;) {
        hs(x, n, p);
        if (*p + 1 < n && x[*p] == '\\' && x[*p + 1] == '\n') { *p += 2; continue; }
        if (*p + 2 < n && x[*p] == '\\' && x[*p + 1] == '\r' && x[*p + 2] == '\n') {
            *p += 3; continue;
        }
        size_t end;
        if (space_comment(x, n, *p, &end)) { *p = end; continue; }
        if (*p < n && x[*p] == '\n') { ++*p; continue; }
        return;
    }
}
/* Index of the `}` closing a `{`, ignoring comments and string literals. */
static size_t body_end(const char *x, size_t n, size_t open) {
    int d = 0;
    for (size_t i = open; i < n; ++i) {
        size_t end;
        if (space_comment(x, n, i, &end)) { i = end - 1; continue; }
        if (x[i] == '"' || x[i] == '\'') { skip_literal(x, n, &i); --i; continue; }
        if (x[i] == '{') ++d;
        else if (x[i] == '}' && --d == 0) return i;
    }
    return n;
}
static size_t match_paren(const char *x, size_t n, size_t p) {
    int d = 0;
    for (size_t i = p; i < n; ++i) {
        size_t end;
        if (space_comment(x, n, i, &end)) { i = end - 1; continue; }
        if (x[i] == '"' || x[i] == '\'') { skip_literal(x, n, &i); --i; continue; }
        if (x[i] == '(') ++d;
        else if (x[i] == ')') { if (--d == 0) return i; }
    }
    return n;
}
/* Collect the parameter names of one declarator, e.g. `int a, char *b` yields
 * `a` and `b`. The last non-keyword identifier of each comma-separated part
 * is the parameter; a `(*name)` group is handled as a function pointer. */
static void collect_params(Sc *s, size_t open, size_t close) {
    const char *x = s->src;
    size_t last = 0, last_end = 0, fp = 0, fp_end = 0;
    int d = 0;
    for (size_t i = open + 1; i < close; ++i) {
        size_t end;
        if (space_comment(x, close, i, &end)) { i = end - 1; continue; }
        char c = x[i];
        if (c == '"' || c == '\'') { skip_literal(x, close, &i); --i; continue; }
        if (c == '(') {
            size_t q = i + 1; hs(x, close, &q);
            if (q < close && x[q] == '*') {
                ++q; hs(x, close, &q);
                if (q < close && ids((unsigned char)x[q])) {
                    size_t e = iend(x, close, q);
                    size_t r = e; hs(x, close, &r);
                    if (r < close && x[r] == ')') { fp = q; fp_end = e; }
                }
            }
            ++d; continue;
        }
        if (c == '[' || c == '{') { ++d; continue; }
        if (c == ')' || c == ']' || c == '}') { if (d > 0) --d; continue; }
        if (c == ',' && d == 0) {
            if (last) add_arg(s, x + last, last_end - last);
            else if (fp) add_arg(s, x + fp, fp_end - fp);
            last = last_end = fp = fp_end = 0;
            continue;
        }
        if (ids((unsigned char)c) && d == 0) {
            size_t e = iend(x, close, i);
            if (!kw(x, i, e)) { last = i; last_end = e; }
            i = e - 1;
        }
    }
    if (last) add_arg(s, x + last, last_end - last);
    else if (fp) add_arg(s, x + fp, fp_end - fp);
}
/* Skip whitespace, comments and C23 `[[attributes]]` (possibly nested). */
static int skip_attributes(const char *x, size_t n, size_t *p) {
    for (;;) {
        if (!skip_space_comments(x, n, p)) return 0;
        if (*p + 1 >= n || x[*p] != '[' || x[*p + 1] != '[') return 1;
        int d = 0;
        size_t q = *p;
        for (; q < n; ++q) {
            size_t end;
            if (space_comment(x, n, q, &end)) { q = end - 1; continue; }
            if (x[q] == '"' || x[q] == '\'') { skip_literal(x, n, &q); --q; continue; }
            if (q + 1 < n && x[q] == '[' && x[q + 1] == '[') { ++d; ++q; continue; }
            if (q + 1 < n && x[q] == ']' && x[q + 1] == ']') {
                ++q;
                if (--d == 0) { *p = q + 1; break; }
            }
        }
        if (q >= n) return 0;
    }
}
static size_t skip_back_space_comments(const char *x, size_t i) {
    for (;;) {
        while (i > 0 && (x[i - 1] == ' ' || x[i - 1] == '\t' ||
                         x[i - 1] == '\r' || x[i - 1] == '\n')) --i;
        if (i >= 2 && x[i - 2] == '*' && x[i - 1] == '/') {
            size_t j = i - 2;
            while (j > 0 && !(x[j - 1] == '/' && x[j] == '*')) --j;
            if (j > 0) { i = j - 1; continue; }
        }
        return i;
    }
}
/* Identifier after `.` or `->` is a member name and must never be prefixed,
 * even when whitespace or comments separate the operator from the member. */
static int member_access_before(Sc *s, size_t p) {
    const char *x = s->src;
    size_t i = skip_back_space_comments(x, p);
    if (!i) return 0;
    if (x[i - 1] == '.') return 1;
    if (x[i - 1] == '>') {
        size_t j = skip_back_space_comments(x, i - 1);
        if (j && x[j - 1] == '-') return 1;
    }
    return 0;
}
/* A qualified type name sets `pending`; the next identifier consumes it only
 * when it can actually be a declarator, not an operand of an expression. */
static int declarator_after_pending(Sc *s, size_t p) {
    const char *x = s->src;
    size_t i = skip_back_space_comments(x, p);
    if (!i) return 0;
    char prev = x[i - 1];
    if (strchr("=+-/%!~&|^<>?:", prev)) return 0;
    if (prev == '.') return 0;
    if (prev == '>') {
        size_t j = skip_back_space_comments(x, i - 1);
        if (j && x[j - 1] == '-') return 0;
    }
    return 1;
}
static int type_kw(const char *x, size_t a, size_t b) {
    static const char *k[] = {"auto","char","const","double","enum","extern",
      "float","inline","int","long","register","restrict","short","signed",
      "static","struct","typedef","union","unsigned","void","volatile",
      "_Atomic","_Bool","_Complex","_Imaginary","_BitInt","alignas","alignof",
      "bool","constexpr","typeof","typeof_unqual",0};
    for (int i = 0; k[i]; ++i) if (word(x, a, b, k[i])) return 1;
    return 0;
}
/* True when this identifier is a declarator rather than a use. Used only to
 * decide whether an outer-prefixed spelling inside a nested space is a fresh
 * declaration that needs the current prefix composed onto it. */
static int decl_context(Sc *s, size_t p) {
    const char *x = s->src;
    size_t i = skip_back_space_comments(x, p);
    if (!i) return 0;
    char prev = x[i - 1];
    if (strchr("=+-/%!~&|^<>?:([.", prev)) return 0;
    size_t start = i;
    while (start > 0 && x[start - 1] != ';' && x[start - 1] != '{' &&
           x[start - 1] != '}') --start;
    int type_seen = 0;
    for (size_t q = start; q < i;) {
        size_t end;
        if (space_comment(x, i, q, &end)) { q = end; continue; }
        char c = x[q];
        if (c == '"' || c == '\'') { skip_literal(x, i, &q); continue; }
        if (ids((unsigned char)c)) {
            size_t e = iend(x, i, q);
            if (type_kw(x, q, e) || find_type_n(s, x + q, e - q)) type_seen = 1;
            q = e;
        } else ++q;
    }
    return type_seen;
}
/* Matches an enclosing (not the current) space prefix. */
static int outer_prefixed(Sc *s, const char *name, size_t n) {
    for (size_t i = 0; i + 1 < s->nframes; ++i) {
        const char *p = s->frames[i].prefix;
        size_t pn = strlen(p);
        if (n > pn && !memcmp(name, p, pn) && name[pn] == '_') return 1;
    }
    return 0;
}
static int span_has_line_comment(const char *x, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        size_t end;
        if (space_comment(x, to, i, &end)) {
            if (end > i + 1 && x[i + 1] == '/') return 1;
            i = end - 1;
        }
    }
    return 0;
}
/* Append only comments found in a source span; whitespace handling is left to
 * the caller so a skipped `::` does not introduce extra spaces. */
static void emit_comments_in_span(Sc *s, size_t from, size_t to) {
    const char *x = s->src;
    for (size_t i = from; i < to;) {
        size_t end;
        if (!space_comment(x, to, i, &end)) { ++i; continue; }
        ap(&s->out, x + i, end - i);
        for (size_t j = i; j < end; ++j)
            if (x[j] == '\n') { ++s->line; s->col = 1; } else ++s->col;
        i = end;
    }
}
/* First `{` directly belonging to a struct/union/enum specifier. A brace after
 * `=`, `,`, `)`, `]` or `;` belongs to an initializer or a later function
 * body, not to the type being declared. */
static size_t find_body_open(const char *x, size_t n, size_t p, size_t limit) {
    for (size_t i = p; i < limit; ++i) {
        size_t end;
        if (space_comment(x, limit, i, &end)) { i = end - 1; continue; }
        if (x[i] == '"' || x[i] == '\'') {
            size_t j = i; skip_literal(x, limit, &j); i = j ? j - 1 : i;
            continue;
        }
        if (x[i] == '[' && i + 1 < limit && x[i + 1] == '[') {
            size_t a = i;
            if (skip_attributes(x, limit, &a) && a > i) { i = a - 1; continue; }
        }
        if (x[i] == ';' || x[i] == '=' || x[i] == ',' ||
            x[i] == ')' || x[i] == ']' || x[i] == '}') return limit;
        if (x[i] == '{') return i;
    }
    (void)n;
    return limit;
}
static void copy_struct_ex(Sc *s, size_t p, int member_ctx);
static void process_struct_body(Sc *s, size_t from, size_t to);
/* Register parameters of definitions, prototypes and function-pointer
 * declarators so they stay unprefixed. Definition scopes end with the body;
 * declaration scopes end at their closing `)`. */
static void start_function(Sc *s, size_t after, size_t ns, size_t ne) {
    const char *x = s->src;
    /* `if (...) {` and other keyword-led blocks are not functions; their
     * condition identifiers must not be mistaken for parameters. */
    if (kw(x, ns, ne)) return;
    int decl = decl_context(s, ns) || s->pending != 0;
    size_t q = after;
    if (!skip_attributes(x, s->len, &q)) return;
    size_t open;
    if (q < s->len && x[q] == '(') {
        open = q;
    } else if (q < s->len && x[q] == ')') {
        /* `(*F)(int cb)`: the parameter list follows the declarator group. */
        size_t r = q + 1;
        if (!skip_attributes(x, s->len, &r) || r >= s->len || x[r] != '(') return;
        open = r;
    } else {
        return;
    }
    size_t close = match_paren(x, s->len, open);
    if (close >= s->len) return;
    size_t r = close + 1;
    int body = skip_attributes(x, s->len, &r) && r < s->len && x[r] == '{';
    /* A call such as `f(a);` is not a declaration; only a real body or a
     * declarator context activates a parameter scope. */
    if (!body && !decl) return;
    push_scope(s);
    if (!s->nscopes) return;
    ArgScope *sc = &s->scopes[s->nscopes - 1];
    if (!body) sc->pop_at = close;
    collect_params(s, open, close);
    free(s->pending); s->pending = 0;
}
typedef struct { size_t end, na, nb; } Open;
static int space_open(Sc *s, size_t p, Open *o) {
    const char *x = s->src;
    size_t q = p + 5;
    if (!skip_space_comments(x, s->len, &q) || q >= s->len ||
        !ids((unsigned char)x[q])) return 0;
    o->na = q;
    o->nb = iend(x, s->len, q);
    q = o->nb;
    if (!skip_space_comments(x, s->len, &q) || q >= s->len || x[q] != '{') return 0;
    o->end = q + 1;
    return 1;
}
/* `a::b::c` (optionally `.member`) — parts are joined with underscores. */
static void do_qual(Sc *s, size_t p, Qual *m, int global);
static void do_ident(Sc *s, size_t p, size_t e);
static int qual_push(Qual *m, size_t a, size_t b) {
    m->a = grow(m->a, &m->capa, m->n + 1, sizeof *m->a);
    if (m->n >= m->capa) return 0;
    m->b = grow(m->b, &m->capb, m->n + 1, sizeof *m->b);
    if (m->n >= m->capb) return 0;
    m->a[m->n] = a; m->b[m->n] = b; ++m->n;
    return 1;
}
static int qualified(Sc *s, size_t p, Qual *m) {
    const char *x = s->src;
    m->n = 0; m->ma = m->mb = 0;
    if (p >= s->len || !ids((unsigned char)x[p])) return 0;
    size_t e = iend(x, s->len, p);
    /* A keyword before `::` is not a qualifier: `return ::x` refers to the
     * global `x`, not to a member of `return`. */
    if (kw(x, p, e)) return 0;
    if (!qual_push(m, p, e)) return 0;
    size_t q = e, last_end = e;
    for (;;) {
        skip_space_comments(x, s->len, &q);
        if (q + 1 >= s->len || x[q] != ':' || x[q + 1] != ':') break;
        q += 2; skip_space_comments(x, s->len, &q);
        if (q >= s->len || !ids((unsigned char)x[q])) {
            report(s, "space-bad-form", "malformed qualified name", s->line, s->col);
            return 0;
        }
        size_t ne = iend(x, s->len, q);
        if (kw(x, q, ne)) {
            report(s, "space-bad-form", "keyword in qualified name", s->line, s->col);
            return 0;
        }
        if (!qual_push(m, q, ne)) return 0;
        q = last_end = ne;
    }
    if (m->n < 2) return 0;
    m->end = last_end;
    q = last_end; skip_space_comments(x, s->len, &q);
    if (q < s->len && x[q] == '.') {
        ++q; skip_space_comments(x, s->len, &q);
        if (q >= s->len || !ids((unsigned char)x[q])) {
            report(s, "space-bad-form", "malformed qualified member", s->line, s->col);
            return 0;
        }
        m->ma = q; m->mb = iend(x, s->len, q);
        m->end = m->mb;
        size_t r = m->mb; skip_space_comments(x, s->len, &r);
        if (r < s->len && x[r] == '.') {
            report(s, "space-bad-form", "only one qualified member is allowed", s->line, s->col);
            return 0;
        }
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
/* Replace a consumed construct with spaces, keeping newlines and diagnostics
 * line numbers intact. */
static void blank_span(Sc *s, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (s->src[i] == '\n') { ac(&s->out, '\n'); ++s->line; s->col = 1; }
        else { ac(&s->out, ' '); ++s->col; }
    }
    s->pos = to;
}
/* Start of the logical line: comments and line splices count as whitespace. */
static int at_bol(const char *x, size_t p) {
    size_t q = p;
    for (;;) {
        while (q > 0 && (x[q - 1] == ' ' || x[q - 1] == '\t' ||
                         x[q - 1] == '\r')) --q;
        if (q == 0 || x[q - 1] == '\n') return 1;
        if (q >= 2 && x[q - 2] == '\\' && x[q - 1] == '\n') { q -= 2; continue; }
        if (q >= 3 && x[q - 3] == '\\' && x[q - 2] == '\r' && x[q - 1] == '\n') {
            q -= 3; continue;
        }
        if (q >= 2 && x[q - 2] == '*' && x[q - 1] == '/') {
            size_t i = q - 2;
            while (i > 0 && !(x[i - 1] == '/' && x[i] == '*')) --i;
            if (i > 0) { q = i - 1; continue; }
        }
        return 0;
    }
}
/* End of a preprocessor logical line, including `\\`-continued lines and
 * newlines inside block comments. */
static size_t directive_end(Sc *s, size_t p) {
    const char *x = s->src;
    for (size_t i = p; i < s->len; ++i) {
        size_t ce;
        if (space_comment(x, s->len, i, &ce)) { i = ce - 1; continue; }
        if (x[i] != '\n') continue;
        size_t k = i;
        while (k > p && (x[k - 1] == ' ' || x[k - 1] == '\t' ||
                         x[k - 1] == '\r')) --k;
        if (k > p && x[k - 1] == '\\') continue;
        return i + 1;
    }
    return s->len;
}
/* Directives are opaque: copy every one verbatim. Macro names are recorded
 * only so their uses in ordinary code are left untouched as well. */
static void copy_directive(Sc *s, size_t p) {
    size_t end = directive_end(s, p);
    const char *x = s->src;
    size_t q = p + 1; skip_directive_space(x, end, &q);
    size_t we = iend(x, end, q);
    if (word(x, q, we, "define") || word(x, q, we, "undef")) {
        size_t na = we; skip_directive_space(x, end, &na);
        if (na < end && ids((unsigned char)x[na]))
            remember_macro(s, x + na, iend(x, end, na) - na);
    }
    copy_span(s, p, end);
}
static void emit_prefixed(Sc *s, size_t a, size_t b) {
    Buf full = {0};
    compose(s, s->src + a, b - a, &full);
    ap(&s->out, full.data ? full.data : "", full.len);
    s->col += (unsigned)full.len;
    remember(s, full.data, full.len);
    free(full.data);
}
/* Struct/union bodies are member scopes: their names stay as written, but
 * nested tags and enums are declarations and must be rewritten. */
static void copy_struct_ex(Sc *s, size_t p, int member_ctx) {
    const char *x = s->src;
    size_t q = find_body_open(x, s->len, p, s->len);
    if (q >= s->len) { copy_span(s, p, s->len); return; }
    size_t close = body_end(x, s->len, q);
    if (close >= s->len) close = s->len;
    size_t r = p;
    while (r < q && ids((unsigned char)x[r])) ++r;
    int tagged = 0;
    size_t ta = 0, tb = 0;
    if (skip_space_comments(x, q, &r) && r < q && ids((unsigned char)x[r])) {
        tagged = 1; ta = r; tb = iend(x, q, r);
        add_type(s, x + ta, tb - ta, 0);
    } else if (!member_ctx && close < s->len) {
        /* Anonymous type: the first declarator after `}` names it. */
        r = close + 1;
        if (skip_space_comments(x, s->len, &r) && r < s->len &&
            ids((unsigned char)x[r]))
            add_type(s, x + r, iend(x, s->len, r) - r, 0);
    }
    copy_span(s, p, tagged ? ta : q + 1);
    if (tagged) { emit_prefixed(s, ta, tb); copy_span(s, tb, q + 1); }
    process_struct_body(s, q + 1, close);
    if (close < s->len) copy_span(s, close, close + 1);
    else s->pos = s->len;
}
static void copy_struct(Sc *s, size_t p) { copy_struct_ex(s, p, 0); }
static void do_enum(Sc *s, size_t p) {
    const char *x = s->src;
    size_t q = p + 4;
    if (!skip_space_comments(x, s->len, &q)) return;
    size_t ta = 0, tb = 0;
    if (q < s->len && ids((unsigned char)x[q])) {
        size_t e = iend(x, s->len, q), r = e;
        /* C23 also allows `enum Tag : underlying_type { ... }`. */
        if (skip_space_comments(x, s->len, &r) &&
            (r < s->len && (x[r] == '{' || x[r] == ':'))) {
            ta = q; tb = e; q = r;
        }
    }
    size_t body = find_body_open(x, s->len, q, s->len);
    if (body >= s->len || x[body] != '{') return;
    size_t close = body_end(x, s->len, body);
    if (close >= s->len) close = s->len;
    size_t da = close + 1, db = da;
    if (close < s->len) {
        if (skip_space_comments(x, s->len, &da) && da < s->len &&
            ids((unsigned char)x[da])) db = iend(x, s->len, da);
    }
    Buf base = {0}, ep = {0};
    if (ta) compose(s, x + ta, tb - ta, &base);
    else if (db > da) compose(s, x + da, db - da, &base);
    else compose(s, "", 0, &base);
    if (base.data) { ap(&ep, base.data, base.len); ac(&ep, '_'); }
    copy_span(s, p, ta ? ta : body);
    if (ta) { emit_prefixed(s, ta, tb); copy_span(s, tb, body + 1); }
    else copy_span(s, body, body + 1);
    Type *t = 0;
    if (base.data) {
        size_t before = s->ntypes;
        if (ta) add_type(s, x + ta, tb - ta, 1);
        else if (db > da) add_type(s, x + da, db - da, 1);
        else {
            t = new_type(s);
            if (t) {
                t->name = dupn(base.data, base.len); t->is_enum = 1;
                remember(s, t->name, strlen(t->name));
            }
        }
        if (!t && s->ntypes > before) t = &s->types[s->ntypes - 1];
    }
    /* Enumerator initializers are ordinary expressions: only the enumerator
     * name itself gets prefixed; references to earlier constants and qualified
     * chains go through the normal resolution path. */
    char *save_pending = s->pending, *save_expect = s->expect;
    s->pending = 0; s->expect = 0;
    int want = 1, depth = 0;
    for (size_t i = body + 1; i < close;) {
        size_t ce;
        if (space_comment(x, close, i, &ce)) { copy_span(s, i, ce); i = ce; continue; }
        char c = x[i];
        if (c == '"' || c == '\'') {
            size_t j = i; skip_literal(x, close, &j);
            if (j > i) { copy_span(s, i, j); i = j; continue; }
        }
        Qual *m = &s->qual;
        if (!want && qualified(s, i, m)) { do_qual(s, i, m, 0); i = s->pos; continue; }
        if (ids((unsigned char)c)) {
            size_t e = iend(x, close, i);
            if (want && !kw(x, i, e)) {
                Buf full = {0};
                ap(&full, ep.data ? ep.data : "", ep.len);
                ap(&full, x + i, e - i);
                ap(&s->out, full.data ? full.data : "", full.len);
                s->col += (unsigned)full.len;
                if (t) add_pair(&t->cs, &t->n, &t->cap, x + i, e - i,
                                full.data ? full.data : "");
                add_const(s, base.data ? base.data : "", x + i, e - i,
                          full.data ? full.data : "");
                remember(s, full.data, full.len);
                free(full.data);
                want = 0; s->pos = e; i = e; continue;
            }
            do_ident(s, i, e); i = s->pos; continue;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') { if (depth) --depth; }
        else if (c == ',' && depth == 0) want = 1;
        ac(&s->out, c); ++s->col; ++i; s->pos = i;
    }
    if (close < s->len) copy_span(s, close, close + 1);
    else s->pos = s->len;
    free(s->pending); free(s->expect);
    s->pending = save_pending; s->expect = save_expect;
    free(base.data); free(ep.data);
}
static void process_struct_body(Sc *s, size_t from, size_t to) {
    const char *x = s->src;
    char *save_pending = s->pending, *save_expect = s->expect;
    s->pending = 0; s->expect = 0;
    for (size_t i = from; i < to;) {
        size_t ce;
        if (space_comment(x, to, i, &ce)) { copy_span(s, i, ce); i = ce; continue; }
        char c = x[i];
        if (c == '"' || c == '\'') {
            size_t j = i; skip_literal(x, to, &j);
            if (j > i) { copy_span(s, i, j); i = j; continue; }
        }
        Qual *m = &s->qual;
        if (qualified(s, i, m)) { do_qual(s, i, m, 0); i = s->pos; continue; }
        if (ids((unsigned char)c)) {
            size_t e = iend(x, to, i);
            size_t body = find_body_open(x, s->len, i, to);
            if (body < to && (word(x, i, e, "enum") || word(x, i, e, "struct") ||
                              word(x, i, e, "union"))) {
                if (word(x, i, e, "enum")) do_enum(s, i);
                else copy_struct_ex(s, i, 1);
                i = s->pos; continue;
            }
            /* Member names and ordinary member declarations are left alone. */
            copy_span(s, i, e); i = e; continue;
        }
        ac(&s->out, c); ++s->col; ++i; s->pos = i;
    }
    free(s->pending); free(s->expect);
    s->pending = save_pending; s->expect = save_expect;
}
/* A leading `::` forces the absolute name; otherwise try enclosing spaces
 * from innermost outward before falling back to the absolute chain. */
static void do_qual(Sc *s, size_t p, Qual *m, int global) {
    const char *x = s->src;
    Buf *b = &s->out;
    Buf joined = {0};
    for (size_t i = 0; i < m->n; ++i) {
        if (i) ac(&joined, '_');
        ap(&joined, x + m->a[i], m->b[i] - m->a[i]);
    }
    char *resolved = dupn(joined.data ? joined.data : "", joined.len);
    free(joined.data);
    if (!global && s->nframes && resolved) {
        for (size_t i = s->nframes + 1; i-- > 0;) {
            Buf cand = {0};
            if (i) {
                const char *pfx = s->frames[i - 1].prefix;
                ap(&cand, pfx, strlen(pfx)); ac(&cand, '_');
            }
            ap(&cand, resolved, strlen(resolved));
            if (is_known(s, cand.data, cand.len)) {
                free(resolved); resolved = cand.data; cand.data = 0;
                break;
            }
            free(cand.data);
        }
    }
    size_t rn = resolved ? strlen(resolved) : 0;
    size_t memlen = (m->mb > m->ma) ? 1 + (m->mb - m->ma) : 0;
    Type *t = rn ? find_type(s, resolved) : 0;
    if (t) { free(s->pending); s->pending = dupn(resolved, rn); }
    if (span_has_line_comment(x, p, m->end)) {
        /* A `//` comment must stay after the token, with its newline before
         * the rest of the statement, or it would comment out that statement. */
        ap(b, resolved ? resolved : "", rn); s->col += (unsigned)rn;
        remember(s, resolved, rn);
        if (m->mb > m->ma) {
            if (t && t->is_enum) ac(b, '_'); else ac(b, '.');
            ap(b, x + m->ma, m->mb - m->ma);
            s->col += (unsigned)memlen;
        }
        for (size_t k = p; k < m->end;) {
            size_t ce;
            if (space_comment(x, m->end, k, &ce)) {
                ap(&s->out, x + k, ce - k);
                for (size_t j = k; j < ce; ++j)
                    if (x[j] == '\n') { ++s->line; s->col = 1; } else ++s->col;
                k = ce;
            } else if (x[k] == '\n') {
                ac(&s->out, '\n'); ++s->line; s->col = 1; ++k;
            } else if (x[k] == ' ' || x[k] == '\t' || x[k] == '\r') {
                ac(&s->out, x[k]); ++s->col; ++k;
            } else ++k; /* identifier or `::` already emitted above */
        }
    } else {
        /* Emit newlines crossed before the token so the joined name stays on
         * its original line; block comments can safely follow the token. */
        int had_nl = 0;
        for (size_t k = p; k < m->end;) {
            size_t ce;
            if (space_comment(x, m->end, k, &ce)) { k = ce; continue; }
            if (x[k] == '\n') { ac(&s->out, '\n'); ++s->line; s->col = 1; had_nl = 1; }
            ++k;
        }
        ap(b, resolved ? resolved : "", rn);
        remember(s, resolved, rn);
        if (m->mb > m->ma) {
            if (t && t->is_enum) ac(b, '_'); else ac(b, '.');
            ap(b, x + m->ma, m->mb - m->ma);
        }
        for (size_t k = p; k < m->end;) {
            size_t ce;
            if (!space_comment(x, m->end, k, &ce)) { ++k; continue; }
            ap(&s->out, x + k, ce - k);
            for (size_t j = k; j < ce; ++j)
                if (x[j] == '\n') { ++s->line; s->col = 1; had_nl = 1; }
            k = ce;
        }
        s->col += had_nl ? (unsigned)(rn + memlen) : (unsigned)(m->end - p);
    }
    free(resolved);
    size_t r = m->end; hs(x, s->len, &r);
    if (r + 1 < s->len && x[r] == ':' && x[r+1] == ':')
        report(s, "space-bad-form", "malformed qualified name", s->line, s->col);
    s->pos = m->end;
}
static void do_ident(Sc *s, size_t p, size_t e) {
    const char *x = s->src;
    size_t n = e - p;

    /* Members are selected by `.`/`->`; they live in the struct's namespace,
     * not the enclosing space's, so they are emitted exactly as written. */
    if (member_access_before(s, p)) {
        ap(&s->out, x + p, n);
        s->pos = e; s->col += (unsigned)n;
        return;
    }
    /* Macros are preprocessing tokens: never prefix or otherwise rewrite a
     * name that the file defines or undefines with `#define`/`#undef`. */
    if (is_macro(s, x + p, n)) {
        ap(&s->out, x + p, n);
        s->pos = e; s->col += (unsigned)n;
        return;
    }
    if (find_arg(s, x + p, n)) {
        ap(&s->out, x + p, n);
        s->pos = e; s->col += (unsigned)n;
        return;
    }
    int is_decl = decl_context(s, p);
    int added_typed = 0;
    char *var_type = find_pair(s->vars, s->nvars, x + p, n);
    int had_var = var_type != 0;
    if (s->pending && !declarator_after_pending(s, p)) {
        free(s->pending); s->pending = 0;
    }
    if (s->pending) {
        add_decl_pair(s, p, n, s->pending);
        var_type = find_pair(s->vars, s->nvars, x+p, n);
        free(s->expect); s->expect = dupn(s->pending, strlen(s->pending));
        free(s->pending); s->pending = 0;
        added_typed = 1;
    } else if (var_type) {
        free(s->expect); s->expect = dupn(var_type, strlen(var_type));
    }
    /* A typed declarator (`x::e a = ..., b = ...`) must not be rewritten as
     * an enum constant of the same name. Plain declarators (`int b;`) are not
     * constant uses either, even if a constant with that name is visible. */
    if (!added_typed && !had_var && s->expect) {
        size_t q = e; skip_space_comments(s->src, s->len, &q);
        if ((q < s->len && s->src[q] == '=') || after_decl_comma(s, p)) {
            add_decl_pair(s, p, n, s->expect);
            var_type = find_pair(s->vars, s->nvars, x+p, n);
            added_typed = 1;
        }
    }
    int local = var_type != 0 || is_decl;
    char *mapped = 0;
    if (!local) {
        if (s->expect) {
            Type *t = find_type(s, s->expect);
            if (t) mapped = find_pair(t->cs, t->n, x+p, n);
        }
        if (!mapped) {
            for (size_t i = s->nframes + 1; i-- > 0;) {
                const char *owner = i ? s->frames[i - 1].prefix : "";
                mapped = find_const(s, owner, x + p, n);
                if (!mapped) mapped = find_const_under(s, owner, x + p, n);
                if (mapped) break;
            }
        }
        if (!mapped && s->in_space) {
            /* Last resort: a bare constant name unique within a space. */
            for (size_t i = 0; i < s->nconsts; ++i) {
                if (strlen(s->consts[i].name) != n || memcmp(s->consts[i].name, x + p, n)) continue;
                if (mapped && strcmp(mapped, s->consts[i].mapped)) { mapped = 0; break; }
                mapped = s->consts[i].mapped;
            }
        }
    }
    if (mapped) {
        ap(&s->out, mapped, strlen(mapped));
        remember(s, mapped, strlen(mapped));
        s->pos = e; s->col += (unsigned)n; return;
    }
    int pf = s->in_space && !kw(x,p,e) && x[p] != '_';
    if (pf && already_prefixed(s, x+p, n))
        pf = outer_prefixed(s, x+p, n) && is_decl;
    Buf full = {0};
    if (pf) {
        compose(s, x + p, n, &full);
        ap(&s->out, full.data ? full.data : "", full.len);
        s->col += (unsigned)full.len;
        remember(s, full.data, full.len);
    } else {
        ap(&s->out, x+p, n);
        s->col += (unsigned)n;
    }
    /* Remember ordinary declarations so later uses are not mistaken for
     * enum constants with the same spelling. */
    if (is_decl && !added_typed && !kw(x, p, e))
        add_pair(&s->vars, &s->nvars, &s->capvars, x + p, n,
                 pf ? (full.data ? full.data : "") : (x + p));
    free(full.data);
    s->pos = e;
}
char *space_rewrite(const char *text, size_t len, const space_options *opts,
                    space_diag_fn diag, void *ctx, size_t *out_len) {
    Sc s; memset(&s, 0, sizeof s);
    s.src = text; s.len = len; s.line = 1; s.col = 1;
    s.opts = opts; s.diag = diag; s.ctx = ctx;
    ap(&s.out, "", 0); /* always return a valid buffer, even for empty input */
    if (opts) {
        for (size_t i = 0; i < opts->ndefines && opts->defines && opts->defines[i]; ++i)
            remember_macro(&s, opts->defines[i], strlen(opts->defines[i]));
    }
    while (s.pos < s.len) {
        size_t p = s.pos;
        char c = s.src[p];
        /* Prototype/function-pointer parameter scopes end at their `)`. */
        while (s.nscopes && s.scopes[s.nscopes - 1].pop_at &&
               p == s.scopes[s.nscopes - 1].pop_at) pop_scope(&s);
        /* A qualified type name that was not followed by a declarator must not
         * leak into the next expression identifier. */
        if (c == ')' || c == ']' || c == ',' || c == ';') {
            free(s.pending); s.pending = 0;
        }
        if (c == '\n') { ac(&s.out, c); ++s.pos; ++s.line; s.col = 1; continue; }
        if (c == '#' && at_bol(s.src, p)) { copy_directive(&s, p); continue; }
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
        /* Attributes are opaque: names inside `[[...]]` are not rewritten. */
        if (c == '[' && p + 1 < s.len && s.src[p + 1] == '[') {
            size_t q = p;
            if (skip_attributes(s.src, s.len, &q) && q > p) { copy_span(&s, p, q); continue; }
        }
        /* A leading `::` denotes the global scope. Drop it and resolve the
         * following name as written, regardless of the surrounding space. */
        if (c == ':' && p + 1 < s.len && s.src[p + 1] == ':') {
            size_t q = p + 2, ws = q;
            skip_space_comments(s.src, s.len, &q);
            if (q < s.len && ids((unsigned char)s.src[q])) {
                Qual *m = &s.qual;
                int crossed_line = 0;
                for (size_t k = ws; k < q; ++k)
                    if (s.src[k] == '\n') crossed_line = 1;
                if (crossed_line) copy_span(&s, ws, q);
                else { s.col += (unsigned)(q - p); s.pos = q; }
                if (qualified(&s, q, m)) {
                    do_qual(&s, q, m, 1);
                    if (s.in_space)
                        start_function(&s, m->end, m->a[m->n - 1], m->b[m->n - 1]);
                } else {
                    size_t e = iend(s.src, s.len, q);
                    copy_span(&s, q, e);
                    if (s.in_space) start_function(&s, e, q, e);
                }
                if (!crossed_line) emit_comments_in_span(&s, ws, q);
                continue;
            }
        }
        if (ids((unsigned char)c)) {
            size_t e = iend(s.src, s.len, p);
            Open o;
            if (word(s.src, p, e, "space") && space_open(&s, p, &o)) {
                Buf prefix = {0};
                if (s.in_space) {
                    const char *parent = cur_prefix(&s);
                    ap(&prefix, parent, strlen(parent)); ac(&prefix, '_');
                }
                ap(&prefix, s.src + o.na, o.nb - o.na);
                if (push_frame(&s, prefix.data)) {
                    s.in_space = 1;
                    blank_span(&s, p, o.end);
                    continue;
                }
            } else if (word(s.src, p, e, "space")) {
                size_t q = e;
                if (skip_space_comments(s.src, s.len, &q) && q < s.len && s.src[q] == '{')
                    report(&s, "space-bad-form", "missing space name", s.line, s.col);
            }
            /* A body can follow a tag, a fixed underlying type, or comments. */
            size_t body = find_body_open(s.src, s.len, e, s.len);
            int has_body = body < s.len && s.src[body] == '{';
            if ((word(s.src,p,e,"struct") || word(s.src,p,e,"union")) && has_body) {
                copy_struct(&s, p); continue;
            }
            if (word(s.src,p,e,"enum") && has_body) {
                do_enum(&s, p); continue;
            }
            Qual *m = &s.qual;
            if (qualified(&s, p, m)) {
                do_qual(&s, p, m, 0);
                if (s.in_space)
                    start_function(&s, m->end, m->a[m->n - 1], m->b[m->n - 1]);
                continue;
            }
            if (s.in_space) start_function(&s, e, p, e);
            do_ident(&s, p, e); continue;
        }
        if (c == '{' && s.in_space) {
            free(s.expect); s.expect = 0;
            free(s.pending); s.pending = 0;
            ++s.depth; ac(&s.out, c); ++s.pos; ++s.col; continue;
        }
        if (c == '}' && s.in_space) {
            free(s.expect); s.expect = 0;
            free(s.pending); s.pending = 0;
            Frame *top = &s.frames[s.nframes - 1];
            if (s.depth > top->depth) {
                if (s.nscopes) {
                    ArgScope *fn = &s.scopes[s.nscopes - 1];
                    if (s.depth == fn->depth && s.nframes == fn->frame) pop_scope(&s);
                }
                --s.depth; ac(&s.out, c); ++s.pos; ++s.col; continue;
            }
            /* The brace at the frame's opening depth ends this space block. */
            blank_span(&s, p, p + 1);
            free(top->prefix); --s.nframes;
            if (!s.nframes) s.in_space = 0;
            continue;
        }
        if (c == ';') { free(s.expect); s.expect = 0; }
        ac(&s.out, c); ++s.pos; ++s.col;
    }
    if (s.in_space) report(&s, "space-unclosed", "space block is not closed", s.line, s.col);
    while (s.nscopes) pop_scope(&s);
    free(s.scopes);
    for (size_t i = 0; i < s.nframes; ++i) free(s.frames[i].prefix);
    free(s.frames); free(s.pending); free(s.expect);
    free(s.qual.a); free(s.qual.b);
    for (size_t i = 0; i < s.nnames; ++i) free(s.names[i]);
    free(s.names);
    for (size_t i = 0; i < s.nmacros; ++i) free(s.macros[i]);
    free(s.macros);
    for (size_t i = 0; i < s.ntypes; ++i) {
        free(s.types[i].name);
        for (size_t j = 0; j < s.types[i].n; ++j) {
            free(s.types[i].cs[j].name); free(s.types[i].cs[j].mapped);
        }
        free(s.types[i].cs);
    }
    free(s.types);
    for (size_t i = 0; i < s.nconsts; ++i) {
        free(s.consts[i].owner); free(s.consts[i].name); free(s.consts[i].mapped);
    }
    for (size_t i = 0; i < s.nvars; ++i) { free(s.vars[i].name); free(s.vars[i].mapped); }
    free(s.vars); free(s.consts);
    if (out_len) *out_len = s.out.len;
    return s.out.data;
}

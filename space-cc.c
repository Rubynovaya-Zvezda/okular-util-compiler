#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "space-cc.h"
#include "space_rewrite.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char **dirs;
    size_t ndirs;
    size_t capdirs;
    size_t errors;
} HookCtx;

typedef struct {
    char *temp;
    char *orig;
} Mapping;

static void print_diag(void *ctx, const char *file, unsigned line, unsigned col,
                       const char *id, const char *msg) {
    HookCtx *h = ctx;
    ++h->errors;
    fprintf(stderr, "%s:%u:%u: error: %s: %s\n", file, line, col, id, msg);
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int takes_next(const char *a) {
    static const char *const opts[] = {
        "-o", "-I", "-D", "-U", "-include", "-imacros", "-isystem",
        "-iquote", "-idirafter", "-MF", "-MT", "-MQ", "-x", "-target",
        "--target", "-arch", "-Xclang", "-Xlinker", "-Xpreprocessor",
        "-B", "-L", "--sysroot", "-fmodule-map-file", NULL
    };
    for (size_t i = 0; opts[i]; ++i) {
        if (strcmp(a, opts[i]) == 0) return 1;
    }
    return 0;
}

static int looks_like_source(const char *a) {
    return a[0] != '-' &&
           (ends_with(a, ".c") || ends_with(a, ".h") || ends_with(a, ".i"));
}

static int path_prefix_is(const char *path, const char *root) {
    size_t n = strlen(root);
    if (strncmp(path, root, n) != 0) return 0;
    return n == 1 || path[n] == '/' || path[n] == '\0';
}

static int in_allowed_root(const char *path) {
    char abs[PATH_MAX];
    if (!realpath(path, abs)) return 0;
    char *roots = getenv("SPACE_HOOK_ROOTS");
    if (!roots || !*roots) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        return path_prefix_is(abs, cwd);
    }
    char *copy = strdup(roots);
    if (!copy) return 0;
    int ok = 0;
    for (char *p = copy, *save = NULL; ; ) {
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            save = colon + 1;
        }
        char root[PATH_MAX];
        if (realpath(p, root) && path_prefix_is(abs, root)) {
            ok = 1;
            break;
        }
        if (!colon) break;
        p = save;
    }
    free(copy);
    return ok;
}

static void add_dir(HookCtx *h, char *dir) {
    for (size_t i = 0; i < h->ndirs; ++i) {
        if (strcmp(h->dirs[i], dir) == 0) {
            free(dir);
            return;
        }
    }
    if (h->ndirs == h->capdirs) {
        size_t cap = h->capdirs ? h->capdirs * 2 : 8;
        char **nd = realloc(h->dirs, cap * sizeof *nd);
        if (!nd) {
            free(dir);
            return;
        }
        h->dirs = nd;
        h->capdirs = cap;
    }
    h->dirs[h->ndirs++] = dir;
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        if (n + 1 >= cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        size_t got = fread(buf + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0) break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static void write_line_marker(FILE *f, const char *path) {
    fputs("#line 1 \"", f);
    for (const char *p = path; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputs("\"\n", f);
}

static char *absolute_dir(const char *path) {
    char abs[PATH_MAX];
    if (!realpath(path, abs)) return strdup(".");
    char *slash = strrchr(abs, '/');
    if (!slash) return strdup(".");
    *slash = '\0';
    return strdup(abs);
}

/* Creates a rewritten copy in a fresh temp dir. Returns its path, or NULL. */
static char *make_temp_copy(const char *orig, HookCtx *h, char **orig_dir) {
    const char *tmpbase = getenv("TMPDIR");
    if (!tmpbase || !*tmpbase) tmpbase = "/tmp";
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/space-hook-XXXXXX", tmpbase) >= (int)sizeof dir)
        return NULL;
    if (!mkdtemp(dir)) return NULL;
    add_dir(h, strdup(dir));

    const char *base = strrchr(orig, '/');
    base = base ? base + 1 : orig;
    char *temp = malloc(strlen(dir) + strlen(base) + 2);
    if (!temp) return NULL;
    snprintf(temp, strlen(dir) + strlen(base) + 2, "%s/%s", dir, base);

    size_t len = 0;
    char *text = read_all(orig, &len);
    if (!text) {
        free(temp);
        return NULL;
    }
    space_options opts = {0};
    opts.filename = orig;
    size_t out_len = 0;
    char *out = space_rewrite(text, len, &opts, print_diag, h, &out_len);
    free(text);
    if (!out) {
        free(temp);
        return NULL;
    }

    FILE *f = fopen(temp, "wb");
    if (!f) {
        perror(temp);
        free(out);
        free(temp);
        return NULL;
    }
    write_line_marker(f, orig);
    if (out_len && fwrite(out, 1, out_len, f) != out_len) perror(temp);
    if (fclose(f) != 0) perror(temp);
    free(out);

    *orig_dir = absolute_dir(orig);
    return temp;
}

static char *replace_all(const char *src, const char *from, const char *to) {
    size_t nf = strlen(from), nt = strlen(to);
    size_t cap = strlen(src) + 1, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    const char *p = src;
    for (;;) {
        const char *hit = strstr(p, from);
        if (!hit) {
            size_t rest = strlen(p);
            if (n + rest + 1 > cap) {
                char *no = realloc(out, n + rest + 1);
                if (!no) { free(out); return NULL; }
                out = no;
                cap = n + rest + 1;
            }
            memcpy(out + n, p, rest);
            n += rest;
            break;
        }
        size_t head = (size_t)(hit - p);
        if (n + head + nt + 1 > cap) {
            size_t need = n + head + nt + 1;
            char *no = realloc(out, need);
            if (!no) { free(out); return NULL; }
            out = no;
            cap = need;
        }
        memcpy(out + n, p, head);
        n += head;
        memcpy(out + n, to, nt);
        n += nt;
        p = hit + nf;
    }
    out[n] = '\0';
    return out;
}

static void fix_depfile(const char *path, Mapping *maps, size_t nmaps) {
    size_t len = 0;
    char *text = read_all(path, &len);
    if (!text) return;
    char *cur = text;
    for (size_t i = 0; i < nmaps; ++i) {
        char *next = replace_all(cur, maps[i].temp, maps[i].orig);
        if (!next) break;
        if (cur != text) free(cur);
        cur = next;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(cur, 1, strlen(cur), f);
        fclose(f);
    }
    if (cur != text) free(cur);
    free(text);
}

static void cleanup(HookCtx *h, Mapping *maps, size_t nmaps) {
    for (size_t i = 0; i < nmaps; ++i) {
        unlink(maps[i].temp);
        free(maps[i].temp);
        free(maps[i].orig);
    }
    for (size_t i = 0; i < h->ndirs; ++i) {
        rmdir(h->dirs[i]);
        free(h->dirs[i]);
    }
    free(h->dirs);
}

int space_cc_main(int argc, char **argv) {
    const char *real = getenv("SPACE_CC_REAL");
    if (!real || !*real) real = "clang";

    HookCtx h;
    memset(&h, 0, sizeof h);

    size_t maxargs = (size_t)argc * 3 + 16;
    char **args = calloc(maxargs, sizeof *args);
    Mapping *maps = calloc((size_t)argc + 1, sizeof *maps);
    char **iquotes = calloc((size_t)argc + 1, sizeof *iquotes);
    if (!args || !maps || !iquotes) {
        fprintf(stderr, "okc compile: out of memory\n");
        return 1;
    }
    size_t nargs = 0, nmaps = 0, niquotes = 0;
    const char *depfile = NULL;
    args[nargs++] = argv[0];

    for (int i = 1; i < argc; ++i) {
        if (takes_next(argv[i]) && i + 1 < argc) {
            if (strcmp(argv[i], "-MF") == 0) depfile = argv[i + 1];
            args[nargs++] = argv[i];
            args[nargs++] = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "-MF", 3) == 0 && argv[i][3]) depfile = argv[i] + 3;
        if (argv[i][0] == '-') {
            args[nargs++] = argv[i];
            continue;
        }
        if (looks_like_source(argv[i])) {
            struct stat st;
            if (stat(argv[i], &st) == 0 && S_ISREG(st.st_mode) &&
                in_allowed_root(argv[i])) {
                char *dir = NULL;
                char *temp = make_temp_copy(argv[i], &h, &dir);
                if (temp) {
                    maps[nmaps].temp = temp;
                    maps[nmaps].orig = strdup(argv[i]);
                    ++nmaps;
                    int seen = 0;
                    for (size_t j = 0; j < niquotes; ++j) {
                        if (strcmp(iquotes[j], dir) == 0) seen = 1;
                    }
                    if (!seen) iquotes[niquotes++] = dir;
                    args[nargs++] = temp;
                    continue;
                }
                free(dir);
            }
        }
        args[nargs++] = argv[i];
    }
    for (size_t i = 0; i < niquotes; ++i) {
        args[nargs++] = (char *)"-iquote";
        args[nargs++] = iquotes[i];
    }
    args[nargs] = NULL;

    if (h.errors) {
        cleanup(&h, maps, nmaps);
        free(args);
        free(maps);
        for (size_t i = 0; i < niquotes; ++i) free(iquotes[i]);
        free(iquotes);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("okc compile: fork");
        cleanup(&h, maps, nmaps);
        free(args);
        free(maps);
        for (size_t i = 0; i < niquotes; ++i) free(iquotes[i]);
        free(iquotes);
        return 1;
    }
    if (pid == 0) {
        execvp(real, args);
        perror(real);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("okc compile: waitpid");
        status = 0;
    }
    if (depfile && nmaps && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        fix_depfile(depfile, maps, nmaps);

    cleanup(&h, maps, nmaps);
    free(args);
    free(maps);
    for (size_t i = 0; i < niquotes; ++i) free(iquotes[i]);
    free(iquotes);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

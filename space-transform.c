#include "space-transform.h"
#include "space_rewrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int errors;
} DiagState;

static void print_diag(void *ctx, const char *file, unsigned line, unsigned col,
                       const char *id, const char *msg) {
    DiagState *st = ctx;
    ++st->errors;
    fprintf(stderr, "%s:%u:%u: error: %s: %s\n", file, line, col, id, msg);
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
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

static void usage(void) {
    fprintf(stderr, "usage: okc prepare [-o FILE] [--] FILE\n");
}

int space_transform_main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) input = argv[++i];
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            input = argv[i];
        }
    }
    if (!input) {
        usage();
        return 2;
    }

    size_t len = 0;
    char *text = read_all(input, &len);
    if (!text) return 1;

    space_options opts = {0};
    opts.filename = input;
    DiagState st = {0};
    size_t out_len = 0;
    char *out = space_rewrite(text, len, &opts, print_diag, &st, &out_len);
    free(text);

    if (!out) {
        fprintf(stderr, "okc prepare: out of memory\n");
        return 1;
    }
    if (st.errors) {
        free(out);
        return 1;
    }

    if (output) {
        FILE *f = fopen(output, "wb");
        if (!f) {
            perror(output);
            free(out);
            return 1;
        }
        if (fwrite(out, 1, out_len, f) != out_len) {
            perror(output);
            fclose(f);
            free(out);
            return 1;
        }
        if (fclose(f) != 0) {
            perror(output);
            free(out);
            return 1;
        }
    } else {
        if (fwrite(out, 1, out_len, stdout) != out_len) {
            perror("stdout");
            free(out);
            return 1;
        }
    }
    free(out);
    return 0;
}

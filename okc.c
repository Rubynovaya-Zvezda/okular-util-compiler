#include "space-cc.h"
#include "space-transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    fprintf(stderr,
            "usage: okc compile <clang args...>\n"
            "       okc prepare [-o FILE] [--] FILE\n"
            "       okc <clang args...>   (same as 'compile', for CC=okc)\n");
}

/* The subcommand implementations expect argv[0] to be the program name and
 * options to start at argv[1], so rebuild the vector after dropping argv[1]. */
static int run_subcommand(int (*fn)(int, char **), int argc, char **argv) {
    char **sub = malloc((size_t)argc * sizeof *sub);
    if (!sub) {
        fprintf(stderr, "okc: out of memory\n");
        return 1;
    }
    sub[0] = argv[0];
    for (int i = 2; i < argc; ++i) sub[i - 1] = argv[i];
    int rc = fn(argc - 1, sub);
    free(sub);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "prepare") == 0)
        return run_subcommand(space_transform_main, argc, argv);
    if (strcmp(argv[1], "compile") == 0)
        return run_subcommand(space_cc_main, argc, argv);
    return space_cc_main(argc, argv);
}

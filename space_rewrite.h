#ifndef SPACE_REWRITE_H
#define SPACE_REWRITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostic callback. `id` is one of:
 *   space-unclosed, space-bad-form
 */
typedef void (*space_diag_fn)(void *ctx, const char *file, unsigned line,
                              unsigned col, const char *id, const char *msg);

typedef struct {
    const char *filename;  /* used in diagnostics */
    const char *const *defines; /* macro names from -D/-U, as written */
    size_t ndefines;
} space_options;

/* Returns a malloc'd, NUL-terminated rewritten buffer (caller frees), or NULL
 * on out-of-memory. Diagnostics are reported through `diag`; the caller must
 * count them to decide whether to abort the build.
 */
char *space_rewrite(const char *text, size_t len, const space_options *opts,
                    space_diag_fn diag, void *diag_ctx, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif

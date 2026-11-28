# space hook usage

The hook rewrites C23 source text **before clang's preprocessor runs**, then calls
real clang. Your sources, headers and libraries are not modified.

Syntax:

- `space z { ... }` — declarations inside the block get the `z_` prefix. No nesting.
- `z::item` → `z_item`; `z::item.member` → `z_item_member`. Only two-part forms.
  Qualified names resolve the same way inside and outside a `space` block.
- `::item` → `item` (global scope), including inside a `space` block.
- Parameters of function definitions keep their names; space-scope globals
  used in the function body still get the prefix. Newlines, comments and
  `[[attributes]]` between the signature and `{` are tolerated; attribute
  contents are left untouched.

Example:

```c
space x {
 bool v = false;
 int funct(int a, int b) { v = 0>a+b; return v; }
}
/* becomes */
bool x_v = false;
int x_funct(int a, int b) { x_v = 0>a+b; return x_v; }
```

The hook is the Bazel module at `/home/danila/project/okular/compiler`.

## Build the hook

```sh
cd /home/danila/project/okular/compiler
bazel build //... && bazel test //...       # produces bazel-bin/okc
HOOK="$PWD/bazel-bin/okc"

# Or, without Bazel:
make && HOOK="$PWD/okc"
```

One binary, `okc`, with two subcommands. `SPACE_CC_REAL=clang-19` selects the
underlying compiler.

## Compile with the hook

```sh
"$HOOK" compile -std=c23 -c main.c -o main.o
"$HOOK" compile -std=c23 main.c -o app
```

When used as a drop-in `CC` (no subcommand), `okc` defaults to compile mode.

## Prepare a source without compiling

```sh
"$HOOK" prepare main.c > main.hooked.c
"$HOOK" prepare -o main.hooked.c main.c
```

## Connect the hook to a Bazel build (no `.bazelrc` edits)

```sh
HOOK=/home/danila/project/okular/compiler/bazel-bin/okc
bazel build ... --action_env=CC="$HOOK" --repo_env=CC="$HOOK"
bazel test  ... --action_env=CC="$HOOK" --repo_env=CC="$HOOK"
```

Also pass `--action_env=SPACE_CC_REAL=clang-19 --repo_env=SPACE_CC_REAL=clang-19`
to select another compiler.

## Connect to another build system

```sh
export CC=/abs/path/to/compiler/bazel-bin/okc
make
cmake -S . -B build -DCMAKE_C_COMPILER=/abs/path/to/compiler/bazel-bin/okc
```

Or add to that project's `.bazelrc`:

```
build --action_env=CC=/abs/path/to/compiler/bazel-bin/okc
build --repo_env=CC=/abs/path/to/compiler/bazel-bin/okc
```

The module `okular-compiler` can also be used with `bazel_dep` and a
`local_path_override`; the tool target is `@okular-compiler//:okc`.

## Semantics

```c
space a {
  int a;                              -> int a_a;
  struct { int one; int two; } b;     -> struct { int one; int two; } a_b;
}
space x {
  enum { one, two } c;                -> enum { x_c_one, x_c_two } x_c;
}
x::c.one                              -> x_c_one
x::c enum_variable = one;             -> x_c enum_variable = x_c_one;
enum_variable = two;                  -> enum_variable = x_c_two;
```

Struct/union members are not prefixed — they already have namespace isolation.
Enum constants get their owner's prefix, and a variable declared with type `x::c`
resolves bare constants of that type (e.g. `one` → `x_c_one`).

Errors: `space-nested` (nested `space` block), `space-unclosed` (missing `}`),
`space-bad-form` (malformed or long qualified name). Format:
`file.c:7:1: error: space-nested: ...`.

## Limitations

- `@response-file` arguments are not expanded in v1.
- Names produced by macro expansion are not rewritten.
- `#pragma` payloads and `#include` operands are not rewritten.
- Only files under `SPACE_HOOK_ROOTS` (default: cwd) are rewritten.
- No allowlist/data files: library names inside a `space` block are prefixed like
  any other identifier; outside a block they are untouched. Copy only `okc` to a
  machine with clang and it works.

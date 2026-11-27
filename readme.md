# okular-compiler

Bazel module with a C23 namespace hook:

- `space z { ... }` prefixes declarations in the block with `z_`;
- `z::item` → `z_item`, and `z::item.member` → `z_item_member`.

Struct/union members keep their names (they are already isolated by the struct).
Enum constants are prefixed with their owner, e.g. `space x { enum { one, two } c; }`
becomes `enum { x_c_one, x_c_two } x_c;`, and a variable declared as `x::c`
resolves bare `one` to `x_c_one`.

Build: `bazel build //...` · Test: `bazel test //...`

## Binary: `bazel-bin/okc`

- **`okc compile <clang args...>`** — rewrites inputs, then runs real clang:

  ```sh
  okc compile -std=c23 -c main.c -o main.o
  bazel build ... --action_env=CC="$PWD/bazel-bin/okc" --repo_env=CC="$PWD/bazel-bin/okc"
  CC=/abs/path/to/bazel-bin/okc make
  ```

  With no subcommand (`CC=okc`), `okc` defaults to compile mode.
  `SPACE_CC_REAL=clang-19` selects the underlying compiler.

- **`okc prepare [-o FILE] [--] FILE`** — rewrite without compiling:

  ```sh
  okc prepare input.c > output.c
  okc prepare -o output.c input.c
  ```

No allowlist or data files: copy `okc` to a machine with clang and it works.
Full usage guide: `usage.md`.

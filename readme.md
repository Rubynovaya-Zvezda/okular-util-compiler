# okular-compiler

Bazel module with a C23 namespace hook:

- `space z { ... }` prefixes declarations in the block with `z_`; blocks may
  nest: `space x { space y { int v; } }` → `int x_y_v;`;
- `z::item` → `z_item`, `x::y::item` → `x_y_item`, both inside and outside
  `space` blocks; inside a space, chains resolve relative to the innermost
  declaring space (`y::v` → `x_y_v` in `x/y`).
- `x::y::item.member` → `x_y_item.member` for struct/union members, or
  `x_y_item_member` for enum constants.
- `::item` → `item` (the global scope), also inside `space` blocks; a leading
  `::` forces the absolute name.
- Parameters of function definitions are not prefixed (outer parameters stay
  visible inside nested spaces), but globals declared in the space are (both at
  their declaration and at their uses in the function). Control-flow blocks
  (`if`, `for`, `while`, …) are not treated as function definitions.
- Inner declarations shadow outer ones. Macro names are resolved before any
  namespace rewriting, so bare uses stay untouched regardless of the scope in
  which the macro was defined.
- All preprocessor directives are copied verbatim. Macro names are recorded
  only so their uses are left untouched — macros are never prefixed or
  otherwise rewritten, inside or outside spaces. Names from `-D`/`-U` flags
  are honoured too; macros from headers are still unknown to the text pass.

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

## LSP: `okc lsp`

Point your editor's clangd executable at `okc lsp` (e.g. command `okc`,
args `["lsp", ...clangd flags...]`). It starts real clangd, intercepts LSP
messages and applies the same namespace rewriting to `didOpen`/`didChange`
text before clangd sees it. Request positions are translated into rewritten
coordinates and diagnostics/ranges are translated back, so line numbers and
columns refer to your original source. The `textDocumentSync` capability is
forced to full-document sync, which keeps the proxy's copy authoritative.

`SPACE_CLANGD=PATH` (or `okc lsp --clangd=PATH`) selects the underlying
clangd; by default `clangd` is used from `PATH`.

Completion, hover and diagnostics work with the rewritten names (e.g.
`a::v` is seen as `a_v`). Inserted text is converted back using the spelling
used at the edit location: outside a space an edit becomes `a::v`, inside a
space it becomes the bare `v`. This applies to completion inserts, text edits,
code actions, formatting and rename.

## Editor integrations

- **Vim syntax and folding** (`vim/`): install with vim-plug:
  `Plug 'Rubynovaya-Zvezda/okular-util-compiler', { 'rtp': 'vim' }`.
  Highlights `space`, `x::y::item` and `::item`, and folds `space name { ... }`
  blocks. See `vim/README.md`.
- **coc.nvim** (`coc-okc/`): run `npm publish` in that directory, then
  `:CocInstall coc-okc`. It starts `okc lsp` for C buffers. See
  `coc-okc/README.md` for settings and local installation.

## System install

Put `okc` on `PATH` so editor configs never need project-specific paths:

```sh
bazel run //:install_okc -- --prefix=/usr/local
# or: make install PREFIX=/usr/local
```

Then use `"okc.command": "okc"` in `coc-settings.json`. If you need a custom
`bindir`, pass `--bindir=/path/to/bin` to the Bazel target. clangd finds
`compile_commands.json` by walking up from the source file, so
`okc.args` can usually stay empty.

No allowlist or data files: copy `okc` to a machine with clang and clangd and it works.

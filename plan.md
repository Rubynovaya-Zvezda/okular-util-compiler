# Plan: standalone C23 namespace hook (no project changes)

## Scope

- No edits to existing project code or build files.
- Deliverables: the Bazel module at `/home/danila/project/okular/compiler`
  (`MODULE.bazel`, `BUILD.bazel`) plus `readme.md` and `usage.md` in it.
- Hook is C23 only; one binary `okc`, no C++, no clang headers, no data files.
- `okc compile ...` rewrites sources then invokes real clang; `okc prepare ...`
  rewrites without compiling. As `CC=okc`, compile mode is the default.

## Semantics

1. `space z { ... }` replaces the old `#start`/`#end` guards. Declarators and uses
   inside the block are prefixed with `z_`. Blocks may nest and prefixes compose:
   `space x { space y { int v; } }` → `int x_y_v;`. An unclosed block is
   `space-unclosed`.
2. `z::item` → `z_item`; `x::y::item` → `x_y_item`. Chains of any depth are
   allowed, resolved relative to the innermost declaring space unless prefixed
   by a leading `::`. `.member` becomes `_member` for enum types and stays
   `.member` for struct/union types. Malformed chains are `space-bad-form`.
   Old `space(z).item` syntax is removed.
3. Struct/union members are not prefixed (already isolated by the struct).
4. `enum { one, two } c;` inside `space x` becomes
   `enum { x_c_one, x_c_two } x_c;`; the type `x_c` is recorded with its constants.
5. A variable declared with type `x::c` resolves bare constants of that type:
   `x::c v = one;` → `x_c v = x_c_one;`, and later `v = two;` → `v = x_c_two;`.
6. Rewriting is a plain text pass before preprocessing: comments, strings and
   include/pragma lines are untouched; line count is preserved.
7. No allowlist: library names inside a space block are prefixed; outside blocks
   they are untouched. Library headers/sources are never edited.

## Wrapper behaviour (`okc compile`)

- Leaves clang flags untouched; rewrites each source input into a temp copy with
  `#line 1 "<original>"`, adding `-iquote <original dir>` for quoted includes.
- stdin/outside-root inputs pass through (`SPACE_HOOK_ROOTS`, default cwd).
- Execs `SPACE_CC_REAL` (default `clang`); aborts before clang on hook errors.
- Fixes `-MF` depfiles; no response-file support in v1.

## Tests and validation

- `bazel test //...` / `make test`: `hook_tests` covers all examples above,
  comments/strings, line count, symbol names via `nm`, nesting/chains, unclosed
  or malformed forms, and wrapper rejection of bad input.
- Verify a real build with `--action_env=CC=.../okc --repo_env=CC=.../okc`.

## Acceptance criteria

- Examples from the request produce exactly the shown output.
- Nested spaces and qualified chains rewrite as composed names; unclosed spaces
  and malformed qualified names fail with clear diagnostics.
- Single self-contained `okc` binary; copy it next to a machine with clang and run.

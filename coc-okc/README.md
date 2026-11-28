# coc-okc

coc.nvim extension that runs `okc lsp` as the C language server. It gives Vim
completions, hover and diagnostics for `space`/`::` source code: clangd runs
underneath, while `okc` rewrites text before clangd sees it and converts
suggested edits back to your source syntax.

## Install

Publish from this directory with `npm publish` (maintainers), then in Vim:

```vim
:CocInstall coc-okc
```

To use the copy in this repository without publishing:

```vim
:CocInstall /absolute/path/to/okular-util-compiler/coc-okc
```

`okc` and `clangd` must be on `PATH`, or set the paths below.

## Settings (`coc-settings.json`)

```json
{
  "okc.command": "/abs/path/to/okc",
  "okc.clangd": "clangd-19",
  "okc.args": ["--compile-commands-dir=/path/to/build"],
  "okc.rootPatterns": [".git", "compile_commands.json"],
  "okc.enable": true
}
```

`okc.args` are forwarded to clangd. `okc.clangd` sets `SPACE_CLANGD`.

Run `:CocCommand okc.restart` after changing settings. Open `:CocList services`
to check the server state, and `:CocCommand workspace.showOutput` (channel
`okc`) for clangd logs.

## Notes

- The extension starts only when a document looks like okc C: `c.okc`
  filetype / `.okc` extension, a `.okc` or `okc.json` marker in the project
  tree, or source text containing `space name {` or `x::y`. Otherwise plain C
  is left to clangd/ccls.
- Both the Vim syntax plugin (`vim/` in this repo) and coc-okc can be used
  together; the syntax plugin is only for highlighting and folding.

# vim-okc

Vim syntax highlighting and folding for the `space` / `::` C23 extension used
by `okc`. The plugin attaches only when one of these matches: a `c.okc`/`okc`
filetype (or `*.okc` extension), a `.okc`/`okc.json` marker in the project
tree, or source text containing `space name {` or `x::y`. Plain C files are
untouched.

## Install with vim-plug

Add this to your `vimrc`, then run `:PlugInstall`:

```vim
Plug 'Rubynovaya-Zvezda/okular-util-compiler', { 'rtp': 'vim' }
```

To disable the extras:

```vim
let g:okc_highlight = 0  " keep default C highlighting only
let g:okc_folds = 0      " do not set foldmethod=expr for C buffers
```

## What it adds

- highlights `space` declarations, qualified names (`x::y::item`) and leading
  global qualifiers (`::item`);
- folds `space name { ... }` blocks, including nested ones, without folding
  ordinary C blocks;
- highlight groups can be overridden in your config:

```vim
hi link okcSpaceDecl Statement
hi link okcSpaceName Identifier
hi link okcQualified Identifier
hi link okcGlobalQual Special
```

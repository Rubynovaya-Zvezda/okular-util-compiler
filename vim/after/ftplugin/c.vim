" Fold `space name { ... }` blocks while leaving ordinary C braces alone.
if exists('b:okc_ftplugin_loaded') || get(g:, 'okc_folds', 1) == 0
  finish
endif
let b:okc_ftplugin_loaded = 1

setlocal foldmethod=expr
setlocal foldexpr=okc#fold(v:lnum)
setlocal foldtext=okc#foldtext()
setlocal foldcolumn=1

" Explicitly load okc's C additions; `after/syntax/c.vim` can be shadowed by a
" user's own after file, so do not rely on runtime lookup order.
if exists('g:loaded_okc_plugin')
  finish
endif
let g:loaded_okc_plugin = 1

let s:dir = expand('<sfile>:p:h:h')

" Treat a buffer as okc C when any of these match:
"   - filetype c.okc (or okc), or a .okc extension
"   - a `.okc` / `okc.json` marker in the project tree
"   - source text containing `space name {` or a qualified `x::y` name
function! s:is_okc() abort
  if &filetype =~# '\(^\|\.\)okc\($\|\.\)' || expand('%:e') ==# 'okc'
    return 1
  endif
  if !empty(findfile('.okc', '.;')) || !empty(findfile('okc.json', '.;'))
    return 1
  endif
  let l:mod = findfile('MODULE.bazel', '.;')
  if !empty(l:mod) && !empty(filter(readfile(l:mod), 'v:val =~# "okular-compiler"'))
    return 1
  endif
  let l:head = getline(1, min([line('$'), 400]))
  return !empty(filter(copy(l:head), 'v:val =~# "space\\s\\+\\h\\w*\\s*{" || v:val =~# "\\h\\w*\\s*::\\s*\\h\\w*"'))
endfunction

function! s:setup() abort
  if !s:is_okc()
    return
  endif
  if !exists('b:current_syntax')
    runtime! syntax/c.vim
  endif
  execute 'source' s:dir . '/after/syntax/c.vim'
  execute 'source' s:dir . '/after/ftplugin/c.vim'
endfunction

augroup okc
  autocmd!
  autocmd FileType * call s:setup()
  " Some C ftplugins set foldmethod=syntax later; reapply ours on entry.
  autocmd BufEnter,BufWinEnter * if s:is_okc() && get(b:, 'okc_folds', get(g:, 'okc_folds', 1))
        \ | setlocal foldmethod=expr foldtext=okc#foldtext() | endif
augroup END

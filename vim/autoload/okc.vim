" Fold support for `space name { ... }` blocks.
if exists('g:autoloaded_okc')
  finish
endif
let g:autoloaded_okc = 1

" Remove comments and literals so braces inside them do not affect folds.
function! s:strip(line, in_block) abort
  let l:s = a:line
  let l:out = ''
  let l:i = 0
  let l:in = a:in_block
  while l:i < strlen(l:s)
    if l:in
      if l:i + 1 < strlen(l:s) && l:s[l:i] ==# '*' && l:s[l:i + 1] ==# '/'
        let l:in = 0
        let l:i += 2
      else
        let l:i += 1
      endif
      continue
    endif
    if l:i + 1 < strlen(l:s) && l:s[l:i] ==# '/' && l:s[l:i + 1] ==# '*'
      let l:in = 1
      let l:i += 2
      continue
    endif
    if l:i + 1 < strlen(l:s) && l:s[l:i] ==# '/' && l:s[l:i + 1] ==# '/'
      break
    endif
    let l:out .= l:s[l:i]
    let l:i += 1
  endwhile
  let l:out = substitute(l:out, '"[^"]*"', '""', 'g')
  let l:out = substitute(l:out, "'[^']*'", "''", 'g')
  return [l:out, l:in]
endfunction

function! okc#fold(lnum) abort
  if !exists('b:okc_fold_data') || b:okc_fold_data.tick != b:changedtick
    let l:data = {'tick': b:changedtick, 'levels': {}}
    let l:depth = 0
    let l:stack = []
    let l:pending = -1
    let l:in_block = 0
    for l:n in range(1, line('$'))
      let l:parts = s:strip(getline(l:n), l:in_block)
      let l:s = l:parts[0]
      let l:in_block = l:parts[1]
      let l:before = len(l:stack)
      let l:opened = 0
      let l:i = 0
      while l:i < strlen(l:s)
        let l:rest = l:s[l:i:]
        if l:rest =~# '^space\>'
          let l:m = matchstr(l:rest, '^space\s\+\h\w*\s*{')
          if !empty(l:m)
            call add(l:stack, [l:depth, l:n])
            let l:depth += 1
            let l:opened = 1
            let l:i += strlen(l:m)
            continue
          endif
          let l:m = matchstr(l:rest, '^space\s\+\h\w*')
          if !empty(l:m)
            let l:pending = l:n
            let l:i += strlen(l:m)
            continue
          endif
        endif
        if l:s[l:i] ==# '{'
          if l:pending >= 0
            call add(l:stack, [l:depth, l:pending])
            let l:pending = -1
            let l:opened = 1
          endif
          let l:depth += 1
        elseif l:s[l:i] ==# '}'
          let l:depth -= 1
          if !empty(l:stack) && l:stack[-1][0] == l:depth
            call remove(l:stack, -1)
          endif
        endif
        let l:i += 1
      endwhile
      let l:data.levels[l:n] = l:opened ? len(l:stack) : l:before
    endfor
    let b:okc_fold_data = l:data
  endif
  return get(b:okc_fold_data.levels, a:lnum, 0)
endfunction

function! okc#foldtext() abort
  let l:line = substitute(getline(v:foldstart), '^\s*', '', '')
  let l:count = v:foldend - v:foldstart + 1
  return printf('%s  [%d lines]', l:line, l:count)
endfunction

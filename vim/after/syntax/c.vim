" okc syntax additions for C buffers. Loaded after the built-in C syntax.
if exists('b:okc_syntax_loaded') || get(g:, 'okc_highlight', 1) == 0
  finish
endif
let b:okc_syntax_loaded = 1

" `space name {` — the declaration keyword and its name.
syntax keyword okcSpaceDecl space nextgroup=okcSpaceName skipwhite
syntax match   okcSpaceName '\<\h\w*\>' contained

" Qualified chains: x::y::item, optionally with a .member or ::member.
syntax match okcQualified '\<\h\w*\>\s*\(::\s*\h\w*\>\)\+' contains=okcSpaceDecl
" A leading `::` forces the global name.
syntax match okcGlobalQual '::\s*\h\w*\>' contains=okcSpaceDecl

if !hlexists('okcSpaceDecl')
  highlight def link okcSpaceDecl Statement
endif
if !hlexists('okcSpaceName')
  highlight def link okcSpaceName Identifier
endif
if !hlexists('okcQualified')
  highlight def link okcQualified Identifier
endif
if !hlexists('okcGlobalQual')
  highlight def link okcGlobalQual Special
endif

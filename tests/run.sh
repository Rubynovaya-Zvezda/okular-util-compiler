#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here"
if [ ! -x ./okc ] && [ -x ../okc ]; then cd ..; fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
check_error() {
    name=$1 id=$2 file=$3
    if ./okc prepare -o "$tmp/out.c" "$file" >"$tmp/stdout" 2>"$tmp/stderr"; then
        fail "$name: expected failure"
    fi
    grep -q "$id" "$tmp/stderr" || fail "$name: expected $id, got: $(cat "$tmp/stderr")"
    pass "$name"
}
cat > "$tmp/a.c" <<'E1'
space a {
 int a;
 struct { int one; int two; } b;
}
E1
./okc prepare -o "$tmp/a.out.c" "$tmp/a.c"
grep -q '^ int a_a;' "$tmp/a.out.c" || fail "space declarator"
grep -q 'struct { int one; int two; } a_b;' "$tmp/a.out.c" || fail "struct members"
pass "space block and struct members"
cat > "$tmp/enum.c" <<'E2'
space x {
 enum { one, two } c;
}
E2
./okc prepare -o "$tmp/enum.out.c" "$tmp/enum.c"
grep -q 'enum { x_c_one, x_c_two } x_c;' "$tmp/enum.out.c" || fail "enum constants"
pass "enum constants"
cat > "$tmp/qual.c" <<'E3'
space x {
 enum { one, two } c;
}
x::c enum_variable = one;
enum_variable = two;
x::c.one
E3
./okc prepare -o "$tmp/qual.out.c" "$tmp/qual.c"
grep -q '^x_c enum_variable = x_c_one;' "$tmp/qual.out.c" || fail "declaration"
grep -q '^enum_variable = x_c_two;' "$tmp/qual.out.c" || fail "assignment"
grep -q '^x_c_one$' "$tmp/qual.out.c" || fail "qualified member"
pass "qualified names and enum resolution"
cat > "$tmp/scope.c" <<'E10'
space z {
 int ::name = 1;
 int x::name = 2;
 int f(void) { return ::name + x::name; }
 int k = :: name;
}
int ::outside = 3;
int y::outside = 4;
int g(void) { return ::outside + y::outside; }
int h(void) { return ::y::outside; }
E10
./okc prepare -o "$tmp/scope.out.c" "$tmp/scope.c"
grep -q '^ int name = 1;' "$tmp/scope.out.c" || fail "global name in space"
grep -q '^ int x_name = 2;' "$tmp/scope.out.c" || fail "qualified name in space"
grep -q '^ int z_f(void) { return name + x_name; }' "$tmp/scope.out.c" || fail "uses in space"
grep -q '^ int z_k = name;' "$tmp/scope.out.c" || fail "spaced global qualifier"
grep -q '^int outside = 3;' "$tmp/scope.out.c" || fail "global name outside"
grep -q '^int y_outside = 4;' "$tmp/scope.out.c" || fail "qualified name outside"
grep -q '^int g(void) { return outside + y_outside; }' "$tmp/scope.out.c" || fail "uses outside"
grep -q '^int h(void) { return y_outside; }' "$tmp/scope.out.c" || fail "global qualified name outside"
pass "global and qualified name resolution"
cat > "$tmp/unary.c" <<'E48'
int name = 1;
space x { int name = 2; }
space x_name { int name = 3; }
int *a = &x::name;
int b = *x::name;
int *c = &::name;
int d = *::name;
int *e = &x_name::name;
int f = *x_name::name;
space z {
  int *g = &x::name;
  int h = *::name;
}
E48
./okc prepare -o "$tmp/unary.out.c" "$tmp/unary.c"
grep -Fq 'int *a = &x_name;' "$tmp/unary.out.c" || fail "address of qualified name"
grep -Fq 'int b = *x_name;' "$tmp/unary.out.c" || fail "dereference of qualified name"
grep -Fq 'int *c = &name;' "$tmp/unary.out.c" || fail "address of global name"
grep -Fq 'int d = *name;' "$tmp/unary.out.c" || fail "dereference of global name"
grep -Fq 'int *e = &x_name_name;' "$tmp/unary.out.c" || fail "address of nested qualified name"
grep -Fq 'int f = *x_name_name;' "$tmp/unary.out.c" || fail "dereference of nested qualified name"
grep -Fq 'int *z_g = &x_name;' "$tmp/unary.out.c" || fail "address of qualified name in space"
grep -Fq 'int z_h = *name;' "$tmp/unary.out.c" || fail "dereference of global name in space"
pass "unary operators before qualified and global names"
cat > "$tmp/params.c" <<'E11'
space x {
 bool v = false;
 int funct(int a, int b) {
  v=false;
  v = 0>a+b;
  v = !v;
  return v ;
 }
 int other(int a) { return a + v; }
}
E11
./okc prepare -o "$tmp/params.out.c" "$tmp/params.c"
grep -q '^ bool x_v = false;' "$tmp/params.out.c" || fail "space-scope variable"
grep -q '^ int x_funct(int a, int b) {' "$tmp/params.out.c" || fail "parameter names in signature"
grep -q '^  x_v=false;' "$tmp/params.out.c" || fail "global use in function"
grep -q '^  x_v = 0>a+b;' "$tmp/params.out.c" || fail "parameter uses in function"
grep -q '^  x_v = !x_v;' "$tmp/params.out.c" || fail "negated global use"
grep -q '^  return x_v ;' "$tmp/params.out.c" || fail "returned global use"
grep -q '^ int x_other(int a) { return a + x_v; }' "$tmp/params.out.c" || fail "parameters reset between functions"
pass "function parameters are not prefixed"
cat > "$tmp/weird.c" <<'E12'
space x {
 int v;
 int
 f
 (
   int a ,
   const char * s
 )
 /* c */
 // d
 {
   return a + (s != 0) + v;
 }
 int h(int a) [[gnu::unused]]
 { return a + v; }
}
E12
./okc prepare -o "$tmp/weird.out.c" "$tmp/weird.c"
grep -Fq ' x_f' "$tmp/weird.out.c" || fail "newline before paren"
grep -Fq '   int a ,' "$tmp/weird.out.c" || fail "multiline parameters"
grep -Fq '   return a + (s != 0) + x_v;' "$tmp/weird.out.c" || fail "body after comments"
grep -Fq ' int x_h(int a) [[gnu::unused]]' "$tmp/weird.out.c" || fail "attributes between signature and body"
grep -Fq ' { return a + x_v; }' "$tmp/weird.out.c" || fail "body after attributes"
[ "$(wc -l < "$tmp/weird.c")" = "$(wc -l < "$tmp/weird.out.c")" ] || fail "line count with odd formatting"
pass "unusual function formatting"
cat > "$tmp/skip.c" <<'E4'
space z {
const char *s = "z::item one";
/* z::item two */
// z::item three
int item = 1;
}
E4
./okc prepare -o "$tmp/skip.out.c" "$tmp/skip.c"
grep -Fq '"z::item one"' "$tmp/skip.out.c" || fail "string literal rewritten"
grep -Fq '/* z::item two */' "$tmp/skip.out.c" || fail "block comment rewritten"
grep -Fq '// z::item three' "$tmp/skip.out.c" || fail "line comment rewritten"
grep -q '^int z_item' "$tmp/skip.out.c" || fail "guard identifier"
[ "$(wc -l < "$tmp/skip.c")" = "$(wc -l < "$tmp/skip.out.c")" ] || fail "line count"
pass "comments and strings"
cat > "$tmp/compile.c" <<'E5'
int namespace::item(void) { return 7; }
E5
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -c "$tmp/compile.c" -o "$tmp/compile.o"
if command -v nm >/dev/null 2>&1; then
    nm "$tmp/compile.o" | grep -q 'namespace_item' || fail "symbol namespace_item"
fi
pass "compiler wrapper"
cat > "$tmp/lib.c" <<'E6'
int outside(void) { return printf("x"); }
space a {
int inside(void) { return printf("x"); }
}
E6
./okc prepare -o "$tmp/lib.out.c" "$tmp/lib.c"
grep -q 'return printf("x");' "$tmp/lib.out.c" || fail "library name outside"
grep -q 'return a_printf("x");' "$tmp/lib.out.c" || fail "library name inside"
pass "library names"
cat > "$tmp/nested.c" <<'E7'
space x {
 int a;
 space y {
  int v;
  struct { int m; } s;
  enum { one, two } e;
  int f(int p) { return y::v + p + one; }
  space z {
   int deep;
  }
 }
}
x::y::v=5;
x::y::e enum_var = one;
enum_var = two;
x::y::z::deep = 7;
int g(void) { return ::x::y::v + x::y::v; }
E7
./okc prepare -o "$tmp/nested.out.c" "$tmp/nested.c"
grep -q '^ int x_a;' "$tmp/nested.out.c" || fail "outer space variable"
grep -q '^  int x_y_v;' "$tmp/nested.out.c" || fail "nested variable"
grep -q 'struct { int m; } x_y_s;' "$tmp/nested.out.c" || fail "nested struct"
grep -q 'enum { x_y_e_one, x_y_e_two } x_y_e;' "$tmp/nested.out.c" || fail "nested enum"
grep -q '^  int x_y_f(int p) { return x_y_v + p + x_y_e_one; }' "$tmp/nested.out.c" || fail "nested function"
grep -q '^   int x_y_z_deep;' "$tmp/nested.out.c" || fail "deep nesting"
grep -q '^x_y_v=5;' "$tmp/nested.out.c" || fail "qualified chain assignment"
grep -q '^x_y_e enum_var = x_y_e_one;' "$tmp/nested.out.c" || fail "qualified chain type"
grep -q '^enum_var = x_y_e_two;' "$tmp/nested.out.c" || fail "qualified chain enum"
grep -q '^x_y_z_deep = 7;' "$tmp/nested.out.c" || fail "deep qualified chain"
grep -q '^int g(void) { return x_y_v + x_y_v; }' "$tmp/nested.out.c" || fail "global qualified chain"
[ "$(wc -l < "$tmp/nested.c")" = "$(wc -l < "$tmp/nested.out.c")" ] || fail "line count with nesting"
pass "nested spaces and qualified chains"
cat > "$tmp/fnscope.c" <<'E15'
space x {
 int outer(int p) {
  space y {
   int inner(int q) { return q + p; }
  }
  return p;
 }
}
E15
./okc prepare -o "$tmp/fnscope.out.c" "$tmp/fnscope.c"
grep -q '^ int x_outer(int p) {' "$tmp/fnscope.out.c" || fail "outer parameter"
grep -Fq 'int x_y_inner(int q) { return q + p; }' "$tmp/fnscope.out.c" || fail "outer parameter in nested function"
grep -Fq '  return p;' "$tmp/fnscope.out.c" || fail "outer parameter after nested space"
pass "nested parameter scopes"
cat > "$tmp/pp.c" <<'E16'
#define OUT 9
#define _UNDER 2
#define ADD(a,b) ((a)+(b))
space x {
#include <stddef.h>
#define VALUE 1
 space y {
  int v = OUT + _UNDER + ADD(1,2) + VALUE;
 }
#undef VALUE
}
E16
./okc prepare -o "$tmp/pp.out.c" "$tmp/pp.c"
grep -q '^#define OUT 9' "$tmp/pp.out.c" || fail "outer macro definition"
grep -q '^#define _UNDER 2' "$tmp/pp.out.c" || fail "underscore macro definition"
grep -q '^#define ADD(a,b) ((a)+(b))' "$tmp/pp.out.c" || fail "function-like macro definition"
grep -q '^#include <stddef.h>' "$tmp/pp.out.c" || fail "include rewritten"
grep -q '^#define VALUE 1' "$tmp/pp.out.c" || fail "space macro definition"
grep -q '^#undef VALUE' "$tmp/pp.out.c" || fail "macro undef"
grep -q '^  int x_y_v = OUT + _UNDER + ADD(1,2) + VALUE;' "$tmp/pp.out.c" || fail "macro uses rewritten"
[ "$(wc -l < "$tmp/pp.c")" = "$(wc -l < "$tmp/pp.out.c")" ] || fail "line count with directives"
pass "macros are ignored by the rewriter"
cat > "$tmp/enumscope.c" <<'E17'
space x {
 enum { one } a;
 space y {
  enum { one } b;
  int v = one;
 }
}
E17
./okc prepare -o "$tmp/enumscope.out.c" "$tmp/enumscope.c"
grep -q 'enum { x_a_one } x_a;' "$tmp/enumscope.out.c" || fail "outer enum constant"
grep -q 'enum { x_y_b_one } x_y_b;' "$tmp/enumscope.out.c" || fail "inner enum constant"
grep -Fq 'int x_y_v = x_y_b_one;' "$tmp/enumscope.out.c" || fail "inner constant resolution"
pass "scoped enum constants"
cat > "$tmp/tagged.c" <<'E18'
space x { struct S { int m; } s; enum E { one } e; }
int v = x::S.m + x::E::one;
E18
./okc prepare -o "$tmp/tagged.out.c" "$tmp/tagged.c"
grep -q 'struct x_S { int m; } x_s;' "$tmp/tagged.out.c" || fail "tagged struct"
grep -q 'enum x_E { x_E_one } x_e;' "$tmp/tagged.out.c" || fail "tagged enum"
grep -q '^int v = x_S.m + x_E_one;' "$tmp/tagged.out.c" || fail "qualified members"
pass "tagged types and qualified members"
cat > "$tmp/multiline.c" <<'E19'
space
 x
{
 int v;
}
E19
./okc prepare -o "$tmp/multiline.out.c" "$tmp/multiline.c"
grep -q '^ int x_v;' "$tmp/multiline.out.c" || fail "multi-line space opener"
[ "$(wc -l < "$tmp/multiline.c")" = "$(wc -l < "$tmp/multiline.out.c")" ] || fail "line count with multi-line opener"
pass "multi-line space opener"
cat > "$tmp/shadow.c" <<'E20'
space x {
 int v = 1;
 space y {
  int v = 2;
  int f(void) { return v; }
 }
}
E20
./okc prepare -o "$tmp/shadow.out.c" "$tmp/shadow.c"
grep -q '^ int x_v = 1;' "$tmp/shadow.out.c" || fail "outer declaration"
grep -q '^  int x_y_v = 2;' "$tmp/shadow.out.c" || fail "inner declaration shadowing"
grep -q '^  int x_y_f(void) { return x_y_v; }' "$tmp/shadow.out.c" || fail "inner declaration use"
pass "inner declarations shadow outer names"
cat > "$tmp/control.c" <<'E21'
space s {
 int v;
 int f(void) {
  if (v) { return v; }
  for (int i = 0; i < v; ++i) { v = v + i; }
  while (v) { v = v - 1; }
  return v;
 }
}
E21
./okc prepare -o "$tmp/control.out.c" "$tmp/control.c"
grep -Fq 'if (s_v) { return s_v; }' "$tmp/control.out.c" || fail "if condition"
grep -Fq 'for (int s_i = 0; s_i < s_v; ++s_i) { s_v = s_v + s_i; }' "$tmp/control.out.c" || fail "for condition"
grep -Fq 'while (s_v) { s_v = s_v - 1; }' "$tmp/control.out.c" || fail "while condition"
pass "control-flow blocks are not functions"
cat > "$tmp/expect.c" <<'E22'
space x { enum { a, b } e; }
x::e v = a;
int f(void) { int b = 3; return b; }
E22
./okc prepare -o "$tmp/expect.out.c" "$tmp/expect.c"
grep -q '^x_e v = x_e_a;' "$tmp/expect.out.c" || fail "qualified enum declaration"
grep -q '^int f(void) { int b = 3; return b; }' "$tmp/expect.out.c" || fail "enum expect leak"
pass "enum state does not leak past statements"
cat > "$tmp/badspace.c" <<'E23'
space { int v; }
E23
check_error "missing space name" "space-bad-form" "$tmp/badspace.c"
cat > "$tmp/brace.c" <<'E24'
space x { struct { char s[sizeof("}")]; int n; } v; }
E24
./okc prepare -o "$tmp/brace.out.c" "$tmp/brace.c"
grep -Fq 'struct { char s[sizeof("}")]; int n; } x_v;' "$tmp/brace.out.c" || fail "brace in string"
pass "braces inside strings are ignored"
cat > "$tmp/qualws.c" <<'E25'
space s { int x_y; }
x /*c*/ :: y = 1;
space t { int v; }
t ::
v = 2;
E25
./okc prepare -o "$tmp/qualws.out.c" "$tmp/qualws.c"
grep -q '^x_y/\*c\*/ = 1;' "$tmp/qualws.out.c" || fail "comment before qualifiers"
grep -q '^t_v = 2;' "$tmp/qualws.out.c" || fail "newline after qualifiers"
[ "$(wc -l < "$tmp/qualws.c")" = "$(wc -l < "$tmp/qualws.out.c")" ] || fail "line count with qualifiers"
pass "qualified names across comments and newlines"
cat > "$tmp/comments-cross.c" <<'E28'
space s { int x_y; }
x /* a
 b */ :: y = 1;
E28
./okc prepare -o "$tmp/comments-cross.out.c" "$tmp/comments-cross.c"
grep -q '^x_y/\* a$' "$tmp/comments-cross.out.c" || fail "multiline comment before qualifiers"
grep -q '^ b \*/ = 1;' "$tmp/comments-cross.out.c" || fail "multiline comment tail"
[ "$(wc -l < "$tmp/comments-cross.c")" = "$(wc -l < "$tmp/comments-cross.out.c")" ] || fail "line count with multiline comment"
pass "multiline comments crossed by qualifiers"
cat > "$tmp/comma.c" <<'E29'
space x { enum { one, b, two } e; }
x::e a = one, b = b, c = two;
x::e v = { one, two };
E29
./okc prepare -o "$tmp/comma.out.c" "$tmp/comma.c"
grep -q '^x_e a = x_e_one, b = b, c = x_e_two;' "$tmp/comma.out.c" || fail "comma declarator"
grep -q '^x_e v = { x_e_one, x_e_two };' "$tmp/comma.out.c" || fail "initializer constants"
pass "comma declarators do not consume enum constants"
cat > "$tmp/ppedge.c" <<'E26'
space x {
#define /*c*/ V 1
#define \
 U 2
int v = V + U;
}
E26
./okc prepare -o "$tmp/ppedge.out.c" "$tmp/ppedge.c"
grep -q '^#define /\*c\*/ V 1' "$tmp/ppedge.out.c" || fail "macro after comment"
grep -q '^int x_v = V + U;' "$tmp/ppedge.out.c" || fail "continued macro name"
pass "macro names after comments and continuations"
cat > "$tmp/cmddef.c" <<'E27'
space x { int v = FROM_CMD + TWICE(2); }
E27
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -DFROM_CMD=3 '-DTWICE(n)=((n)*2)' -fsyntax-only "$tmp/cmddef.c"
pass "command-line macros"
cat > "$tmp/nested-unclosed.c" <<'E13'
space x {
 space y {
 int v;
}
E13
check_error "unclosed nested space" "space-unclosed" "$tmp/nested-unclosed.c"
cat > "$tmp/unclosed.c" <<'E8'
space a {
int x;
E8
check_error "unclosed space" "space-unclosed" "$tmp/unclosed.c"
cat > "$tmp/badqual.c" <<'E9'
int x = a::b::;
E9
check_error "malformed qualified name" "space-bad-form" "$tmp/badqual.c"
cat > "$tmp/badmember.c" <<'E14'
int y = a::b.c.d;
E14
check_error "long qualified member" "space-bad-form" "$tmp/badmember.c"
if SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -fsyntax-only "$tmp/unclosed.c" >"$tmp/stdout" 2>"$tmp/stderr"; then
    fail "wrapper accepted unclosed space"
fi
grep -q 'space-unclosed' "$tmp/stderr" || fail "wrapper diagnostic"
pass "wrapper rejects bad input"
cat > "$tmp/leadcomment.c" <<'E30'
space x { int v; }
int q = :: /*c*/ v;
E30
./okc prepare -o "$tmp/leadcomment.out.c" "$tmp/leadcomment.c"
grep -q '^int q = v/\*c\*/;' "$tmp/leadcomment.out.c" || fail "comment after leading ::"
[ "$(wc -l < "$tmp/leadcomment.c")" = "$(wc -l < "$tmp/leadcomment.out.c")" ] || fail "line count after leading ::"
pass "comments after leading :: are preserved"
cat > "$tmp/enumexpr.c" <<'E31'
space x { enum E { one, two = one + 1, three = x::E::one } e; }
int q = two + three;
E31
./okc prepare -o "$tmp/enumexpr.out.c" "$tmp/enumexpr.c"
grep -q 'enum x_E { x_E_one, x_E_two = x_E_one + 1, x_E_three = x_E_one } x_e;' "$tmp/enumexpr.out.c" || fail "enum initializer expressions"
grep -q '^int q = x_E_two + x_E_three;' "$tmp/enumexpr.out.c" || fail "bare enum uses outside space"
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -Wall -Wextra -fsyntax-only "$tmp/enumexpr.c"
pass "enum initializer expressions and constants"
cat > "$tmp/enumcomments.c" <<'E32'
space x { enum { one, /* one */ two, // one
 three } e; }
E32
./okc prepare -o "$tmp/enumcomments.out.c" "$tmp/enumcomments.c"
grep -Fq 'enum { x_e_one, /* one */ x_e_two, // one' "$tmp/enumcomments.out.c" || fail "comments in enum body"
grep -Fq ' x_e_three } x_e;' "$tmp/enumcomments.out.c" || fail "enumerator after line comment"
[ "$(wc -l < "$tmp/enumcomments.c")" = "$(wc -l < "$tmp/enumcomments.out.c")" ] || fail "line count in enum comments"
pass "comments inside enum bodies are not code"
cat > "$tmp/structbody.c" <<'E33'
space x {
 struct S /* { */ {
  enum { one } e;
  struct T { int m; } t;
 } s;
}
E33
./okc prepare -o "$tmp/structbody.out.c" "$tmp/structbody.c"
grep -Fq 'struct x_S /* { */ {' "$tmp/structbody.out.c" || fail "brace in struct comment"
grep -Fq 'enum { x_e_one } e;' "$tmp/structbody.out.c" || fail "enum nested in struct"
grep -Fq 'struct x_T { int m; } t;' "$tmp/structbody.out.c" || fail "tag nested in struct"
grep -Fq '} x_s;' "$tmp/structbody.out.c" || fail "struct declarator"
[ "$(wc -l < "$tmp/structbody.c")" = "$(wc -l < "$tmp/structbody.out.c")" ] || fail "line count in struct body"
pass "struct bodies handle comments and nested declarations"
cat > "$tmp/ppcomments.c" <<'E34'
# /*c*/ define V 1
/*d*/ #define W 2
space x { int v = V + W; }
E34
./okc prepare -o "$tmp/ppcomments.out.c" "$tmp/ppcomments.c"
grep -q '^# /\*c\*/ define V 1' "$tmp/ppcomments.out.c" || fail "comment after #"
grep -q '^/\*d\*/ #define W 2' "$tmp/ppcomments.out.c" || fail "comment before #"
grep -q 'int x_v = V + W;' "$tmp/ppcomments.out.c" || fail "macro names after directive comments"
pass "directives with comments are recognized"
cat > "$tmp/bareenum.c" <<'E35'
enum { one } c;
int v = one;
E35
./okc prepare -o "$tmp/bareenum.out.c" "$tmp/bareenum.c"
grep -q '^enum { c_one } c;' "$tmp/bareenum.out.c" || fail "outside enum declaration"
grep -q '^int v = c_one;' "$tmp/bareenum.out.c" || fail "bare enum constant outside space"
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -Wall -Wextra -fsyntax-only "$tmp/bareenum.c"
pass "bare enum constants outside spaces"
cat > "$tmp/nestedprefix.c" <<'E36'
space x { int x_v = 1; space y { int x_v = 2; int z = x_v; } }
E36
./okc prepare -o "$tmp/nestedprefix.out.c" "$tmp/nestedprefix.c"
grep -q 'int x_v = 1;' "$tmp/nestedprefix.out.c" || fail "outer prefixed declaration"
grep -q 'int x_y_x_v = 2;' "$tmp/nestedprefix.out.c" || fail "inner declaration composes prefix"
grep -q 'int x_y_z = x_v;' "$tmp/nestedprefix.out.c" || fail "outer-prefixed use stays unchanged"
pass "nested declarations compose outer prefixes"
: > "$tmp/empty.c"
./okc prepare -o "$tmp/empty.out.c" "$tmp/empty.c"
[ -f "$tmp/empty.out.c" ] || fail "empty input produced no output file"
[ ! -s "$tmp/empty.out.c" ] || fail "empty input produced non-empty output"
pass "empty input is not an out-of-memory error"
cat > "$tmp/fixedenum.c" <<'E38'
space x { enum E : int { one, two } e; }
int q = x::E::one;
E38
./okc prepare -o "$tmp/fixedenum.out.c" "$tmp/fixedenum.c"
grep -q 'enum x_E : int { x_E_one, x_E_two } x_e;' "$tmp/fixedenum.out.c" || fail "fixed underlying enum"
grep -q '^int q = x_E_one;' "$tmp/fixedenum.out.c" || fail "fixed enum qualified constant"
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -Wall -Wextra -fsyntax-only "$tmp/fixedenum.c"
pass "C23 fixed underlying enums"
cat > "$tmp/qualcomment.c" <<'E39'
space s { int x_y; }
x // c
 :: y = 1;
E39
./okc prepare -o "$tmp/qualcomment.out.c" "$tmp/qualcomment.c"
grep -q '^x_y // c$' "$tmp/qualcomment.out.c" || fail "line comment in qualified name"
grep -Eq '^[[:space:]]*= 1;' "$tmp/qualcomment.out.c" || fail "statement commented out by qualifier"
[ "$(wc -l < "$tmp/qualcomment.c")" = "$(wc -l < "$tmp/qualcomment.out.c")" ] || fail "line count with qualifier comment"
pass "line comments in qualified names do not comment out code"
cat > "$tmp/typedshadow.c" <<'E40'
space a {
 enum { c = 5 } e1;
 a::e1 v = c;
 space b {
  enum { c = 9 } e2;
  int f(void) {
   b::e2 v = c;
   return v == c ? 0 : 1;
  }
 }
}
E40
./okc prepare -o "$tmp/typedshadow.out.c" "$tmp/typedshadow.c"
grep -Fq 'return a_b_v == a_b_e2_c ? 0 : 1;' "$tmp/typedshadow.out.c" || fail "shadowed enum type"
pass "shadowed declarations resolve to the inner type"
cat > "$tmp/enumdeclcomment.c" <<'E41'
space x { enum { one } /*c*/ e; }
E41
./okc prepare -o "$tmp/enumdeclcomment.out.c" "$tmp/enumdeclcomment.c"
grep -Fq 'enum { x_e_one } /*c*/ x_e;' "$tmp/enumdeclcomment.out.c" || fail "comment before enum declarator"
[ "$(wc -l < "$tmp/enumdeclcomment.c")" = "$(wc -l < "$tmp/enumdeclcomment.out.c")" ] || fail "line count with enum declarator comment"
pass "comments before enum declarators"
cat > "$tmp/initbraces.c" <<'E42'
space x { struct S s = { 1 }, t = { 2 }; }
E42
./okc prepare -o "$tmp/initbraces.out.c" "$tmp/initbraces.c"
grep -Fq 'struct x_S x_s = { 1 }, x_t = { 2 };' "$tmp/initbraces.out.c" || fail "initializer braces as type body"
pass "initializer braces are not type bodies"
cat > "$tmp/qualifiedsizeof.c" <<'E43'
space x { enum E { one } e; int q = sizeof(x::E) + one; }
E43
./okc prepare -o "$tmp/qualifiedsizeof.out.c" "$tmp/qualifiedsizeof.c"
grep -Fq 'int x_q = sizeof(x_E) + x_E_one;' "$tmp/qualifiedsizeof.out.c" || fail "qualified type in expression"
pass "qualified type uses do not leak declarator state"
cat > "$tmp/enumreturn.c" <<'E44'
space x { enum E { one } e; enum E f(void) { return one; } }
E44
./okc prepare -o "$tmp/enumreturn.out.c" "$tmp/enumreturn.c"
grep -Fq 'enum x_E { x_E_one } x_e; enum x_E x_f(void) { return x_E_one; }' "$tmp/enumreturn.out.c" || fail "enum return type body"
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -Wall -Wextra -fsyntax-only "$tmp/enumreturn.c"
pass "type references in signatures do not swallow bodies"
cat > "$tmp/parammembers.c" <<'E45'
space x { struct S { int m; } s; int f(struct S *p) { return p->m + s.m; } }
E45
./okc prepare -o "$tmp/parammembers.out.c" "$tmp/parammembers.c"
grep -Fq 'int x_f(struct x_S *p) { return p->m + x_s.m; }' "$tmp/parammembers.out.c" || fail "member access in function body"
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -Wall -Wextra -fsyntax-only "$tmp/parammembers.c"
pass "member access after . and -> is not prefixed"
cat > "$tmp/qualifiedmember.c" <<'E46'
space x { struct S { int m; }; int f(void) { struct x::S *p = 0; return p->m; } }
E46
./okc prepare -o "$tmp/qualifiedmember.out.c" "$tmp/qualifiedmember.c"
grep -Fq 'struct x_S *x_p = 0; return x_p->m;' "$tmp/qualifiedmember.out.c" || fail "qualified type member access"
pass "qualified type declarations keep member names"
cat > "$tmp/prototypes.c" <<'E47'
space x { int f(int a, char *b); typedef int (*F)(int cb); }
E47
./okc prepare -o "$tmp/prototypes.out.c" "$tmp/prototypes.c"
grep -Fq 'int x_f(int a, char *b); typedef int (*x_F)(int cb);' "$tmp/prototypes.out.c" || fail "prototype parameters"
SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -Wall -Wextra -fsyntax-only "$tmp/prototypes.c"
pass "prototypes and function pointer parameters stay unprefixed"
{
    printf 'space x { enum { '
    i=1
    while [ "$i" -le 80 ]; do
        [ "$i" -gt 1 ] && printf ', '
        printf 'c%d' "$i"
        i=$((i + 1))
    done
    printf ' } e; int q = c70 + c80; }\n'
} > "$tmp/manyconsts.c"
./okc prepare -o "$tmp/manyconsts.out.c" "$tmp/manyconsts.c"
grep -Fq 'x_e_c80 } x_e;' "$tmp/manyconsts.out.c" || fail "enum constants beyond fixed cap"
grep -Fq 'int x_q = x_e_c70 + x_e_c80;' "$tmp/manyconsts.out.c" || fail "uses of constants beyond fixed cap"
pass "enum constant tables grow past fixed caps"
{
    printf 'space x {\n'
    i=1
    while [ "$i" -le 300 ]; do
        printf ' int v%d;\n' "$i"
        i=$((i + 1))
    done
    printf ' int q = v300;\n}\n'
} > "$tmp/manyvars.c"
./okc prepare -o "$tmp/manyvars.out.c" "$tmp/manyvars.c"
grep -Fq 'int x_v300;' "$tmp/manyvars.out.c" || fail "declarations beyond fixed cap"
grep -Fq 'int x_q = x_v300;' "$tmp/manyvars.out.c" || fail "uses beyond fixed cap"
pass "declaration tables grow past fixed caps"
{
    i=0
    while [ "$i" -lt 70 ]; do
        printf 'space a%d {\n' "$i"
        i=$((i + 1))
    done
    printf ' int v;\n'
    i=69
    while [ "$i" -ge 0 ]; do
        printf '}\n'
        i=$((i - 1))
    done
    printf 'int q = '
    i=0
    while [ "$i" -lt 70 ]; do
        printf 'a%d::' "$i"
        i=$((i + 1))
    done
    printf 'v;\n'
} > "$tmp/deepqual.c"
./okc prepare -o "$tmp/deepqual.out.c" "$tmp/deepqual.c"
expected=
i=0
while [ "$i" -lt 70 ]; do
    [ -n "$expected" ] && expected="${expected}_"
    expected="${expected}a$i"
    i=$((i + 1))
done
expected="${expected}_v"
grep -Fq "int $expected;" "$tmp/deepqual.out.c" || fail "deep qualified declaration"
grep -Fq "int q = $expected;" "$tmp/deepqual.out.c" || fail "deep qualified use"
pass "qualified chains grow past fixed caps"
doc=readme.md
[ -f "$doc" ] || doc=../readme.md
if grep -q 'usage\.md' "$doc" && [ ! -e "$(dirname "$doc")/usage.md" ]; then
    fail "stale usage.md reference"
fi
pass "no stale documentation link"
cat > "$tmp/fake-clangd" <<'EOS'
#!/bin/sh
frame() { printf 'Content-Length: %s\r\n\r\n%s' "${#1}" "$1"; }
cat > "$FAKE_LOG"
frame '{"jsonrpc":"2.0","id":1,"result":{"capabilities":{"astProvider":true,"textDocumentSync":{"change":2}}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///t.c","diagnostics":[{"range":{"start":{"line":1,"character":8},"end":{"line":1,"character":11}},"message":"x"}]}}'
frame '{"jsonrpc":"2.0","id":2,"result":[{"label":"a_v","insertText":"a_v","textEdit":{"range":{"start":{"line":1,"character":8},"end":{"line":1,"character":11}},"newText":"a_v"}}]}'
EOS
chmod +x "$tmp/fake-clangd"
frame() { printf 'Content-Length: %s\r\n\r\n%s' "${#1}" "$1"; }
{
frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t.c","languageId":"c","version":1,"text":"space a { int v = 7; }\nint x = a::v;\n"}}}'
frame '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///t.c"},"position":{"line":1,"character":11}}}'
frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///t.c","version":2},"contentChanges":[{"text":"space a { int v = 7; }\nint y = a::v;\n"}]}}'
} > "$tmp/lsp.in"
FAKE_LOG="$tmp/lsp.log" SPACE_CLANGD="$tmp/fake-clangd" ./okc lsp < "$tmp/lsp.in" > "$tmp/lsp.out"
grep -Fq 'int x = a_v;' "$tmp/lsp.log" || fail "lsp didOpen rewrite"
grep -Fq 'int y = a_v;' "$tmp/lsp.log" || fail "lsp didChange rewrite"
if grep -Fq 'a::v' "$tmp/lsp.log"; then fail "lsp qualified name not rewritten"; fi
grep -Fq '"character":10' "$tmp/lsp.log" || fail "lsp request position mapping"
grep -Fq '"astProvider":true' "$tmp/lsp.out" || fail "lsp boolean round trip"
grep -Fq '"textDocumentSync":1' "$tmp/lsp.out" || fail "lsp full sync override"
grep -Fq '"character":12' "$tmp/lsp.out" || fail "lsp diagnostic position mapping"
grep -Fq '"label":"a::v"' "$tmp/lsp.out" || fail "lsp completion label unrewrite"
grep -Fq '"insertText":"a::v"' "$tmp/lsp.out" || fail "lsp completion insert text unrewrite"
grep -Fq '"newText":"a::v"' "$tmp/lsp.out" || fail "lsp text edit unrewrite"
pass "clangd lsp proxy rewrites documents, edits and positions"
if command -v vim >/dev/null 2>&1 && [ -d vim ]; then
cat > "$tmp/vimtest.vim" <<EOF
set rtp^=$PWD/vim
set nocompatible
filetype plugin on
source $PWD/vim/plugin/okc.vim
edit $tmp/vimplain.c
call writefile([string(hlexists('okcSpaceDecl')), string(&foldmethod)], '$tmp/vimplain')
edit $tmp/bazelproj/a.c
call writefile([string(hlexists('okcSpaceDecl'))], '$tmp/vimmodule')
edit $tmp/vimfold.c
call writefile([string(hlexists('okcSpaceDecl')), string(&foldmethod), string(okc#fold(2)), string(okc#fold(6))], '$tmp/vimresult')
qa!
EOF
cat > "$tmp/vimplain.c" <<'EOF'
int main(void) { return 0; }
EOF
mkdir -p "$tmp/bazelproj"
printf 'bazel_dep(name = "okular-compiler", version = "0.1.0")\n' > "$tmp/bazelproj/MODULE.bazel"
printf 'int main(void) { return 0; }\n' > "$tmp/bazelproj/a.c"
cat > "$tmp/vimfold.c" <<'EOF'
int g;
space outer {
  int a;
  space inner { int b; }
  int c = a::v;
}
EOF
vim -Es -u NONE -i NONE -S "$tmp/vimtest.vim" >/dev/null 2>&1 || fail "vim plugin load"
[ "$(sed -n 1p "$tmp/vimplain")" = '0' ] || fail "vim plugin on plain C"
[ "$(sed -n 1p "$tmp/vimmodule")" = '1' ] || fail "vim bazel module detection"
[ "$(sed -n 1p "$tmp/vimresult")" = '1' ] || fail "vim syntax groups"
[ "$(sed -n 2p "$tmp/vimresult")" = "'expr'" ] || fail "vim foldmethod"
[ "$(sed -n 3p "$tmp/vimresult")" = '1' ] || fail "vim outer fold"
[ "$(sed -n 4p "$tmp/vimresult")" = '1' ] || fail "vim closing fold line"
pass "vim syntax and folding plugin with okc detection"
fi
if command -v node >/dev/null 2>&1 && [ -f coc-okc/lib/index.js ]; then
    (cd coc-okc && node test/basic.js >/dev/null) || fail "coc-okc extension"
    pass "coc-okc extension"
fi
mkdir -p "$tmp/installcheck"
cp install-okc.sh "$tmp/installcheck/install-okc.sh"
printf '#!/bin/sh\nexit 0\n' > "$tmp/installcheck/okc"
chmod +x "$tmp/installcheck/okc" "$tmp/installcheck/install-okc.sh"
"$tmp/installcheck/install-okc.sh" --bindir="$tmp/installcheck/bin" >/dev/null
[ -x "$tmp/installcheck/bin/okc" ] || fail "install_okc target script"
pass "install_okc script"
echo "all hook tests passed"

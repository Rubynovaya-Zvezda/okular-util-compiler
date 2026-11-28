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
space a {
 space b {
 int x;
 }
}
E7
check_error "nested space" "space-nested" "$tmp/nested.c"
cat > "$tmp/unclosed.c" <<'E8'
space a {
int x;
E8
check_error "unclosed space" "space-unclosed" "$tmp/unclosed.c"
cat > "$tmp/badqual.c" <<'E9'
int x = a::b::c;
E9
check_error "long qualified name" "space-bad-form" "$tmp/badqual.c"
if SPACE_HOOK_ROOTS="$tmp" ./okc compile -std=c23 -fsyntax-only "$tmp/unclosed.c" >"$tmp/stdout" 2>"$tmp/stderr"; then
    fail "wrapper accepted unclosed space"
fi
grep -q 'space-unclosed' "$tmp/stderr" || fail "wrapper diagnostic"
pass "wrapper rejects bad input"
echo "all hook tests passed"

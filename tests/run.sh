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

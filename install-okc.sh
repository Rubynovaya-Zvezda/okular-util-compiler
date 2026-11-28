#!/bin/sh
# Install the built okc binary so editors can use "okc.command": "okc".
set -eu
prefix=/usr/local
bindir=
while [ $# -gt 0 ]; do
  case $1 in
    --prefix) shift; prefix=${1:-/usr/local} ;;
    --prefix=*) prefix=${1#--prefix=} ;;
    --bindir) shift; bindir=$1 ;;
    --bindir=*) bindir=${1#--bindir=} ;;
    -h|--help) printf 'usage: install_okc [--prefix DIR] [--bindir DIR]\n'; exit 0 ;;
    *) echo "install_okc: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$bindir" ] || bindir=$prefix/bin
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$here/okc
[ -x "$src" ] || { echo "install_okc: $src not found" >&2; exit 1; }
mkdir -p "$bindir"
tmp=$bindir/okc.new
cp "$src" "$tmp"
chmod 755 "$tmp"
mv -f "$tmp" "$bindir/okc"
echo "installed $bindir/okc"

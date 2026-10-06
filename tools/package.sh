#!/bin/sh
# A release archive of a bootstrapped tree (./bootstrap.sh first): yelc and yel-lsp, the runtime
# they read and link (the prelude, std, the headers and yel.c, libuv's headers), and the objects a
# program links (build/yel.o, build/libuv.a), laid out as yelc finds its home (the directory above
# its own: bin/yelc's is the archive's root). The archive is checked before it is made: the yelc in
# it says the version given, and compiles and runs a program from its own home, from elsewhere.
#
#   tools/package.sh <target> <version> [out dir]     e.g. tools/package.sh linux-x64 "pre-alpha 3 (1a2b3c4 2026-10-06)"
#
# Out: <out dir>/yel-<target>.tar.gz (out by default). CC (cc) builds yel-lsp and the check's program.
set -eu
cd "$(dirname "$0")/.."
target=${1:?usage: tools/package.sh <target> <version> [out dir]}
version=${2:?usage: tools/package.sh <target> <version> [out dir]}
out=${3:-out}
CC=${CC:-cc}
libs="-lm -lpthread"
if [ "$(uname -s)" = Linux ]; then libs="$libs -ldl -lrt"; fi
for made in build/yelc2 build/yel.o build/libuv.a; do
	[ -f "$made" ] || { echo "package: no $made (run ./bootstrap.sh first)" >&2; exit 1; }
done

# yel-lsp, by this yelc (tools/lsp)
build/yelc2 tools/lsp build/yel-lsp.c
$CC -O2 -w -Iruntime -Iruntime/libuv/include -o build/yel-lsp build/yel-lsp.c build/yel.o build/libuv.a $libs

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
home="$root/yel-$target"
mkdir -p "$home/bin" "$home/build" "$home/runtime/libuv"
cp build/yelc2 "$home/bin/yelc"
cp build/yel-lsp "$home/bin/yel-lsp"
cp build/yel.o build/libuv.a "$home/build/"
cp runtime/prelude.yel runtime/yel.c runtime/*.h "$home/runtime/"
cp -R runtime/std runtime/wit "$home/runtime/"
cp -R runtime/libuv/include "$home/runtime/libuv/"
cp runtime/libuv/LICENSE runtime/libuv/LICENSE-extra "$home/runtime/libuv/"
cp README.md LICENSE "$home/"

# the check: what it says it is, and a program built from its home alone
said=$("$home/bin/yelc" --version)
[ "$said" = "yelc $version" ] || { echo "package: yelc says '$said', not 'yelc $version'" >&2; exit 1; }
check="$root/check"
mkdir -p "$check"
printf '%s\n' 'main: func() { print("hello from yel\n"); }' > "$check/hello.yel"
(cd "$check" && "$home/bin/yelc" hello.yel hello.c)
$CC -O1 -w -I"$home/runtime" -I"$home/runtime/libuv/include" -o "$check/hello" "$check/hello.c" "$home/build/yel.o" "$home/build/libuv.a" $libs
ran=$("$check/hello")
[ "$ran" = "hello from yel" ] || { echo "package: the check's program said '$ran'" >&2; exit 1; }

mkdir -p "$out"
tar -czf "$out/yel-$target.tar.gz" -C "$root" "yel-$target"
echo "$out/yel-$target.tar.gz: $said"

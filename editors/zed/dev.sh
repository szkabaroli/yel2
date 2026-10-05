#!/bin/sh
# yel2's Zed extension, ready to install as a dev extension (Zed: "zed: install dev extension", then
# this directory): yel-lsp built (build/yel-lsp), the grammar generated (editors/tree-sitter-yel)
# and committed as a snapshot to a git repository of its own (build/tree-sitter-yel, as Zed builds a
# grammar from a repository at a commit), and extension.toml's grammar pointed at that commit.
#
#   editors/zed/dev.sh            yel-lsp and the grammar
#   editors/zed/dev.sh grammar    the grammar alone (after a change to it: no yel-lsp built)
#
# YELC: the compiler that builds yel-lsp (default build/yelc2: one that includes the compiler package,
# as yel-lsp does).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
YELC=${YELC:-build/yelc2}
CC=${CC:-cc}

# yel-lsp
if [ "${1:-}" != grammar ]; then
	cd "$root"
	LIBS="build/yel.o build/libuv.a -lm -lpthread"
	if [ "$(uname -s)" = Linux ]; then LIBS="$LIBS -ldl -lrt"; fi
	"$YELC" tools/lsp build/yel-lsp.c
	$CC -O2 -Iruntime -Iruntime/libuv/include -c -o build/yel.o runtime/yel.c
	$CC -O2 -Iruntime -Iruntime/libuv/include -o build/yel-lsp build/yel-lsp.c $LIBS
	echo "built build/yel-lsp"
fi

# the grammar, generated
cd "$root/editors/tree-sitter-yel"
[ -d node_modules ] || npm install --no-audit --no-fund
npx tree-sitter generate

# its snapshot: a repository of its own, one commit
snapshot="$root/build/tree-sitter-yel"
rm -rf "$snapshot"
mkdir -p "$snapshot"
cp -R grammar.js package.json src "$snapshot/"
cd "$snapshot"
git init -q
git add -A
git -c user.name=yel -c user.email=yel@localhost -c commit.gpgsign=false commit -qm "tree-sitter-yel snapshot"
rev=$(git rev-parse HEAD)

# extension.toml's grammar: that commit
cd "$here"
awk -v repo="file://$snapshot" -v rev="$rev" '
	/^\[grammars\.yel\]/ { section = 1; print; next }
	/^\[/ { section = 0 }
	section && /^repository = / { print "repository = \"" repo "\""; next }
	section && /^rev = / { print "rev = \"" rev "\""; next }
	{ print }
' extension.toml > extension.toml.new
mv extension.toml.new extension.toml
echo "grammar: file://$snapshot at $rev"
echo "now in Zed: zed: rebuild dev extension (or, the first time, zed: install dev extension: $here)"

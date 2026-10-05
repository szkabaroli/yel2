#!/bin/sh
# yel-lsp's session: the server built (build/yel-lsp), a client's messages played through it, and its
# replies, one a line, checked against tools/lsp/session.out (--update: written there instead). The
# messages arrive split mid-header and mid-body, as a pipe may give them. The session: a request
# before initialize, initialize (UTF-16 positions), a document never saved opened with non-ASCII text
# and a type error (the compiler's diagnostic, once the change has settled), hovers (a local's
# type), a definition and references, a completion after a dot, the outline, a rename and
# where it may be, a method call's signature, the error fixed
# in two changes (one check: its diagnostics cleared), formatting, an unknown method, bad JSON, the
# document closed, shutdown, a request after it, exit; then one that ends with no shutdown.
#
#   tools/lsp/run.sh [--update]
#
# YELC: the compiler (default build/yelc2).
set -u
cd "$(dirname "$0")/../.."
YELC=${YELC:-build/yelc2}
CC=${CC:-cc}
LIBS="build/yel.o build/libuv.a -lm -lpthread"
if [ "$(uname -s)" = Linux ]; then LIBS="$LIBS -ldl -lrt"; fi
"$YELC" tools/lsp build/yel-lsp.c || { echo "yel-lsp does not compile"; exit 1; }
$CC -O1 -Iruntime -Iruntime/libuv/include -c -o build/yel.o runtime/yel.c || exit 1
$CC -O1 -Werror=implicit-function-declaration -Iruntime -Iruntime/libuv/include -o build/yel-lsp build/yel-lsp.c $LIBS || exit 1

# a message framed: its header, the blank line, its body
frame() { printf 'Content-Length: %d\r\n\r\n%s' "$(printf %s "$1" | wc -c | tr -d ' ')" "$1"; }

# the document: in a directory that is not on disk, so the check sees it only as the editor's
# buffer (the compiler's overlays); its URIs and paths read as <root> in the replies
root=$(pwd)
uri="file://$root/build/lsp-unsaved/main.yel"
rm -rf build/lsp-unsaved
# a line of non-ASCII: é is one UTF-16 unit and two bytes, 😀 two units and four bytes; and a
# string given where an s32 is wanted (the compiler's error, its place in UTF-16 units); a record,
# and a func reading one of its fields
text="main: func() -> s32 {\\n    let cafe = \\\"\\u00e9\\ud83d\\ude00\\\"; let total-count = 1;\\n    let wrong: s32 = \\\"text\\\";\\n    total-count + wrong + cafe.len() as s32\\n}\\nrecord pair { left: s32, right: s32, }\\nfirst: func(p: pair) -> s32 { p.left }\\n"
# the error fixed, though not laid out as yelc --fmt lays it out
fixed="main: func() -> s32 {   0 }\\n"

session() {
	frame '{"jsonrpc":"2.0","id":1,"method":"textDocument/hover","params":{}}'
	frame '{"jsonrpc":"2.0","id":2,"method":"initialize","params":{"capabilities":{"general":{"positionEncodings":["utf-16"]}}}}'
	frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
	# split mid-header and mid-body
	whole=$(frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"$uri\",\"languageId\":\"yel\",\"version\":1,\"text\":\"$text\"}}}")
	printf %s "$whole" | head -c 9
	sleep 0.2
	printf %s "$whole" | tail -c +10 | head -c 60
	sleep 0.2
	printf %s "$whole" | tail -c +70
	# total-count: after cafe = "é😀"; at UTF-16 column 29 (byte 32)
	frame "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":1,\"character\":29}}}"
	frame "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":1,\"character\":9}}}"
	# what names name: total-count's use (after cafe = "é😀": UTF-16 columns) to its let, and the
	# uses of wrong (its declaration too)
	frame "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":3,\"character\":6}}}"
	frame "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"textDocument/references\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":2,\"character\":8},\"context\":{\"includeDeclaration\":true}}}"
	# what may follow p. (just past the dot): the record's fields, then the funcs taking it first
	frame "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":6,\"character\":32}}}"
	# the outline: main, the record and its fields, first
	frame "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"textDocument/documentSymbol\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"}}}"
	# total-count renamed (at its use): where it may be, then its declaration's and use's edits
	# (after "é😀": UTF-16 columns)
	frame "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"textDocument/prepareRename\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":3,\"character\":6}}}"
	frame "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"textDocument/rename\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":3,\"character\":6},\"newName\":\"sum\"}}"
	# the signature of cafe.len(), inside its parens: len's, its receiver its first parameter
	frame "{\"jsonrpc\":\"2.0\",\"id\":37,\"method\":\"textDocument/signatureHelp\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"position\":{\"line\":3,\"character\":35}}}"
	# the check runs once no change has come for a while: its diagnostics
	sleep 1
	frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"$uri\",\"version\":2},\"contentChanges\":[{\"text\":\"$fixed\"}]}}"
	# two changes in a row: one check, of the last
	frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"$uri\",\"version\":3},\"contentChanges\":[{\"text\":\"$fixed\"}]}}"
	sleep 1
	frame "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"},\"options\":{\"tabSize\":4,\"insertSpaces\":true}}}"
	frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/definition","params":{}}'
	frame '{"jsonrpc":"2.0","id":7,"method":'
	frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"$uri\"}}}"
	sleep 1
	frame '{"jsonrpc":"2.0","id":"last","method":"shutdown"}'
	frame '{"jsonrpc":"2.0","id":8,"method":"textDocument/hover","params":{}}'
	frame '{"jsonrpc":"2.0","method":"exit"}'
}

# the replies, each on a line of its own (their headers checked: each Content-Length its body's)
session | build/yel-lsp > build/yel-lsp.out 2> build/yel-lsp.err
status=$?
perl -e '
	local $/; my $all = <STDIN>;
	while (length $all) {
		$all =~ s/\AContent-Length: (\d+)\r\n\r\n// or die "a reply with no header: $all\n";
		print substr($all, 0, $1, ""), "\n";
	}' < build/yel-lsp.out | sed "s|$root|<root>|g" > build/yel-lsp.replies || exit 1
echo "exit $status" >> build/yel-lsp.replies
# stdin closed with no shutdown (the editor gone): exit 1, as LSP says
frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | build/yel-lsp > /dev/null 2>&1
echo "exit $? (stdin closed with no shutdown)" >> build/yel-lsp.replies
if [ "${1:-}" = --update ]; then
	cp build/yel-lsp.replies tools/lsp/session.out
	echo "tools/lsp/session.out written"
	exit 0
fi
if diff -u tools/lsp/session.out build/yel-lsp.replies; then echo "yel-lsp: the session as expected"; else exit 1; fi

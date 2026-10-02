#!/bin/sh
# The bitcode backend against the C one: each tests/bitcode/*.yel compiled both ways (yelc --backend
# bitcode, linked with the runtime (runtime/yel.c) built to bitcode, by clang at -O0 and -O2), each run, and what
# it does (stdout, stderr, exit code) the same as the C build's, also with a collection before every
# allocation (YEL_GC_STRESS=1: the frame's roots must keep what is live). With WASI_SDK: the bitcode built for
# wasm too, and run under wasmtime (its exit code ok or err, as WASI's: 0 or 1). On an arm64 Mac with
# Rosetta: built for x86-64 too (libuv and the runtime with it), and run under Rosetta: each of the C
# calling conventions llvm.yel knows, run.
#
#   tools/bitcode-test.sh [tests...]     (YELC: the compiler, default build/yelc2)
set -u
cd "$(dirname "$0")/.."
YELC=${YELC:-build/yelc2}
CFLAGS="-Iruntime -Iruntime/libuv/include"
out=build/bitcode-test
mkdir -p "$out"
# what the runtime links with natively (libuv: its event loop)
SYSTEM="build/libuv.a -lm -lpthread"
if [ "$(uname -s)" = Linux ]; then SYSTEM="$SYSTEM -ldl -lrt"; fi
LIBS="$out/yel.o $SYSTEM"
tests=${*:-tests/bitcode/*.yel}
# LLVM's verifier, run on every module the compiler writes (clang, a release build, skips it on what
# it is given: an invalid module may build, or crash it, instead of saying what is wrong)
OPT=${OPT:-$(command -v opt || echo /opt/homebrew/opt/llvm/bin/opt)}
[ -x "$OPT" ] || { echo "no LLVM opt (for its verifier): set OPT"; exit 1; }
verify() { "$OPT" -passes=verify -disable-output "$1" 2>&1 | head -5; }
failed=0
# the runtime, for the C build (an object) and the bitcode one (bitcode, for each target: the C one's
# as it is, linked into the program's module)
cc -O0 -w $CFLAGS -c -o "$out/yel.o" runtime/yel.c || exit 1
clang -O2 -c -emit-llvm $CFLAGS -o "$out/runtime.bc" runtime/yel.c 2> "$out/runtime.err" || { cat "$out/runtime.err"; exit 1; }
if [ -n "${WASI_SDK:-}" ]; then
	"$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O2 -c -emit-llvm -Iruntime -o "$out/runtime-wasm.bc" runtime/yel.c 2> "$out/runtime-wasm.err" || { cat "$out/runtime-wasm.err"; exit 1; }
fi
# x86-64 under Rosetta (an arm64 Mac that has it): libuv and the runtime built for it
x86=""
if [ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] && arch -x86_64 /usr/bin/true 2> /dev/null; then
	x86=x86_64-apple-macosx
	CC="cc -arch x86_64" sh runtime/libuv/build.sh build/x86_64 > /dev/null || exit 1
	clang --target=$x86 -O2 -c -emit-llvm $CFLAGS -o "$out/runtime-x86.bc" runtime/yel.c 2> "$out/runtime-x86.err" || { cat "$out/runtime-x86.err"; exit 1; }
fi
# a build's run: its stdout, its stderr (apart: how the two interleave is each libc's buffering), then
# its exit code
run() {
	"$@" > "$out/stdout.txt" 2> "$out/stderr.txt"
	status=$?
	cat "$out/stdout.txt"
	echo "-- stderr"
	cat "$out/stderr.txt"
	echo "exit $status"
}
for test in $tests; do
	name=$(basename "$test" .yel)
	# a test's own C functions (tests/bitcode/<name>.c, its externs'): included into the C build,
	# compiled to bitcode for the bitcode one's target
	own=${test%.yel}.c
	include=""
	extra=""
	extra_wasm=""
	extra_x86=""
	if [ -f "$own" ]; then
		include="-include $own"
		clang -O2 -c -emit-llvm $CFLAGS -o "$out/$name-own.bc" "$own" || { echo "FAIL $name: its C"; failed=1; continue; }
		extra="$out/$name-own.bc"
		if [ -n "${WASI_SDK:-}" ]; then
			"$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O2 -c -emit-llvm -Iruntime -o "$out/$name-own-wasm.bc" "$own" || { echo "FAIL $name: its C (wasm)"; failed=1; continue; }
			extra_wasm="$out/$name-own-wasm.bc"
		fi
		if [ -n "$x86" ]; then
			clang --target=$x86 -O2 -c -emit-llvm $CFLAGS -o "$out/$name-own-x86.bc" "$own" || { echo "FAIL $name: its C (x86-64)"; failed=1; continue; }
			extra_x86="$out/$name-own-x86.bc"
		fi
	fi
	# what it must do: its expected output (tests/bitcode/<name>.out: a test whose C cannot be built
	# with the C build's, as C functions taking yel's own structs), else the C build's
	expected=${test%.yel}.out
	if [ "$(dirname "$test")" = tests/bitcode ] && [ -f "$expected" ]; then
		want=$(cat "$expected"; echo "-- stderr"; echo "exit 0")
	else
		"$YELC" "$test" "$out/$name.c" && cc -O0 -w $CFLAGS $include -o "$out/$name-c" "$out/$name.c" $LIBS || { echo "FAIL $name: the C build"; failed=1; continue; }
		want=$(run "$out/$name-c")
	fi
	if ! "$YELC" "$test" "$out/$name.bc" --backend bitcode 2> "$out/$name.err"; then
		echo "FAIL $name: $(cat "$out/$name.err")"
		failed=1
		continue
	fi
	invalid=$(verify "$out/$name.bc")
	if [ -n "$invalid" ]; then
		echo "FAIL $name: LLVM's verifier: $invalid"
		failed=1
		continue
	fi
	ok=1
	for level in O0 O2; do
		if ! clang -$level -Wno-override-module "$out/$name.bc" $extra "$out/runtime.bc" $SYSTEM -o "$out/$name-$level" 2> "$out/$name-$level.err"; then
			echo "FAIL $name: clang -$level: $(head -3 "$out/$name-$level.err")"
			ok=0
			continue
		fi
		got=$(run "$out/$name-$level")
		if [ "$got" != "$want" ]; then
			echo "FAIL $name (-$level): the C build gave"
			echo "$want" | head -5
			echo "  and the bitcode build"
			echo "$got" | head -5
			ok=0
		fi
		# and with a collection before every allocation: a root the frame does not keep shows here
		got=$(YEL_GC_STRESS=1 run "$out/$name-$level")
		if [ "$got" != "$want" ]; then
			echo "FAIL $name (-$level, YEL_GC_STRESS=1): the C build gave"
			echo "$want" | head -5
			echo "  and the bitcode build"
			echo "$got" | head -5
			ok=0
		fi
	done
	if [ -n "${WASI_SDK:-}" ] && [ $ok = 1 ]; then
		"$YELC" "$test" "$out/$name-wasm.bc" --backend bitcode --triple wasm32-unknown-wasip3 \
			&& { invalid=$(verify "$out/$name-wasm.bc"); [ -z "$invalid" ] || { echo "LLVM's verifier: $invalid" > "$out/$name-wasm.err"; false; }; } \
			&& "$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O2 -Wno-override-module "$out/$name-wasm.bc" $extra_wasm "$out/runtime-wasm.bc" -lm -o "$out/$name.wasm" 2> "$out/$name-wasm.err" \
			|| { echo "FAIL $name: the wasm build: $(head -3 "$out/$name-wasm.err")"; ok=0; }
		if [ $ok = 1 ]; then
			code=$(echo "$want" | tail -1 | sed 's/exit //')
			wasm_want=$(echo "$want" | sed '$d'; if [ "$code" = 0 ]; then echo "exit 0"; else echo "exit 1"; fi)
			got=$(run wasmtime run -W component-model-async=y -S p3=y "$out/$name.wasm")
			[ "$got" = "$wasm_want" ] || { echo "FAIL $name (wasm): the C build gave $code, wasmtime: $(echo "$got" | tail -1)"; ok=0; }
		fi
	fi
	if [ -n "$x86" ] && [ $ok = 1 ]; then
		if "$YELC" "$test" "$out/$name-x86.bc" --backend bitcode --triple $x86 2> "$out/$name-x86.err" \
			&& { invalid=$(verify "$out/$name-x86.bc"); [ -z "$invalid" ] || { echo "LLVM's verifier: $invalid" > "$out/$name-x86.err"; false; }; } \
			&& clang --target=$x86 -O2 -Wno-override-module "$out/$name-x86.bc" $extra_x86 "$out/runtime-x86.bc" build/x86_64/libuv.a -lm -lpthread -o "$out/$name-x86" 2>> "$out/$name-x86.err"; then
			got=$(run arch -x86_64 "$out/$name-x86")
			[ "$got" = "$want" ] || { echo "FAIL $name (x86-64): the C build gave"; echo "$want" | head -5; echo "  and the x86-64 bitcode build"; echo "$got" | head -5; ok=0; }
		else
			echo "FAIL $name: the x86-64 build: $(head -3 "$out/$name-x86.err")"
			ok=0
		fi
	fi
	if [ $ok = 1 ]; then echo "ok   $name"; else failed=1; fi
done
# with WASI_SDK and no tests named: the component tests (as bootstrap.sh runs the C build's) built as
# bitcode: imports, async ones, streams and futures, resources, the filesystem, exports (the calls
# tests/component.calls makes), and (wac) components plugged into each other
if [ -n "${WASI_SDK:-}" ] && [ -z "${*:-}" ]; then
	crun() { perl -e 'alarm 120; exec @ARGV' wasmtime run -W component-model-async=y -S p3=y "$@"; }
	component() { # name source [reactor] [-I wit]
		cname=$1; source=$2; shift 2
		model=""
		if [ "${1:-}" = reactor ]; then model=-mexec-model=reactor; shift; fi
		"$YELC" "$source" "$out/c-$cname.bc" --backend bitcode --triple wasm32-unknown-wasip3 "$@" --wit "$out/c-$cname-wit" > "$out/c-$cname.log" 2>&1 \
			&& { invalid=$(verify "$out/c-$cname.bc"); [ -z "$invalid" ] || { echo "LLVM's verifier: $invalid" >> "$out/c-$cname.log"; false; }; } \
			&& "$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O2 -Wno-override-module $model "$out/c-$cname.bc" "$out/runtime-wasm.bc" -lm -Wl,-z,stack-size=8388608 -Wl,--component-type,"$out/c-$cname-wit" -o "$out/c-$cname.wasm" >> "$out/c-$cname.log" 2>&1
	}
	expect() { # name want got
		if [ "$3" = "$(cat "$2")" ]; then echo "ok   $1 (component)"; else echo "FAIL $1 (component): $(echo "$3" | head -3)"; failed=1; fi
	}
	built() { component "$@" || { echo "FAIL $1 (component): $(grep -v '^ *#\|Stack\|PLEASE' "$out/c-$1.log" | head -3)"; failed=1; return 1; }; }
	built imports tests/imports.yel && expect imports tests/imports.out "$(crun "$out/c-imports.wasm" 2>&1)"
	built host-waits tests/host-waits.yel && expect host-waits tests/host-waits.out "$(crun "$out/c-host-waits.wasm" 2>&1)"
	built cat tests/cat.yel && expect cat tests/cat.component.out "$(crun "$out/c-cat.wasm" < tests/cat.in 2>&1)"
	built stdin-drop tests/stdin-drop.yel && expect stdin-drop tests/stdin-drop.component.out "$(head -c 2000000 /dev/zero | crun "$out/c-stdin-drop.wasm" 2>&1)"
	built resources tests/resources.yel && expect resources tests/resources.out "$(crun -S inherit-network=y --dir . "$out/c-resources.wasm" 2>&1)"
	built filesystem tests/filesystem.yel && expect filesystem tests/filesystem.out "$(crun --dir . "$out/c-filesystem.wasm" 2>&1)"
	if built component tests/component reactor; then
		while read -r call; do crun --invoke "$call" "$out/c-component.wasm"; done < tests/component.calls > "$out/c-component.calls.out" 2>&1
		expect component tests/component.calls.out "$(cat "$out/c-component.calls.out")"
	fi
	if command -v wac > /dev/null; then
		built fixed-api tests/fixed-api reactor && built fixed-caller tests/fixed-caller -I "$out/c-fixed-api-wit" \
			&& wac plug "$out/c-fixed-caller.wasm" --plug "$out/c-fixed-api.wasm" -o "$out/c-fixed-lists.wasm" \
			&& expect fixed-lists tests/fixed-lists.component.out "$(crun -W component-model-fixed-length-lists=y "$out/c-fixed-lists.wasm" 2>&1)"
		built stream-api tests/stream-api reactor && built stream-caller tests/stream-caller -I "$out/c-stream-api-wit" \
			&& wac plug "$out/c-stream-caller.wasm" --plug "$out/c-stream-api.wasm" -o "$out/c-streams.wasm" \
			&& expect streams tests/streams.component.out "$(crun "$out/c-streams.wasm" 2>&1)"
	fi
fi
exit $failed

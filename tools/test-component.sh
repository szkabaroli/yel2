#!/bin/sh
# A package's tests as a WASI 0.3 component (yelc test --wit: the runner an export interface, tests:
# names() and run(name)), each run by a call of its own on a fresh instance (wasmtime --invoke), so a
# failure (a panic) ends only its test, as the native runner's child processes do. Told as the native
# runner tells them: ok or FAIL for each (what the test wrote first), then how many failed; the exit
# status 1 where any did. Snapshots (expect.snapshot) are read and written in the working directory,
# granted to each call, as the native runner's; YEL_SNAPSHOTS=update writes those that differ.
#
#   tools/test-component.sh <package> [names...]   (a test runs where any name is part of its own;
#                                                   all for none. WASI_SDK: a wasi-sdk directory;
#                                                   YELC: the compiler, default build/yelc2;
#                                                   WASMTIME_FLAGS: what else the host grants each
#                                                   call, as -S inherit-network=y; BACKEND=bitcode:
#                                                   built as LLVM bitcode, not C)
set -u
cd "$(dirname "$0")/.."
YELC=${YELC:-build/yelc2}
[ -n "${WASI_SDK:-}" ] || { echo "test-component.sh: set WASI_SDK to a wasi-sdk directory"; exit 2; }
[ $# -ge 1 ] || { echo "usage: tools/test-component.sh <package> [names...]"; exit 2; }
package=$1
shift
out=build/test-component
mkdir -p "$out"
name=$(basename "$package" .yel)
if [ "${BACKEND:-c}" = bitcode ]; then
	"$YELC" test "$package" "$out/$name.bc" --wit "$out/$name-wit" --backend bitcode --triple wasm32-unknown-wasip3 || exit 1
	"$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O2 -c -emit-llvm -Iruntime -o "$out/runtime-wasm.bc" runtime/yel.c || exit 1
	program="$out/$name.bc"
	runtime="$out/runtime-wasm.bc"
else
	"$YELC" test "$package" "$out/$name.c" --wit "$out/$name-wit" || exit 1
	"$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O1 -Iruntime -c -o "$out/yel-wasm.o" runtime/yel.c || exit 1
	program="$out/$name.c"
	runtime="$out/yel-wasm.o"
fi
"$WASI_SDK/bin/clang" --target=wasm32-wasip3 -O1 -Iruntime -Wno-override-module -Wl,-z,stack-size=8388608 -mexec-model=reactor \
	-Wl,--component-type,"$out/$name-wit" -o "$out/$name.wasm" "$program" "$runtime" -lm 2> "$out/$name.link" \
	|| { cat "$out/$name.link"; exit 1; }
# a call of one export (a component that hangs is killed after two minutes): the working directory
# granted (its tests' snapshots, read and written there), and YEL_SNAPSHOTS passed on
snapshots=""
[ -n "${YEL_SNAPSHOTS:-}" ] && snapshots="--env YEL_SNAPSHOTS=$YEL_SNAPSHOTS"
call() { perl -e 'alarm 120; exec @ARGV' wasmtime run -W component-model-async=y -S p3=y --dir . $snapshots ${WASMTIME_FLAGS:-} --invoke "$1" "$out/$name.wasm"; }
# names() as WAVE gives it: ["a", "m.b"]
tests=$(call 'names()' | sed 's/^\[//; s/\]$//; s/", "/\n/g; s/"//g')
ran=0
failed=0
for test in $tests; do
	if [ $# -gt 0 ]; then
		chosen=0
		for wanted in "$@"; do case "$test" in *"$wanted"*) chosen=1 ;; esac; done
		[ $chosen = 1 ] || continue
	fi
	ran=$((ran + 1))
	if call "run(\"$test\")" > "$out/stdout.txt" 2> "$out/stderr.txt"; then
		# (what it wrote, then the call's result: ())
		sed '$d' "$out/stdout.txt"
		cat "$out/stderr.txt"
		echo "ok   $test"
	else
		cat "$out/stdout.txt" "$out/stderr.txt"
		echo "FAIL $test"
		failed=$((failed + 1))
	fi
done
if [ $ran = 1 ]; then echo "1 test, $failed failed"; else echo "$ran tests, $failed failed"; fi
[ $failed = 0 ]

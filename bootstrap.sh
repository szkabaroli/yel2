#!/bin/sh
# Bootstraps yelc, the yel compiler written in yel, from its seed: the C a previous yelc made of
# itself (seed/yelc.c). Nothing but a C compiler is needed.
#   stage 0  cc builds the seed                                    -> build/yelc0
#   stage 1  yelc0 compiles the compiler (compiler/), cc builds that -> build/yelc1
#   stage 2  yelc1 compiles it                                      -> build/yelc2
#   stage 3  yelc2 compiles it                                      -> build/yelc3.c
# yelc2.c must equal yelc3.c: the compiler, built by itself, reproduces itself (a fixed point).
# yelc1 may differ from yelc2: it was built by the seed's code generator, not by the new one.
# Then every tests/*.yel is compiled by yelc2 and run (again with YEL_GC_STRESS=1: a collection before
# every allocation), and its output must be tests/<name>.out; yelc2 compiles itself once more under
# YEL_GC_STRESS=$GC_STRESS (default 5000) and must make the same C; every
# tests/errors/*.yel must fail to compile with the errors their first lines name, and no others.
#
#   ./bootstrap.sh                 quick, for development: -O0, the fixed point and every test (each
#                                  also under GC stress; WASI_SDK=<dir>: the component tests too)
#   ./bootstrap.sh --full          -O2, and the compiler compiling itself under GC stress, natively
#                                  and (WASI_SDK) as wasm; GC_STRESS=n: collect every n allocations
#                                  (default 5000: a self-compile makes ~13 million, each collection
#                                  marks the whole live heap), GC_STRESS_WASM=n as wasm (default 20000:
#                                  wasmtime's run is the slow one; lower both when the GC's or the
#                                  ABI's rooting changes: the tests already collect before every one)
#   ./bootstrap.sh --update-seed   --full, then, after a clean fixed point, write yelc3.c as the new seed
#
# A new language feature lands in two steps, because the seed only compiles what it knew: first
# teach yelc the feature without using it in compiler/, and update the seed; then use it.
set -eu
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-cc}
# the mode: quick for development (-O0, no self-compile under stress), or --full (and --update-seed):
# -O2 and every check
full=0
case "${1:-}" in --full|--update-seed) full=1 ;; esac
if [ "$full" -eq 1 ]; then OPT=${OPT:--O2}; else OPT=${OPT:--O0}; fi
# a native build links libuv (runtime/libuv: the native host's event loop), built once
sh runtime/libuv/build.sh build
CFLAGS="$OPT -Iruntime -Iruntime/libuv/include"
# the runtime's definitions (runtime/yel.c), compiled once and linked with every program
$CC $CFLAGS -c -o build/yel.o runtime/yel.c
LIBS="build/yel.o build/libuv.a -lm -lpthread"
if [ "$(uname -s)" = Linux ]; then LIBS="$LIBS -ldl -lrt"; fi
# a seed from before a change to the runtime brings the runtime it was made for (seed/runtime), for
# the C it makes: its own (stage 0) and stage 1's. Its definitions too (seed/runtime/yel.c), if it has
# them (a header-only runtime has none)
SEED_CFLAGS=$CFLAGS
SEED_LIBS=$LIBS
if [ -d seed/runtime ]; then
	SEED_CFLAGS="$OPT -Iseed/runtime -Iruntime/libuv/include"
	SEED_LIBS=${LIBS#build/yel.o }
	if [ -f seed/runtime/yel.c ]; then
		$CC $SEED_CFLAGS -c -o build/yel-seed.o seed/runtime/yel.c
		SEED_LIBS="build/yel-seed.o $SEED_LIBS"
	fi
fi

echo "stage 0: cc seed/yelc.c"
$CC $SEED_CFLAGS -o build/yelc0 seed/yelc.c $SEED_LIBS

for n in 1 2 3; do
	prev=$((n - 1))
	echo "stage $n: yelc$prev compiler -> build/yelc$n.c"
	"build/yelc$prev" compiler "build/yelc$n.c"
	flags=$CFLAGS
	libs=$LIBS
	if [ "$n" -eq 1 ]; then flags=$SEED_CFLAGS; libs=$SEED_LIBS; fi
	if [ "$n" -lt 3 ]; then $CC $flags -o "build/yelc$n" "build/yelc$n.c" $libs; fi
done
cmp -s build/yelc2.c build/yelc3.c || { echo "no fixed point: diff build/yelc2.c build/yelc3.c"; exit 1; }
echo "fixed point: yelc2.c == yelc3.c"

failed=0
# a test is a file, or a package's directory with an expected output (tests/<name>.out; a directory
# without one is a package the tests include)
tests=""
for test in tests/*.yel tests/*/; do
	test=${test%/}
	name=$(basename "$test" .yel)
	[ "$name" = errors ] && continue
	# a component of exports only runs under a host (below, with WASI_SDK)
	[ "$name" = component ] && continue
	[ -f "tests/$name.out" ] || continue
	tests="$tests $test"
done
for test in $tests; do
	name=$(basename "$test" .yel)
	build/yelc2 "$test" "build/$name.c"
	$CC $CFLAGS -o "build/$name" "build/$name.c" $LIBS
	"build/$name" > "build/$name.out" 2>&1 || true
	# and again with a collection before every allocation: a root the C does not keep shows here
	YEL_GC_STRESS=1 "build/$name" > "build/$name.stress.out" 2>&1 || true
	if ! cmp -s "tests/$name.out" "build/$name.out"; then
		echo "  FAIL $name: diff tests/$name.out build/$name.out"
		failed=1
	elif ! cmp -s "tests/$name.out" "build/$name.stress.out"; then
		echo "  FAIL $name (YEL_GC_STRESS=1): diff tests/$name.out build/$name.stress.out"
		failed=1
	else
		echo "  ok   $name"
	fi
done
# tests/testing: yelc test's runner of the package's tests (std:testing's expectations; three that
# fail, each ending only its own process, a snapshot that differs among them; an async one; ones in
# a module), its report as tests/testing.runner.out has it (built as a program instead, its tests
# left out: above). Its snapshots (tests/testing/snapshots) are all there: none is written
build/yelc2 test tests/testing build/testing-runner.c
$CC $CFLAGS -o build/testing-runner build/testing-runner.c $LIBS
build/testing-runner > build/testing-runner.out 2>&1 || true
if cmp -s tests/testing.runner.out build/testing-runner.out; then
	echo "  ok   testing (yelc test)"
else
	echo "  FAIL testing (yelc test): diff tests/testing.runner.out build/testing-runner.out"
	failed=1
fi
# tests/host: the host's interfaces (imports, the clock's waits, resources, the filesystem, TCP, name
# lookups), natively std's host: yelc test's runner of them, every test passing
build/yelc2 test tests/host build/host-tests.c
$CC $CFLAGS -o build/host-tests build/host-tests.c $LIBS
if build/host-tests > build/host-tests.out 2>&1; then
	echo "  ok   host ($(tail -n 1 build/host-tests.out))"
else
	echo "  FAIL host: build/host-tests.out"
	failed=1
fi
# std's own tests, beside it (a package's *.test.yel): yelc test of std's core (runtime/std/core,
# its folders one package) and of each of its packages that has any, each run, and again under
# YEL_GC_STRESS=1 (a collection before every allocation): every test passes
for package in runtime/std/*/; do
	package=${package%/}
	# (core's are in its folders)
	ls "$package"/*.test.yel > /dev/null 2>&1 || ls "$package"/*/*.test.yel > /dev/null 2>&1 || continue
	name=std$(echo "${package#runtime/std}" | tr / -)
	build/yelc2 test "$package" "build/$name-tests.c"
	$CC $CFLAGS -o "build/$name-tests" "build/$name-tests.c" $LIBS
	if "build/$name-tests" > "build/$name-tests.out" 2>&1 && YEL_GC_STRESS=1 "build/$name-tests" > "build/$name-tests.stress.out" 2>&1; then
		echo "  ok   $name ($(tail -n 1 "build/$name-tests.out"))"
	else
		echo "  FAIL $name: build/$name-tests.out, build/$name-tests.stress.out"
		failed=1
	fi
done
# the compiler's own tests (compiler/*.test.yel): small programs built as IR in memory
# (analysis.lower), and what its passes make of them (escape.yel's flow, releases.yel's ends let go)
build/yelc2 test compiler build/compiler-tests.c
$CC $CFLAGS -o build/compiler-tests build/compiler-tests.c $LIBS
if build/compiler-tests > build/compiler-tests.out 2>&1; then
	echo "  ok   compiler ($(tail -n 1 build/compiler-tests.out))"
else
	echo "  FAIL compiler: build/compiler-tests.out"
	failed=1
fi
# tests/fmt/*.yel: yelc --fmt lays each out as tests/fmt/<name>.out has it, and lays that out again
# as it is
for test in tests/fmt/*.yel; do
	name=fmt-$(basename "$test" .yel)
	cp "$test" "build/$name.yel"
	build/yelc2 --fmt "build/$name.yel" > /dev/null
	cp "build/$name.yel" "build/$name.again.yel"
	build/yelc2 --fmt "build/$name.again.yel" > /dev/null
	if ! cmp -s "${test%.yel}.out" "build/$name.yel"; then
		echo "  FAIL $name: diff ${test%.yel}.out build/$name.yel"
		failed=1
	elif ! cmp -s "build/$name.yel" "build/$name.again.yel"; then
		echo "  FAIL $name: formatted again it changes: diff build/$name.yel build/$name.again.yel"
		failed=1
	else
		echo "  ok   $name"
	fi
done
# --full: the compiler compiling itself with a collection every GC_STRESS allocations makes the same C
if [ "$full" -eq 1 ]; then
	YEL_GC_STRESS=${GC_STRESS:-5000} build/yelc2 compiler build/yelc3.stress.c
	if cmp -s build/yelc3.c build/yelc3.stress.c; then
		echo "  ok   self-compile (YEL_GC_STRESS=${GC_STRESS:-5000})"
	else
		echo "  FAIL self-compile (YEL_GC_STRESS=${GC_STRESS:-5000}): diff build/yelc3.c build/yelc3.stress.c"
		failed=1
	fi
fi
# tests/errors/*.yel must not compile: yelc2 reports each error their first lines name (// error:
# line:col: message: the diagnostic's message, at that place), each // help: or // note: line among
# them, and no other error (one more is an echo of these, which the compiler should not tell)
for test in tests/errors/*.yel; do
	name=errors-$(basename "$test" .yel)
	if build/yelc2 "$test" "build/$name.c" > "build/$name.err" 2>&1; then
		echo "  FAIL $name: compiled, expected: $(head -n 1 "$test")"
		failed=1
		continue
	fi
	ok=1
	wanted=0
	while IFS= read -r line; do
		case "$line" in
		"// error: "*)
			want=${line#// error: }
			wanted=$((wanted + 1))
			place=${want%%: *}
			message=${want#*: }
			case "$place" in
			*/*) at=$place ;;
			*[!0-9:]* | "$want") message=$want; at=$test ;;
			*) at=$test:$place ;;
			esac
			grep -qxF -- "error: $message" "build/$name.err" || ok=0
			grep -qF -- "--> $at" "build/$name.err" || ok=0
			;;
		"// help: "* | "// note: "*)
			kind=${line#// }
			kind=${kind%%: *}
			grep -qF -- "= $kind: ${line#// *: }" "build/$name.err" || ok=0
			;;
		*) break ;;
		esac
	done < "$test"
	# the errors told (past one, the last line counts them)
	told=$(grep -c '^error: ' "build/$name.err" || true)
	if [ "$told" -gt 1 ]; then told=$((told - 1)); fi
	[ "$told" -eq "$wanted" ] || ok=0
	if [ $ok = 1 ]; then
		echo "  ok   $name"
	else
		echo "  FAIL $name: expected $wanted errors ($(head -n 1 "$test")), got:"
		cat "build/$name.err"
		failed=1
	fi
done
# the bitcode backend (compiler/llvm.yel): tests/bitcode/*.yel built both ways, doing the same (where
# clang is there: it builds the bitcode)
if command -v clang > /dev/null; then
	if YELC=build/yelc2 sh tools/bitcode-test.sh > build/bitcode-test.log 2>&1; then bitcode_failed=0; else bitcode_failed=1; fi
	sed 's/^ok   /  ok   bitcode /;s/^FAIL /  FAIL bitcode /' build/bitcode-test.log
	if [ $bitcode_failed = 1 ]; then failed=1; fi
fi
# with WASI_SDK set (a wasi-sdk directory) and wasmtime installed: the compiler as wasm compiles
# itself, collecting in linear memory, with a collection every 50 allocations, and makes the same C
if [ -n "${WASI_SDK:-}" ]; then
	# WASI 0.3 components: tests/host's tests as one (each a call), commands whose stdio and exit are
	# theirs (the same output as natively), and tests/component
	# (exports only) called by the host, each call's result as tests/component.calls.out has it
	"$WASI_SDK/bin/clang" --target=wasm32-wasip3 $OPT -Iruntime -c -o build/yel-wasm.o runtime/yel.c
	wasm() { "$WASI_SDK/bin/clang" --target=wasm32-wasip3 $OPT -Iruntime -Wl,-z,stack-size=8388608 "$@" build/yel-wasm.o -lm; }
	# a component that hangs fails (killed after two minutes) instead of stopping the run
	run() { perl -e 'alarm 120; exec @ARGV' wasmtime run -W component-model-async=y -S p3=y "$@"; }
	# stdin to stdout as streams (a stream and a future in a tuple: nested ones cross too)
	build/yelc2 tests/cat.yel build/cat.c --wit build/cat-wit
	wasm -Wl,--component-type,build/cat-wit -o build/cat.wasm build/cat.c
	if run build/cat.wasm < tests/cat.in | cmp -s tests/cat.component.out -; then echo "  ok   stdin to stdout (component)"; else echo "  FAIL stdin to stdout (component)"; failed=1; fi
	# a stream from the host read in part: its end let go after its last use (else stdin's future
	# never comes: the host's writer waits for a reader that is gone)
	build/yelc2 tests/stdin-drop.yel build/stdin-drop.c --wit build/stdin-drop-wit
	wasm -Wl,--component-type,build/stdin-drop-wit -o build/stdin-drop.wasm build/stdin-drop.c
	if head -c 2000000 /dev/zero | run build/stdin-drop.wasm | cmp -s tests/stdin-drop.component.out -; then echo "  ok   host ends let go (component)"; else echo "  FAIL host ends let go (component)"; failed=1; fi
	# fixed-length lists across a component boundary: tests/fixed-caller's imports plugged (wac) into
	# tests/fixed-api's exports (wasmtime's --invoke cannot write them yet)
	if command -v wac > /dev/null; then
		build/yelc2 tests/fixed-api build/fixed-api.c --wit build/fixed-api-wit
		wasm -mexec-model=reactor -Wl,--component-type,build/fixed-api-wit -o build/fixed-api.wasm build/fixed-api.c
		build/yelc2 tests/fixed-caller build/fixed-caller.c -I build/fixed-api-wit --wit build/fixed-caller-wit
		wasm -Wl,--component-type,build/fixed-caller-wit -o build/fixed-caller.wasm build/fixed-caller.c
		wac plug build/fixed-caller.wasm --plug build/fixed-api.wasm -o build/fixed-lists.wasm
		if run -W component-model-fixed-length-lists=y build/fixed-lists.wasm | cmp -s tests/fixed-lists.component.out -; then
			echo "  ok   fixed-length lists (components)"
		else
			echo "  FAIL fixed-length lists (components)"
			failed=1
		fi
		# a resource exported: tests/res-caller's import of it plugged into tests/res-api (two
		# handles made, methods called through them, each dropped: its destructor runs)
		build/yelc2 tests/res-api build/res-api.c --wit build/res-api-wit
		wasm -mexec-model=reactor -Wl,--component-type,build/res-api-wit -o build/res-api.wasm build/res-api.c
		build/yelc2 tests/res-caller build/res-caller.c -I build/res-api-wit --wit build/res-caller-wit
		wasm -Wl,--component-type,build/res-caller-wit -o build/res-caller.wasm build/res-caller.c
		wac plug build/res-caller.wasm --plug build/res-api.wasm -o build/resources-exported.wasm
		if run build/resources-exported.wasm | cmp -s tests/resources-exported.component.out -; then
			echo "  ok   resources exported (components)"
		else
			echo "  FAIL resources exported (components)"
			failed=1
		fi
		# a UI component lowered by hand as templates will be (tests/ui-counter), its yel:ui/dom given
		# by a stand-in host (tests/ui-dom: each call printed) and driven as the shell would
		# (tests/ui-driver): the wire, call by call
		build/yelc2 tests/ui-dom build/ui-dom.c --wit build/ui-dom-wit
		wasm -mexec-model=reactor -Wl,--component-type,build/ui-dom-wit -o build/ui-dom.wasm build/ui-dom.c
		build/yelc2 tests/ui-counter build/ui-counter.c -I tests/ui-wit --wit build/ui-counter-wit
		wasm -mexec-model=reactor -Wl,--component-type,build/ui-counter-wit -o build/ui-counter.wasm build/ui-counter.c
		build/yelc2 tests/ui-driver build/ui-driver.c -I build/ui-counter-wit -I tests/ui-wit --wit build/ui-driver-wit
		wasm -Wl,--component-type,build/ui-driver-wit -o build/ui-driver.wasm build/ui-driver.c
		wac plug build/ui-counter.wasm --plug build/ui-dom.wasm -o build/ui-counter-hosted.wasm
		wac plug build/ui-driver.wasm --plug build/ui-counter-hosted.wasm -o build/ui.wasm
		if run build/ui.wasm 2>&1 | cmp -s tests/ui.component.out -; then
			echo "  ok   ui component (components)"
		else
			echo "  FAIL ui component (components)"
			failed=1
		fi
		# streams and futures across WIT (WASI 0.3): tests/stream-caller's imports plugged into
		# tests/stream-api's exports, each direction (a stream and a future given, and taken)
		build/yelc2 tests/stream-api build/stream-api.c --wit build/stream-api-wit
		wasm -mexec-model=reactor -Wl,--component-type,build/stream-api-wit -o build/stream-api.wasm build/stream-api.c
		build/yelc2 tests/stream-caller build/stream-caller.c -I build/stream-api-wit --wit build/stream-caller-wit
		wasm -Wl,--component-type,build/stream-caller-wit -o build/stream-caller.wasm build/stream-caller.c
		wac plug build/stream-caller.wasm --plug build/stream-api.wasm -o build/streams.wasm
		if run build/streams.wasm | cmp -s tests/streams.component.out -; then
			echo "  ok   streams and futures (components)"
		else
			echo "  FAIL streams and futures (components)"
			failed=1
		fi
	fi
	build/yelc2 tests/component build/component.c --wit build/component-wit
	wasm -mexec-model=reactor -Wl,--component-type,build/component-wit -o build/component.wasm build/component.c
	while read -r call; do run --invoke "$call" build/component.wasm; done < tests/component.calls > build/component.calls.out 2>&1
	if cmp -s tests/component.calls.out build/component.calls.out; then echo "  ok   component (exports)"; else echo "  FAIL component: diff tests/component.calls.out build/component.calls.out"; failed=1; fi
	# tests/host as a component: the host's own interfaces (the network and name lookups granted)
	if WASMTIME_FLAGS="-S inherit-network=y -S allow-ip-name-lookup=y" YELC=build/yelc2 tools/test-component.sh tests/host > build/host-component.out 2>&1; then
		echo "  ok   host ($(tail -n 1 build/host-component.out), component)"
	else
		echo "  FAIL host (component): build/host-component.out"
		failed=1
	fi
	# yelc test for a component: the runner an export interface (tests: names, run), each test a call
	# of its own on a fresh instance (tools/test-component.sh), its report the native runner's
	YELC=build/yelc2 tools/test-component.sh tests/testing > build/testing-component.out 2>&1 || true
	if cmp -s tests/testing.runner.out build/testing-component.out; then
		echo "  ok   testing (yelc test, component)"
	else
		echo "  FAIL testing (yelc test, component): diff tests/testing.runner.out build/testing-component.out"
		failed=1
	fi
	# an export module's private func (@(private)) is the component's own: not in its WIT
	if grep -rq "tally-step" build/component-wit; then echo "  FAIL component: tally-step (@(private)) is in its WIT"; failed=1; else echo "  ok   component (a private func not exported)"; fi
	# a view (examples/hello: yel-solid's hello as a yel view) built as the shell's hosts take it
	# (WASI 0.2, Y_HOSTED), its WIT the resource a view lowers to
	if YELC=build/yelc2 sh examples/hello/build.sh > build/hello-example.log 2>&1 \
		&& grep -q "resource app" examples/hello/build/hello-wit/*.wit \
		&& grep -q "dispatch: func(handler-id: u32);" examples/hello/build/hello-wit/*.wit; then
		echo "  ok   view hello example (component)"
	else
		echo "  FAIL view hello example (component): build/hello-example.log"
		failed=1
	fi
	# --full: the compiler as a WASI 0.3 component (its files through wasi:filesystem, the host's)
	# compiles itself, under stress (an 8 MiB stack, as native has: wasm-ld's default is 64 KiB,
	# less than the parser's recursion needs)
	if [ "$full" -eq 1 ]; then
		build/yelc2 compiler build/yelc-component.c --wit build/yelc-wit
		wasm -Wl,--component-type,build/yelc-wit -o build/yelc.wasm build/yelc-component.c
		wasmtime run -W component-model-async=y -S p3=y --dir . --env YEL_GC_STRESS=${GC_STRESS_WASM:-20000} \
			build/yelc.wasm compiler build/yelc3.wasm.c runtime/prelude.yel
		if cmp -s build/yelc3.c build/yelc3.wasm.c; then
			echo "  ok   wasm self-compile (YEL_GC_STRESS=${GC_STRESS_WASM:-20000})"
		else
			echo "  FAIL wasm self-compile: diff build/yelc3.c build/yelc3.wasm.c"
			failed=1
		fi
	fi
fi
[ "$failed" -eq 0 ] || exit 1

if [ "${1:-}" = --update-seed ]; then
	if cmp -s build/yelc3.c seed/yelc.c; then
		echo "seed: unchanged"
	else
		cp build/yelc3.c seed/yelc.c
		# the new seed is made for runtime/: the runtime an older one needed goes
		rm -rf seed/runtime
		echo "seed: updated (seed/yelc.c)"
	fi
fi

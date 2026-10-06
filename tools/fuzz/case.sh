#!/bin/sh
# One fuzz case: yelfuzz's program for a seed, through the compiler, and (full) built and run each
# way, all of which must do the same. Prints "ok <seed>" or "<kind> <seed>", the case kept in
# build/fuzz/<kind>/<seed>.yel with its log (tools/fuzz/run.sh says what each kind is).
#
#   tools/fuzz/case.sh <seed> <size> <check|compile|bitcode|full>
set -u
seed=$1
size=$2
mode=$3
YELC=${YELC:-build/yelc2}
CC=${CC:-cc}
CFLAGS="-Iruntime -Iruntime/libuv/include"
# the runtime's definitions: build/yel.o (tools/fuzz/run.sh builds it)
LIBS="build/yel.o build/libuv.a -lm -lpthread"
# LLVM's opt, for its verifier (tools/fuzz/run.sh finds it)
OPT=${OPT:-opt}
if [ "$(uname -s)" = Linux ]; then LIBS="$LIBS -ldl -lrt"; fi
out=build/fuzz
work=$out/work/$seed
rm -rf "$work"
mkdir -p "$work"
# a command, killed after $1 seconds
limit() { seconds=$1; shift; perl -e 'alarm shift; exec @ARGV' "$seconds" "$@"; }

# the case kept, and said; done
keep() {
	mkdir -p "$out/$1"
	cp "$work/case.yel" "$out/$1/$seed.yel"
	cat "$work"/*.log > "$out/$1/$seed.log" 2>/dev/null
	echo "$1 $seed"
	rm -rf "$work"
	exit 0
}

# a compiler run: a panic or a signal (or no end) is the compiler's; an error, a valid program
# refused
compiled() {
	status=$1
	log=$2
	if [ "$status" -eq 0 ]; then return 0; fi
	if grep -q '^panic:' "$log" || [ "$status" -gt 1 ]; then keep compiler-panic; fi
	known "$log"
	keep rejected
}

# a log matching a known issue (tools/fuzz/known.txt): kept as that issue's
known() {
	grep -v '^#' tools/fuzz/known.txt | while IFS='	' read -r name pattern; do
		[ -n "$name" ] && grep -qE "$pattern" "$1" && echo "$name" && break
	done > "$work/known"
	if [ -s "$work/known" ]; then keep "known-$(cat "$work/known")"; fi
}

# the bitcode backend's build of the case (--backend bitcode, checked by LLVM's verifier, linked with
# the runtime's bitcode at -O0 or -O2 by seed), run as it is and under GC stress: each the same as
# the C build's -O0 run
bitcode() {
	limit 60 "$YELC" "$work/case.yel" "$work/case.bc" --backend bitcode > "$work/bitcode-compile.log" 2>&1
	status=$?
	if [ "$status" -ne 0 ]; then
		if grep -q '^panic:' "$work/bitcode-compile.log" || [ "$status" -gt 1 ]; then keep compiler-panic; fi
		known "$work/bitcode-compile.log"
		if grep -q 'is not done yet' "$work/bitcode-compile.log"; then keep bitcode-unsupported; fi
		keep bitcode-error
	fi
	"$OPT" -passes=verify -disable-output "$work/case.bc" > "$work/bitcode-verify.log" 2>&1
	[ -s "$work/bitcode-verify.log" ] && keep bitcode-invalid
	level=$(echo "O0 O2" | cut -d' ' -f$((seed % 2 + 1)))
	clang -$level -w -Wno-override-module -o "$work/bc" "$work/case.bc" build/fuzz/runtime.bc build/libuv.a -lm -lpthread > "$work/bitcode-cc.log" 2>&1 || keep bitcode-build
	run bc "$work/bc"
	# (env: the stress the run's alone; an assignment before a shell func stays in the script's
	# environment after it, and would run every later command, the compiler too, under it)
	run bc-stress env YEL_GC_STRESS=7 "$work/bc"
	for name in bc bc-stress; do
		if ! cmp -s "$work/o0.result" "$work/$name.result"; then
			{ echo "== $name (bitcode -$level) differs from the C build's o0:"; diff "$work/o0.result" "$work/$name.result" | head -20; } >> "$work/bitcode-differs.log"
		fi
	done
	[ -s "$work/bitcode-differs.log" ] && keep bitcode-differs
}

# a build's run: its stdout and stderr, then (on a line of its own, in <name>.status too) its exit
# code or the signal that ended it ("signal 9": killed after 10 seconds too). Told apart by its
# wait status, not by its exit code: main may return 255
run() {
	name=$1
	shift
	perl -e '$status = shift; $SIG{ALRM} = sub { kill 9, $pid }; alarm 10; $pid = fork; if (!$pid) { exec @ARGV; exit 127 } waitpid($pid, 0); open(S, ">", $status); print S (($? & 127) ? "signal " . ($? & 127) : "exit " . ($? >> 8)), "\n"; close S' "$work/$name.status" "$@" > "$work/$name.result" 2>&1
	{ echo; echo "== $(cat "$work/$name.status")"; } >> "$work/$name.result"
}

build/yelfuzz "$seed" "$size" > "$work/case.yel"
if [ "$mode" = check ]; then
	limit 60 "$YELC" check "$work/case.yel" > "$work/check.log" 2>&1
	compiled $? "$work/check.log"
	echo "ok $seed"
	rm -rf "$work"
	exit 0
fi
limit 60 "$YELC" "$work/case.yel" "$work/case.c" > "$work/compile.log" 2>&1
compiled $? "$work/compile.log"
if [ "$mode" = compile ]; then
	echo "ok $seed"
	rm -rf "$work"
	exit 0
fi
$CC -O0 -w $CFLAGS -o "$work/o0" "$work/case.c" $LIBS > "$work/cc.log" 2>&1 || keep c-error
run o0 "$work/o0"
# killed for taking too long (the program may not end: nothing to compare), or another signal
if grep -q '^signal 9$' "$work/o0.status"; then keep timeout; fi
if grep -q '^signal ' "$work/o0.status"; then keep crash; fi
if [ "$mode" = bitcode ]; then
	bitcode
	echo "ok $seed"
	rm -rf "$work"
	exit 0
fi
# one other build, in turn by seed (each seed one: the batch covers them all), and GC stress
other=$(echo "o2 opt-none opt-size opt-speed" | cut -d' ' -f$((seed % 4 + 1)))
case "$other" in
o2) $CC -O2 -w $CFLAGS -o "$work/other" "$work/case.c" $LIBS > "$work/cc-other.log" 2>&1 ;;
*)
	limit 60 "$YELC" "$work/case.yel" "$work/other.c" --opt "${other#opt-}" > "$work/compile-other.log" 2>&1
	compiled $? "$work/compile-other.log"
	$CC -O0 -w $CFLAGS -o "$work/other" "$work/other.c" $LIBS > "$work/cc-other.log" 2>&1
	;;
esac
if [ -x "$work/other" ]; then run other "$work/other"; else echo "the build failed" > "$work/other.result"; fi
run stress env YEL_GC_STRESS=7 "$work/o0"
for name in other stress; do
	if ! cmp -s "$work/o0.result" "$work/$name.result"; then
		{ echo "== $name ($other) differs from o0:"; diff "$work/o0.result" "$work/$name.result" | head -20; } >> "$work/differs.log"
	fi
done
[ -s "$work/differs.log" ] && keep differs
[ "${BITCODE:-1}" = 1 ] && bitcode
echo "ok $seed"
rm -rf "$work"

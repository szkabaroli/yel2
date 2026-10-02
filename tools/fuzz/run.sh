#!/bin/sh
# Fuzz the compiler with yelfuzz's programs (valid yel, made from a seed), a case on each core
# (tools/fuzz/case.sh). What goes wrong is kept in build/fuzz/<kind>/<seed>.yel with its log:
#
#   compiler-panic   the compiler panicked or crashed (a signal), or did not finish
#   rejected         the compiler refused a valid program (its errors in the log)
#   c-error          the C it made does not build
#   crash            the program died of a signal (its -O0 build)
#   timeout          it did not finish in 10 seconds (its -O0 build: a program that may not end)
#   differs          another build did otherwise: -O2, --opt none|size|speed (one a seed, in
#                    turn), GC stress
#   bitcode-*        the bitcode backend's (--backend bitcode): unsupported (a part it does not
#                    lower yet), error (it refused otherwise), invalid (LLVM's verifier refused the
#                    module), build (clang could not build it), differs (its run, -O0 or -O2 by seed,
#                    or under GC stress, did otherwise than the C build's -O0)
#
#   tools/fuzz/run.sh [--check | --compile | --bitcode] [-j jobs] [count] [first-seed] [size]
#
# --check: yelc check only (the front end and the readonly and move checks: the fastest);
# --compile: to C (the optimizer and C's making too); --bitcode: the C build's -O0 against the
# bitcode one's; else full: built and run each way, the bitcode build too (BITCODE=0: not it).
# OPT: LLVM's opt (its verifier; default: the one on PATH, else Homebrew's).
# Defaults: 1000 cases from seed 1, size 3, a job for each core. YELC: the compiler (default
# build/yelc2), which builds the fuzzer into build/yelfuzz.
set -u
cd "$(dirname "$0")/../.."
mode=full
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
while [ $# -gt 0 ]; do
	case "$1" in
	--check) mode=check; shift ;;
	--compile) mode=compile; shift ;;
	--bitcode) mode=bitcode; shift ;;
	-j) jobs=$2; shift 2 ;;
	*) break ;;
	esac
done
count=${1:-1000}
first=${2:-1}
size=${3:-3}
YELC=${YELC:-build/yelc2}
export YELC
"$YELC" tools/fuzz build/yelfuzz.c || { echo "the fuzzer does not compile"; exit 1; }
# the runtime's definitions, for the fuzzer and each case (tools/fuzz/case.sh links them)
${CC:-cc} -O1 -Iruntime -Iruntime/libuv/include -c -o build/yel.o runtime/yel.c || exit 1
${CC:-cc} -O1 -Iruntime -Iruntime/libuv/include -o build/yelfuzz build/yelfuzz.c build/yel.o build/libuv.a -lm -lpthread || exit 1
mkdir -p build/fuzz
# the bitcode build's: LLVM's verifier, and the runtime as bitcode
if [ "$mode" = full ] || [ "$mode" = bitcode ]; then
	OPT=${OPT:-$(command -v opt || echo /opt/homebrew/opt/llvm/bin/opt)}
	[ -x "$OPT" ] || { echo "no LLVM opt (for its verifier): set OPT"; exit 1; }
	export OPT
	clang -O2 -c -emit-llvm -Iruntime -Iruntime/libuv/include -o build/fuzz/runtime.bc runtime/yel.c || exit 1
fi
start=$(date +%s)
seq "$first" $((first + count - 1)) | xargs -P "$jobs" -I{} sh tools/fuzz/case.sh {} "$size" "$mode" > build/fuzz/last.txt
seconds=$(( $(date +%s) - start ))
kept=$(grep -vc '^ok ' build/fuzz/last.txt)
echo "$count cases ($mode, seeds $first to $((first + count - 1)), size $size, $jobs jobs) in ${seconds}s: $kept kept"
grep -v '^ok ' build/fuzz/last.txt | sort | awk '{ n[$1]++; s[$1] = s[$1] " " $2 } END { for (k in n) printf "  %-15s %d:%s\n", k, n[k], substr(s[k], 1, 70) }'

#!/bin/sh
# Bootstraps yelc, the yel compiler written in yel, from its seed: the C a previous yelc made of
# itself (seed/yelc.c). Nothing but a C compiler is needed. This builds the seed (stage 0,
# build/yelc0) and the runtime every program links (build/yel.o, build/libuv.a), then hands the
# rest to build.yel (yelc build): stages 1 to 3, the fixed point and every test, each step run
# once what it needs is done, together, and made again only where what goes into it changed.
#
#   ./bootstrap.sh                 quick, for development: -O0, the stages and every test (each
#                                  program also under GC stress; WASI_SDK=<dir>: the component
#                                  tests too)
#   ./bootstrap.sh --full          -O2, and the compiler compiling itself under GC stress, natively
#                                  and (WASI_SDK) as wasm; GC_STRESS=n: collect every n allocations
#                                  (default 5000: a self-compile makes ~13 million, each collection
#                                  marks the whole live heap), GC_STRESS_WASM=n as wasm (default
#                                  20000: wasmtime's run is the slow one; lower both when the GC's
#                                  or the ABI's rooting changes: the tests already collect before
#                                  every one)
#   ./bootstrap.sh --update-seed   --full, then, every test passed, yelc3.c the new seed
#   ./bootstrap.sh [--full] <step>...   build.yel's steps instead (build/yelc0 build --help lists
#                                  them: test-language, test-run, test-packages, ...)
#
# A new language feature lands in two steps, because the seed only compiles what it knew: first
# teach yelc the feature without using it in compiler/, and update the seed; then use it.
set -eu
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-cc}
full=false
steps=test
case "${1:-}" in
--full) full=true; shift ;;
--update-seed) full=true; steps=update-seed; shift ;;
esac
[ $# -gt 0 ] && steps="$*"
if [ $full = true ]; then OPT=${OPT:--O2}; else OPT=${OPT:--O0}; fi
# a native build links libuv (runtime/libuv: the native host's event loop), built once
sh runtime/libuv/build.sh build
CFLAGS="$OPT -Iruntime -Iruntime/libuv/include"
# what goes into a file this makes, as one line (its sources' checksum, the C compiler and flags):
# each is made again only where its stamp (beside it) differs. A linked program is not the same
# bytes twice, and build.yel keys stage 1 by the seed's
stamp() { echo "$(cat "$@" | cksum) $CC $OPT"; }
# the runtime's definitions (runtime/yel.c), compiled once and linked with every program
if [ "$(cat build/yel.o.stamp 2> /dev/null)" != "$(stamp runtime/yel.c runtime/*.h)" ]; then
	$CC $CFLAGS -c -o build/yel.o runtime/yel.c
	stamp runtime/yel.c runtime/*.h > build/yel.o.stamp
fi
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

# (the seed, and the runtime it builds with)
seed=$(if [ -d seed/runtime ]; then stamp seed/yelc.c seed/runtime/*; else stamp seed/yelc.c runtime/yel.c runtime/*.h; fi)
if [ "$(cat build/yelc0.stamp 2> /dev/null)" != "$seed" ]; then
	echo "stage 0: cc seed/yelc.c"
	$CC $SEED_CFLAGS -o build/yelc0 seed/yelc.c $SEED_LIBS
	echo "$seed" > build/yelc0.stamp
fi
# A seed for another runtime cannot build the build program for this one: stage 1 is built here
# then, and runs it (its stage 1 then is the stage 2 above)
builder=build/yelc0
if [ -d seed/runtime ]; then
	echo "stage 1: yelc0 compiler -> build/yelc1.c (the seed's runtime)"
	build/yelc0 compiler build/yelc1.c
	$CC $SEED_CFLAGS -o build/yelc1 build/yelc1.c $SEED_LIBS
	builder=build/yelc1
fi

# (the steps one word each)
exec "$builder" build -p build --summary "-Dseed=$builder" "-Dc-optimize=$OPT" "-Dfull=$full" \
	"-Dgc-stress=${GC_STRESS:-5000}" "-Dgc-stress-wasm=${GC_STRESS_WASM:-20000}" $steps

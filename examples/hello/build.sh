#!/bin/sh
# The hello component (hello.yel: yel-solid's hello example, a counter) as a WASI 0.2 component for
# the shell's hosts: build/hello.wasm, exporting yel:hello/app-component (resource app) and
# importing yel:ui/dom and only what those hosts grant (the streams, the clocks: Y_HOSTED leaves out
# the environment and exit).
#
#   WASI_SDK=<dir> examples/hello/build.sh      (YELC: the compiler, default build/yelc2)
#
# Run it as yel-solid's example is run: jco transpile build/hello.wasm --instantiation async, then
# packages/yel-solid/examples/hello/harness.mjs against the transpiled output.
set -eu
cd "$(dirname "$0")/../.."
YELC=${YELC:-build/yelc2}
: "${WASI_SDK:?set WASI_SDK to the wasi-sdk directory}"
out=examples/hello/build
mkdir -p "$out"
"$YELC" examples/hello "$out/hello.c" -I examples/hello/wit --wit "$out/hello-wit"
cc="$WASI_SDK/bin/clang --target=wasm32-wasip2 -O2 -DY_HOSTED -Iruntime"
$cc -c -o "$out/yel.o" runtime/yel.c
$cc -mexec-model=reactor -Wl,-z,stack-size=1048576 -Wl,--component-type,"$out/hello-wit" -o "$out/hello.wasm" "$out/hello.c" "$out/yel.o" -lm
echo "$out/hello.wasm"

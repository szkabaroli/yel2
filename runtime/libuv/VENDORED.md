# Vendored: libuv

[libuv](https://github.com/libuv/libuv) `v1.53.0` (`840404ce8ba7cc0204be52389a6cfff9f2c90fb6`), MIT
(`LICENSE`, `LICENSE-extra`). The files are unmodified: `include/`, and `src/` without `src/win/`
(Windows is not built yet) or anything else of the repository. The same copy as Porffor's
(`external/porffor/runtime/c/libuv`), so both native hosts stand on one libuv.

The native host of a yel program: its event loop (the executor waits on it when every task waits)
and the system's calls behind WASI's interfaces natively (`runtime/wasi-native.h`: the file system
over its `uv_fs_*` requests). A component does not use it: there the WASI host drives everything.

`build.sh <dir>` compiles it once into `<dir>/libuv.a` (the files libuv's `CMakeLists.txt` lists for
macOS or Linux); a native build links that archive, and `-lpthread` (and `-ldl -lrt` on Linux).

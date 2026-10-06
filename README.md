# yel2

[![Test](https://github.com/szkabaroli/yel2/actions/workflows/test.yml/badge.svg)](https://github.com/szkabaroli/yel2/actions/workflows/test.yml)
[![Release](https://github.com/szkabaroli/yel2/actions/workflows/release.yml/badge.svg)](https://github.com/szkabaroli/yel2/actions/workflows/release.yml)
[![Pre-release](https://img.shields.io/github/v/release/szkabaroli/yel2?include_prereleases&label=pre-release)](https://github.com/szkabaroli/yel2/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

The self-hosted compiler for [yel](https://github.com/szkabaroli/yel): a small, statically typed
language whose modules are WIT interfaces. yel2 is written in yel, compiles itself, and builds
native programs (through C or LLVM bitcode) and WebAssembly components (WASI 0.2 and 0.3).

> **Pre-alpha.** Anything may change, the language included.

```yel
record planet { name: string, moons: s64, }

describe: func(p: planet) -> string {
    match p.moons {
        0 => "{p.name} has no moons",
        1 => "{p.name} has one moon",
        let n => "{p.name} has {n} moons",
    }
}

main: func() {
    let planets: list<planet> = [
        { name: "Mercury", moons: 0, },
        { name: "Earth", moons: 1, },
        { name: "Mars", moons: 2, },
    ];
    for line in planets.map({ p -> describe(p) }) { print("{line}\n"); }
}
```

## Features

- **Self-hosted.** Bootstrapped from a C seed. Each build checks the fixed point: the compiler's
  C, compiled by itself, is the same again.
- **WIT-native modules.** A package's modules are its component's WIT interfaces, and a WIT
  interface can be included like a module.
- **Native and WebAssembly.** C or LLVM bitcode for native programs. WASI 0.2 and 0.3 components,
  async across the component boundary included.
- **Memory managed for you.** A precise garbage collector, with escape analysis that keeps records
  on the stack where it can.
- **Readonly and observable types.** Deep `T & readonly`, and `@(observable)` resources that tell
  what read them when they change.
- **Views.** A UI view is a module whose WIT a host mounts, kept current by reactive effects.
- **Tooling.** A formatter (`yelc --fmt`), tests (`@(test)`), a build system written in yel
  (`build.yel`), and a language server (`yel-lsp`).

## Install

Pre-alpha builds for Linux and macOS, x64 and arm64, from the
[releases](https://github.com/szkabaroli/yel2/releases).

**Homebrew** (macOS, Linux):

```sh
brew tap szkabaroli/yel2 https://github.com/szkabaroli/yel2
brew install yel
```

**Install script** (macOS, Linux), into `~/.yel`:

```sh
curl -fsSL https://raw.githubusercontent.com/szkabaroli/yel2/main/install.sh | sh
```

`YEL_VERSION=pre-alpha-3` installs a given release and `YEL_INSTALL=<dir>` puts it elsewhere.
Then add `~/.yel/bin` to your `PATH`.

**Manually:** download `yel-<os>-<cpu>.tar.gz` from a release, and keep its directory whole:
`yelc` finds its runtime beside it, or in `YEL_HOME`.

**From source:** see [Building from source](#building-from-source).

Windows is not supported yet. Every install needs a C compiler (`cc`, or `CC`), which builds what
`yelc` makes.

## Usage

```sh
yelc run hello.yel [args]    # run a program, interpreted: no C made, no C compiler needed
yelc hello.yel hello.c       # a program (a file, or a package's directory) to C
yelc build                   # the project's build.yel: its steps (yelc build --help)
yelc check src               # errors only, nothing made
yelc lint src                # the project's lint.yel's rules on the package, interpreted
yelc test src out.c          # the package's @(test) funcs as a program
yelc --fmt src               # format in place
```

To run a program compiled to C, link it with the runtime:

```sh
cc -I"$YEL/runtime" -I"$YEL/runtime/libuv/include" hello.c "$YEL/build/yel.o" "$YEL/build/libuv.a" -lm -lpthread -o hello
```

where `$YEL` is the release directory. Add `-ldl -lrt` on Linux.

## Building from source

Only a C compiler is needed:

```sh
./bootstrap.sh               # the seed, stages 1 to 3, the fixed point, every test (about 1 minute)
./bootstrap.sh --full        # -O2, and the compiler compiling itself under GC stress
build/yelc2 --version        # the compiler it made
```

`WASI_SDK=<dir> ./bootstrap.sh` adds the WebAssembly component tests (wasi-sdk 34, wasmtime 49,
wac). LLVM's `clang` and `opt` on `PATH` enable the bitcode backend's tests.

## Editor support

[Zed](editors/zed/README.md), with Tree-sitter highlighting ([editors/tree-sitter-yel](editors/tree-sitter-yel))
and the compiler's diagnostics, hovers and formatting through `yel-lsp` ([tools/lsp](tools/lsp)).

## Documentation

- [The language](docs/language.md): everything yel2 compiles today
- [Internals](docs/internals.md): how it bootstraps, the repository's layout, CI and releases, what is next

## License

[MIT](LICENSE). The bundled libuv is under its own license ([runtime/libuv](runtime/libuv/LICENSE)).

# Internals

How yel2 is built, laid out and tested. [The language](language.md) is the reference for what it compiles.

## Formatting

`yelc --fmt` (`compiler/fmt.yel`) lays a file's tokens out again (read from its syntax tree: its comments, each string whole, and which `<…>` are a type's), 100 columns wide: each `()`, `[]` and `{}` stays on one line where it fits, else breaks with an item per line and a list's last given a comma. A `()` is laid out afresh each time; how the author broke a `[]` or `{}` over lines stays, filled where a line runs too long (so a bin-packed list stays packed). A long condition breaks before `&&` or `||`, four spaces further in. A block's braces break with the line they are on (an `if` and its `else`s together), a call whose last argument is a record or list breaks only that argument (`out.push({ … })`), and a call of one long string stays on one line. A comment paragraph with a line too wide is filled again, but a label line (`// error: …`, `// usage: …`) is kept as written. Tokens print as their source bytes, so it changes no string; formatting the compiler makes the same C, and formatting twice changes nothing. A file whose brackets do not match is left as it is (and named). `compiler/` and `tests/` are formatted with it.

## How it bootstraps

The seed, `seed/yelc.c`, is the C that a previous yelc made of itself: plain C against `runtime/yel.h`, linked with `runtime/yel.c`.

| Stage | What runs | Makes |
| --- | --- | --- |
| 0 | `cc seed/yelc.c` | `build/yelc0` |
| 1 | `yelc0` compiling `compiler/`, then cc | `build/yelc1` |
| 2 | `yelc1` compiling it, then cc | `build/yelc2` |
| 3 | `yelc2` compiling it | `build/yelc3.c` |

`bootstrap.sh` makes stage 0 (and the runtime, `build/yel.o` and `build/libuv.a`) and hands the rest to `build.yel`, as `build/yelc0 build -p build test`: stages 1 to 3 are its steps, each a command made once for what goes into it (the compiler's sources, the prelude and std, the compiler compiling it), installed in `build/`, then the fixed point, then every test, each its own step and each by `build/yelc2`. `yelc2.c` must equal `yelc3.c`: the compiler, built by itself, reproduces itself. `yelc1.c` may differ, since it came from the seed's code generator. The tests are suites, each a step of its own (`./bootstrap.sh test-language`, `build/yelc2 build -p build test-run`; `--help` lists them): every test of `tests/language` and `tests/std` compiled and run, again under `YEL_GC_STRESS=1`, and interpreted (`tools/runner` runs each, as its header says); each package of `tests/packages` with a `<name>.out` compiled and run; `tests/fmt`; std's and the compiler's own tests; the bitcode backend (with `clang`); the WASI 0.3 components (with `WASI_SDK`). They run together (`-j`, the cores), a failure fails only what depends on it, and a test none of whose inputs changed is not run again where it makes files (a test of `tests/language` or `tests/std`, a format test) or not compiled again (a package's test, though it runs each time).

**A new language feature lands in two steps,** because the seed only compiles what it knew when it was made:
1. Teach the compiler (`compiler/`) the feature without using it in the compiler itself, then run `./bootstrap.sh --update-seed`.
2. Start using the feature in `compiler/`.

A change to the code generator flows through by itself: stage 1 still has the seed's output, stages 2 and 3 have the new one, and `--update-seed` records it.

A change to the runtime that the seed's C cannot compile against (as when the C became typed) needs the old runtime kept for it: copy `runtime/yel.h` (and the other headers, and `runtime/yel.c`) to `seed/runtime/` before changing it, or, when only names changed, make `seed/runtime/yel.h` a shim that includes today's runtime and maps the old names. The seed and stage 1 build against that copy, stages 2 and 3 and the tests against `runtime/`, and `--update-seed` deletes the copy along with the old seed. (Such a seed cannot build `build.yel`'s program for today's runtime: `bootstrap.sh` builds stage 1 itself then, and stage 1 runs `build.yel`.)

The very first seed was made by a throwaway yel0 interpreter, which has since been removed: the seed took its place.

## The interpreter

`yelc run` (`compiler/interp.yel`) runs a program on its checked tree, after the checker and before
any IR, so it shares the front end with every other use of the compiler and nothing after it. The
checker leaves what it needs: each name's binding (`Func.bindings`), each call's func by its key,
each expression's type. A frame is a map from each binding's `Mark` to a cell, so a closure keeps
the cells it was made with and sees later writes to them. Values are yel's own (a list a list, a
string a string), held by the compiler's collector; numbers are shown by its own interpolation, so
they print exactly as a compiled program prints them. Integers wrap at their width, as the
runtime's helpers do; value records and fixed-length lists are copied where they are put; `==`
follows the runtime's (by parts, or by identity for lists, maps and shared records).

std is interpreted as it is written, except its funcs that work on raw memory (a list's items, a
map's slots, a buffer's bytes, a string's length): those are done natively, by the func's key, as
are the prelude's externs. A program that reaches anything else (a WASI import not done here)
stops, naming it.

- **Async** runs on the compiler's own executor: an interpreted `start` is a real task (with its
  own frame, and its starter's `@(context)` globals as they were), a `wait` a real wait, a race a
  stream each racer's value is written to. Sleeps, turns and stream reads and writes are the
  runtime's own. Only an expression that waits or starts goes through the async evaluator; its
  waiting parts are evaluated first, then the rest of it as a sync expression would be.
- **Writers** a block made with `stream-pair` and only ever writes to are closed after the
  statement that last uses them, or on a way out, as `releases.yel` closes them in compiled code.
  It is an approximation of that analysis on the tree, not the analysis itself.
- **Generators** are a resumable machine of frames (a block and its next statement, a loop, a for),
  each `step` running to the next `yield`.
- **Observable** resources go through `yel:std/observe`'s registrar as compiled ones do: a field's
  read tells it, a write tells it first, and a list or map changed in place bumps a version.
- **Processes**: `fork` forks the interpreter itself, so the child goes on interpreting from the
  same point, as a compiled program's child does.

`yelc build test-run` runs every test of `tests/language` and `tests/std` interpreted.

## The linter

`yelc lint [package dir]... [-I <dir>]...` checks each package (`compiler/lint.yel`), then runs the
project's `lint.yel` (the nearest, beside `build.yel`; without one, `std:lint`'s recommended rules)
in the interpreter. The packages given are one program, so a use in one counts for another's
declarations: the compiler is linted with the tools built from its modules,
`yelc lint compiler tools/lsp tools/runner`.

- **The tree** the rules see is the checked declarations as `std:lint`'s nodes, built natively and
  handed to the interpreted program through `interp.run-hosted` (std:lint's `files()`; the
  interpreter knows nothing of the linter). Each node has what the checker found it names: a
  local's binding (one number for its declaration and every use), a func's, a const's or a type's
  key, and the module that is in. The uses the checker notes for the language server are added
  where the tree no longer has them (a const it put its value in place of), each in the
  declaration it is in. The packages' tests are checked too, so a use in a test is a use.
- **Rules** are yel (`runtime/std/lint/lint.yel` says the API): a rule's `create` asks its
  context for the kinds of node it looks at (`c.on("call", …)`, and `c.on-end` for what it finds
  of the whole program) and tells what it finds (`c.report`). A plugin is a list of rules: the
  project's own funcs, or a package `lint.yel` includes. A run walks each file once, each node
  before its children, then calls the rules' ends; each finding is told as
  `path:line:column: severity: message [rule]`, and the run ends with 1 where one is an error.
- **The recommended rules** (`rules.recommended()`, a plugin like any other):
  - unused locals, parameters, funcs, consts, globals, types and includes. A name starting with
    `_` is meant to be unused; a func that only calls itself is unused; `main`, tests, externs and
    the display funcs an interpolation calls are used where they are. A declaration a package
    keeps to itself (`@(private)`) is told, and any of a program's (a package with a main: no
    other uses it);
  - `unreachable-arms` (an arm after one that matches every value), `duplicate-arms` (an arm whose
    pattern, as text, an unguarded arm before it has), `unreachable-code` (a statement after a
    return, a break, a continue or a call that never returns, in the same block);
  - `self-assignment` (`x = x;`, `p.f = p.f;`), `constant-condition` (`if true`, `x == x` but a
    float's `x != x`, a loop whose body breaks first);
  - `short-names(least = 2)`: a local's, a parameter's or a func's name shorter than `least`;
  - `empty-blocks`, off unless `l.severity("empty-blocks", "warn")`: an if's, a for's or a loop's
    body, or an arm, that does nothing.

`tests/packages/lint` is the linter's test: a package and a `lint.yel` with a rule of its own,
what `yelc lint` tells of it in `expected.txt`.

## Layout

- `seed/yelc.c`: the compiler, as the C it compiles itself to (regenerated by `--update-seed`).
- `compiler/`: the compiler, a yel package. Its top level holds what every part shares (`syntax.yel`, `types.yel`, `text.yel`, and `ir.yel`, the IR); each part is a module: `lexer`, `grammar` (the parser: a file's tokens to its syntax tree), `lower` (the tree to the declarations the checker reads), `packages` (which reads a program's packages; `grammar-wit.yel` reads WIT, on the parser's token cursor), `checker`, `gen` (the back end: `gen.yel` the C of types and the helpers made for them, `codegen.yel` the checked funcs to the IR, `render.yel` the IR to C), and `opt` (on the IR: `inline.yel`, `escape.yel`, `roots.yel`). `main.yel` runs them in that order, the one way every use of the compiler goes (`analysis.compile`): a build, `yelc check`, `yelc test`, the language server (`analysis.check`, which then gathers what an editor reads: targets, references, docs) and the compiler's tests (`analysis.lower`) differ only in how far it goes and what is done with what it made; where the context exits (yelc), errors stop it where a phase ends, else (an analysis) they are kept. The syntax tree (`syntax.yel`'s, red-green) is lossless: a file's every byte is in it, its whitespace and comments tokens of their own, each string in pieces (its quotes, its text, each interpolation's braces, expression and format), what an error passes over an error node, so a broken file's tree is whole. Its green nodes are its kind, its children and its width (no place: one may be anywhere); a red node is one seen in place, its parent and its offset, made as a walk reaches it. `grammar` builds it as it parses, a construct wrapped once parsed around what was built since a mark taken before it (a call around its callee, a binary around its left), and tells syntax errors (a missing token just past the last taken, the statement in error passed over); `lower` gives it its meaning: the scope each declaration is in, a literal's value, what yel writes for what it says (`xs[i]` is `xs.at(i)`, `wait for`'s loop, an arm for each alternative that binds), and what is made from what is written (a view's resource and funcs, a regex's matcher), built as nodes, never written as text and parsed. An analysis (the language server) keeps each file's tree (`Context.trees`): a declaration's docs are the comment lines just above its node, and whether a file declares `main` is read from its tree, nothing lowered; a build lowers each as it is read and keeps none. One lexer reads every file (`lexer.lex-file`), the formatter's and WIT's too. A check keeps what the next of the same program may take (`Context.cache`, the language server's, one for each package): each file's tree as parsed from its text (its errors told again), and each func's body as checked, by its text as written where it is. Where every declaration's interface (a func's head, any other's whole text) and the sources read are as the last check found them, an unchanged func's body is not checked again: its types and bindings, its effect sites, its notes and its errors are taken from there, and only edited funcs are checked; anything else (a signature, a type, a file added) is checked whole. `compiler/incremental.test.yel` edits a real file and checks that what an incremental check finds is what a check from nothing finds. `compiler/tree.test.yel` checks the lexer and the tree on every `.yel` file of the repository: each token where it is, and each tree reading back as its file, byte for byte. With `--backend bitcode`, `llvm.yel` lowers the IR into LLVM bitcode instead (`bitcode.yel` writes it; `llvm.yel` also says how each target's C calling convention passes a value to the runtime: arm64, x86-64 System V, wasm32), for a target `--triple` names (by default the one yelc runs on). With `-g` it carries debug info as LLVM's metadata, which LLVM turns into DWARF: a subprogram for each func, each instruction's line and column, and each local's variable and type (numbers, bools, chars, strings, records by name, tuples, options, results; a resource's or a variant's object behind its pointer), so a debugger (lldb, gdb) sets breakpoints by a `.yel` file and line, steps through it, shows backtraces in it, and shows the locals' values (`clang -g` links it; on macOS that makes the `.dSYM`).
- `runtime/prelude.yel`: the runtime's C functions as `extern` funcs, which every program sees.
- `runtime/yel.h`: the C runtime of compiled code: strings, lists and maps (items of any one C type), showing, I/O. The header declares it (with the types, macros and small inline helpers a program's C needs); `runtime/yel.c` defines it, compiled once per target (`build/yel.o`, `build/yel-wasm.o`) and linked with every program, whichever backend made it (C, or LLVM bitcode with `--backend bitcode`).
- `tests/`, laid out as test262 is:
  - `tests/language/<part>/` and `tests/std/<part>/`: a test a file, each one behaviour where it can be (`tests/language/patterns` is split so; the others are still a file each of what was one program), next to the ones of that part that must not compile. Each starts with its header (`tools/runner/main.yel` says all it may hold):

    ```yel
    // ---
    // description: a | b matches what either matches
    // features: [patterns, match]
    // skip: [interp]          (the backends it is not for: native, interp)
    // exit: 1                 (how it must end; 0 where not given)
    // negative: compile       (it must not compile; then each error, help and note yelc must give:)
    // error: 12:5: message    (the message at that place of the test; another file's with its path)
    // ---
    ```

    A test checks what it means to with `std:testing`'s `expect` (`expect.equal(got, want)`), and passes where it ends as its header says. One whose output is what it tests (formatting, WAVE, a panic's message) has a `<name>.out` beside it, which what it writes (standard output and error) must be.
  - `tests/packages/`: programs of more than one file, each with a `<name>.out` (one without is a package the tests include: `tests/packages/shapes`), `testing` (`yelc test`'s runner), `build-system` and `host`.
  - `tests/components/`: the WASI component tests (an api and its caller, the UI component and its host).
  - `tests/bitcode/`: the bitcode backend's (`tools/bitcode-test.sh`); `tests/fmt/`: `yelc --fmt`'s layout, each input and its `.out`.


## CI and releases

`.github/workflows/test.yml` bootstraps and tests on each target's own runner (linux-x64, linux-arm64, darwin-arm64, darwin-x64: libuv and the runtime are built for the system they run on, so nothing is cross-compiled), LLVM's clang and opt installed for the bitcode tests, then `--full` on linux-x64 with the WASI 0.3 component tests (wasi-sdk 34, wasmtime 49, wac). `release.yml` makes a pre-alpha when run by hand from `main` (Actions, Release, Run workflow; or `gh workflow run release.yml`): the next `pre-alpha-<n>` tag and its version (`pre-alpha 3 (1a2b3c4 2026-10-06)`), which `build.yel` embeds in every stage (`-Dversion`, by default `YEL_VERSION`, else `compiler/version.txt`'s `dev`: `#embed("version.txt")`, the same text in stages 2 and 3, so the fixed point holds), each target bootstrapped at -O2 and tested, its archive made and checked by `tools/package.sh` (`yel-<target>.tar.gz`: `bin/yelc` and `bin/yel-lsp`, the runtime and the objects a program links, laid out as yelc finds its home, the directory above its own; the check: `--version` says the release, and a program builds and runs from the archive alone), then a GitHub pre-release with every archive and the commits since the last one.


## Next

These are ordered so that each one makes the compiler itself easier to write:

1. WIT past synchronous funcs: resources a program exports (`resource.new`, `resource.rep`, a destructor); a stream's or a future's end let go when a binding not made by a `let` (a match arm's, a `for`'s) holds it; and natively, an import satisfied by a yel package's visible module, and a C header for a native host of the exports;
2. the collector, past its baseline: faster marking (a radix page table in place of the hash: collections are a third of a compile's time), and collections at turn boundaries (an export's return, an async yield) once components exist, with a younger generation for what dies within one; escape analysis for variants and lists too, and a record that escapes only into another that does not;
   - later, and only on native: a flag that drops the slots and scans the stack conservatively (the page table already finds objects from interior pointers; the collector would pin what the stack points at);
3. yel's UI half: components, state, templates, targeting `yel-host`.

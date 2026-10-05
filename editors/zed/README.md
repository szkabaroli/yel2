# yel2 for Zed

Zed's extension for yel2, in two parts:

- **The grammar** ([`../tree-sitter-yel`](../tree-sitter-yel)): highlighting, the outline (funcs, types,
  their fields, modules, constants, views), bracket matching and indentation. It follows
  `compiler/grammar.yel`, and parses every `.yel` file in the repository (but `tests/errors/`, which
  are meant to fail) with no error.
- **The language server**, `yel-lsp` (`tools/lsp`): the compiler's errors as diagnostics, as
  `yelc check` finds them, on the open documents as they are (unsaved too); formatting as
  `yelc --fmt` does; and go to definition, find references, highlights of a name's uses, and a
  hover with a func's signature, a type's fields or cases, or a value's type (inferred too), for
  funcs, types, modules, constants, globals, locals, parameters, and records' and views' fields
  (a view's names resolve to the view's own lines, not to the yel it is lowered to). Built-in
  types (`s32`, `string`, `list`) tell what they are, their definition std's module of them where
  there is one (`list.yel`, `text.yel`, `integer.yel`'s `s32`); a tuple's and an anonymous
  record's fields tell their type (no declaration: no definition). A variant's (or an enum's)
  cases and a flags' flags resolve as constructors and in patterns, an option's and a result's
  cases (`some`, `none`, `ok`, `err`) too, and so do the fields a record pattern names.
  Completion, as the check finds it on what is written so far (`x.` with no name yet, a `;` not
  yet written): after a dot, a value's fields and the funcs that take it first (what `x.f()`
  calls), a module's members, or a type's cases and funcs; elsewhere the locals seen there, the
  module's and the package's names, the builtins, the modules, and the keywords.
  The outline (the compiler's: each declaration as its syntax tree has it, a module's, a record's
  fields, a variant's cases, a view's fields in it, each with what it is as written); a call's
  signature while its arguments are written (its callee's declaration as written, the parameter
  the cursor is at: x.f(…) of a value gives f's first to x), a `(` or a `,` asking for it; and
  rename, of what this package declares (a local, a parameter, a func, a type, a field, a case):
  its declaration and every use, a keyword's word written `%name`. A rename is checked before it
  is made: the program checked again with it made must have no error it did not have, and the
  new name must name what the old one did and nothing else (an outer name it would take the uses
  of is refused, as is a name declared twice). Hover tells a declaration as written (its
  signature from its syntax tree: type parameters, defaults and all), and its docs, the comment
  lines just above it.
  A change is checked once (requests made meanwhile wait for that check, never start one of
  their own), in two steps: as far as what each name names, which requests are answered from at
  once, then the readonly and move checks on the IR; the diagnostics are told once, with the
  second step's (or with the first's where it found errors: the second is not run on those).

## Install (as a dev extension)

```sh
editors/zed/dev.sh
```

It builds `build/yel-lsp`, generates the grammar's parser, and commits a snapshot of the grammar to a
git repository of its own (`build/tree-sitter-yel`), as Zed builds a grammar from a repository at a
commit; `extension.toml`'s grammar then points at that commit. In Zed, run **zed: install dev
extension** and choose this directory. Run `dev.sh` again after changing the grammar, then **zed:
rebuild dev extension**.

The compiler that builds `yel-lsp` must be one that can include the compiler's own package (`YELC`,
default `build/yelc2`: a bootstrap's).

## The server

Zed starts `yel-lsp` from the settings' path, else `yel-lsp` on the `PATH`, else the worktree's
`build/yel-lsp`. Opened in the yel2 repository, it is given the repository's `runtime/prelude.yel`
(the standard library and WIT beside it); elsewhere, set the prelude in the settings:

```json
{
  "lsp": {
    "yel-lsp": {
      "binary": {
        "path": "/path/to/yel2/build/yel-lsp",
        "arguments": ["/path/to/yel2/runtime/prelude.yel"]
      }
    }
  }
}
```

## Publishing

Once `editors/tree-sitter-yel` is pushed, `extension.toml`'s grammar names the repository and a commit
of it in place of the snapshot:

```toml
[grammars.yel]
repository = "https://github.com/szkabaroli/yel2"
rev = "<commit>"
path = "editors/tree-sitter-yel"
```

The generated parser (`src/parser.c`, `src/grammar.json`, `src/node-types.json`,
`src/tree_sitter/`) must be committed with it: Zed compiles it, it does not generate it.

# apm — the A Package Manager

`apm` is `pip`/`uv` for the A language: install, update, publish and run
third-party packages written in A itself. It is implemented in A (this
directory), with no external dependencies beyond the A toolchain and
(when needed) `git`/`tar`/`unzip`/`curl`.

## Quick start

```sh
# from the repo root (build the toolchain first, see ../README.md)
./apm/apm init myapp                 # scaffold a project (writes apm.json)
./apm/apm install string-utils       # add packages (resolves deps, locks)
./apm/apm list                       # show what is installed
./apm/apm run main.a                 # run a program with packages on -I
```

`apm/apm` is a small launcher: the A runtime exposes no `argv` to
programs, so the launcher carries the real CLI in `APM_ARGS` and runs
`apm.a`. You can also drive it directly:

```sh
APM_ARGS="install hello-lib" ./build/a run apm/apm.a
```

## How it works

- **Projects** are directories containing `apm.json` (created by `apm init`).
  Packages install into `.apm/packages/<import_name>/` and are pinned in
  `apm.lock.json` (commit it for reproducible installs, like `Cargo.lock`).
- **Imports** work like Python: `import hello_lib` (package `hello-lib`
  maps `-` → `_`). `apm run` passes `-I .apm/packages` to `a run`;
  `apm exec` sets `A_PATH` instead.
- **Manifest** (`apm.json`): `name`, `version`, `description`, `main`,
  `dependencies` (package → version spec). Example:

```json
{
  "name": "myapp",
  "version": "0.1.0",
  "main": "main.a",
  "dependencies": {"string-utils": "^0.3.0"}
}
```

- **Version specs**: `1.2.3` exact, `^1.2.3` (same major), `~1.2.3`
  (same minor), `>=1.2.3`, `<2.0.0`, comma lists (`">=1.0.0,<2.0.0"`),
  `*` (any). The resolver picks the highest match and walks transitive
  dependencies; conflicting specs fail with a clear message.
- **Registry**: `apm/registry/registry.json` — an index of package
  versions and their sources. Sources can be a local path (relative to
  the index), a `git+<url>[#<ref>]` clone, or an `http(s)` tarball/zip.
  Point at another index with `--registry <path|url>` or `APM_REGISTRY`.
  Remote indices are downloaded over HTTP(S) each run.
- **Publish**: `apm publish` copies a package into the registry (local
  file registries only) and adds its version to the index, ready for
  others to `apm install`.

## Commands

| Command | What it does |
| --- | --- |
| `apm init [name]` | Create `apm.json` (+ `.gitignore` hint) |
| `apm install [pkg[@ver]...]` | Add packages, or install all manifest deps |
| `apm add` | Alias of `install` |
| `apm remove <pkg...>` | Uninstall (+ prune orphaned deps) |
| `apm uninstall` | Alias of `remove` |
| `apm list`, `apm ls` | Show installed packages (from `apm.lock.json`) |
| `apm info <pkg>` | Registry details + install status |
| `apm search <query>` | Search registry names/descriptions |
| `apm update [pkg...]` | Re-resolve to the latest matching versions |
| `apm run <file.a>` | `a run -I .apm/packages <file.a>` |
| `apm exec <cmd...>` | Run a command with `A_PATH` set |
| `apm publish` | Publish this package to the registry |
| `apm doctor` | Check toolchain, git/tar, registry, project |
| `apm clean`, `--all` | Clear `.apm/cache` (or all of `.apm/`) |
| `apm cache` | Show cache directory and size |
| `apm version`, `apm help` | Version / usage |

Options: `-p/--project <dir>`, `--registry <path|url>`,
`--path <dir>` / `--git <url>[#<ref>]` (single-package installs from a
local directory or git repo), `--dev`, `--all`, `-h/--help`, `-V/--version`.

Environment: `APM_HOME` (apm dir), `APM_REGISTRY` (index path/URL),
`APM_PROJECT` (default project dir), `APM_A_BIN` (`a` binary; the
launcher also finds `../build/a` automatically).

## Writing a package

A package is a directory with an `apm.json` plus your `.a` module(s):

```
my-pkg/
  apm.json
  my_pkg.a        # module file; must match the import name (name with '-' -> '_')
```

`apm.json` for a package is the same shape as a project manifest
(`name`, `version`, `description`, `main`, `dependencies`). Publish it
with `apm publish` (copies into the registry), then anyone can
`apm install my-pkg`.

## Developing apm itself

```sh
./build/a test apm/selftest.a   # semver/manifest/registry/resolver tests
```

Seed registry packages (`hello-lib`, `math-utils`, `string-utils`) live
under `apm/registry/packages/` and are used by the tests and README demo.

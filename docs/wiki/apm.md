# Package manager (`apm`)

`apm` is the A equivalent of `pip`/`uv`: a package manager for third-party
A packages, **written in A itself** (see [`apm/`](../../apm/README.md)).

```sh
./apm/apm init myapp                 # scaffold a project (writes apm.json)
./apm/apm install string-utils       # add packages (resolves deps, locks)
./apm/apm run main.a                 # run a program with packages on -I
```

## Model

- A **project** is a directory with `apm.json`. Installed packages live in
  `.apm/packages/<import_name>/` and are pinned in `apm.lock.json`.
- Imports behave like Python: `import hello_lib` (package `hello-lib`
  maps `-` → `_`). `a run`/`a build`/`a check`/`a test` add
  `<project>/.apm/packages` to the module search paths automatically
  whenever `apm.json` is found walking up from the entry file, so no
  `apm run` wrapper is needed; `apm exec` sets `A_PATH` instead.
- The **registry** (`apm/registry/registry.json`) indexes package versions
  and their sources (local path, `git+<url>[#<ref>]`, or http tarball/zip);
  `APM_REGISTRY` points at another index or a remote JSON URL.
- **Publishing**: `apm publish` copies a package into a local registry.

See [`apm/README.md`](../../apm/README.md) for the full command list, the
manifest/`apm.json` schema, version-spec syntax (`^`, `~`, `>=`, …), and
how to write and publish a package.

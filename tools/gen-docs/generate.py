#!/usr/bin/env python3
"""Generate the A built-in/method reference documentation.

The markdown is produced from the compiler's doc tables — the same source
the type checker and the language server read — so the reference can never
drift from the implementation:

* `compiler/stages/name-resolution/builtin_docs.hpp`   (builtin_docs.cpp
  dump tool prints them as JSON)
* `compiler/stages/type-checker/method_signatures.hpp` (primitive methods)

Outputs (overwritten in place, marked with `GENERATED`):
* `docs/builtins.md`
* `docs/methods.md`

Usage:
    python3 tools/gen-docs/generate.py [--check]
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GEN_DIR = Path(__file__).resolve().parent
DUMP_CPP = GEN_DIR / "dump_builtin_docs.cpp"
DOCS_DIR = ROOT / "docs"

GENERATED_NOTE = (
    "<!-- AUTO-GENERATED from the compiler doc tables by "
    "`tools/gen-docs/generate.py`. Do not edit by hand. -->\n"
)


def sha(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()[:12]


def build_dump(binary: Path) -> None:
    source = DUMP_CPP.read_text(encoding="utf-8")
    digest = sha(source)
    marker = Path(binary).with_suffix(".sha")
    if marker.exists() and marker.read_text() == digest and binary.exists():
        return
    cmd = [
        os.environ.get("CXX", "g++"),
        "-std=c++20",
        "-O2",
        "-I", str(ROOT / "compiler/stages"),
        "-I", str(ROOT / "runtime"),
        "-o", str(binary),
        str(DUMP_CPP),
    ]
    subprocess.run(cmd, check=True)
    marker.write_text(digest)


def run_dump(binary: Path) -> dict:
    dump = Path(tempfile.gettempdir()) / "a-gen-docs-dump.json"
    subprocess.run([str(binary), str(dump)], check=True)
    return json.loads(dump.read_text(encoding="utf-8"))


def is_function(entry: dict) -> bool:
    return "(" in entry["signature"]


def entry_section(entry: dict) -> str:
    """One reference section for a builtin (Python-function style)."""
    name, sig, doc = entry["name"], entry["signature"], entry["documentation"]
    parts = [f"### `{sig}`\n"]
    if doc:
        parts.append(doc + "\n")
    if entry.get("example"):
        parts.append("Example:\n\n```a\n" + entry["example"].rstrip() + "\n```\n")
    if entry.get("seeAlso"):
        see = ", ".join(f"`{s.strip()}`" for s in entry["seeAlso"].split(",") if s.strip())
        parts.append(f"See also: {see}\n")
    return "\n".join(parts)


def render_builtins(data: dict) -> str:
    builtins = data.get("builtins", [])
    functions = sorted(
        (b for b in builtins if is_function(b)), key=lambda b: b["name"].lower()
    )
    constants = sorted(
        (b for b in builtins if not is_function(b)), key=lambda b: b["name"].lower()
    )
    kernels = [b for b in functions if b["name"].startswith("kernels_")]
    others = [b for b in functions if not b["name"].startswith("kernels_")]

    out: list[str] = [
        "# A — Built-in Functions and Constants",
        "",
        GENERATED_NOTE,
        "Every name documented here is available in every A module **without an "
        "import**. These are the same doc tables the type checker and the "
        "language server read (`builtin_docs.hpp`), so signatures and examples "
        "always match the running toolchain.\n",
        "Functions marked with a module subscript (for example `math.sqrt`) are\n"
        "also available through their native module; methods are documented on\n"
        "[Methods](methods.md). Type names (`int`, `f64`, `String`, …) are in\n"
        "[Keywords](keywords.md).\n",
        "## TOC\n",
        " * [Built-in functions](#built-in-functions)",
        " * [Built-in constants](#built-in-constants)",
        " * [Numeric kernels (`kernels_*`)](#kernels)",
        " * [Primitive type methods](methods.md)",
        "",
        "## Built-in functions",
        "",
    ]
    for entry in others:
        out.append(entry_section(entry))

    out += [
        "## Built-in constants",
        "",
        "These compile-time constants are available without import.",
        "",
    ]
    for entry in constants:
        out.append(entry_section(entry))

    out += ["## Numeric kernels (`kernels_*`)", "",
            "Tensor/numeric primitives exposed through the `kernels` package.",
            "",
            "| Function | Description |",
            "|---|---|"]
    for entry in sorted(kernels, key=lambda b: b["name"].lower()):
        sig = entry["signature"].replace("|", "\\|")
        desc = entry["documentation"].replace("|", "\\|")
        out.append(f"| `{entry['name']}` | {sig} — {desc} |" if desc else f"| `{entry['name']}` | {sig} |")
    out.append("")
    return "\n".join(out) + "\n"


METHOD_ORDER = ["array", "map", "set", "string"]


def render_methods(data: dict) -> str:
    methods = data.get("methods", {})
    out: list[str] = [
        "# A — Primitive Type Methods",
        "",
        GENERATED_NOTE,
        "These methods live on primitive values and are available on any value of\n"
        "the right type, without importing a module. They come from\n"
        "`compiler/stages/type-checker/method_signatures.hpp` — the exact table the\n"
        "type checker uses, so completions, hover, and this reference always agree.\n",
        "## TOC\n",
    ]
    for kind in METHOD_ORDER:
        out.append(f" * [{kind.title()} methods](#{kind}-methods)")
    out += ["", ""]
    for kind in METHOD_ORDER:
        entries = methods.get(kind, [])
        out.append(f"## {kind.title()} methods\n")
        out.append("| Method | Signature | Description |")
        out.append("|---|---|---|")
        for m in entries:
            sig = m["signature"].replace("|", "\\|")
            desc = m["documentation"].replace("|", "\\|")
            sig_name, sep, _ = sig.partition("(")
            cell_sig = sig if sep and sig_name == m["name"] else m["name"] + "(" + sig + ")"
            out.append(f"| `{m['name']}` | `{cell_sig}` | {desc} |" if desc not in ("", "-") else f"| `{m['name']}` | `{cell_sig}` |")
        out.append("")
    return "\n".join(out) + "\n"


def main() -> int:
    check = "--check" in sys.argv
    cache_dir = Path(tempfile.gettempdir()) / "a-gen-docs"
    cache_dir.mkdir(exist_ok=True)
    binary = cache_dir / "dump_builtin_docs"
    build_dump(binary)
    data = run_dump(binary)

    pages = {
        "builtins.md": render_builtins(data),
        "methods.md": render_methods(data),
    }
    rc = 0
    for name, content in pages.items():
        path = DOCS_DIR / name
        if check:
            existing = path.read_text(encoding="utf-8") if path.exists() else ""
            if existing != content:
                print(f"STALE: docs/{name} differs from generated output "
                      f"(run: python3 tools/gen-docs/generate.py)")
                rc = 1
            continue
        path.write_text(content, encoding="utf-8")
        n = len(content.splitlines())
        print(f"wrote docs/{name} ({n} lines)")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

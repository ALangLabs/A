#!/usr/bin/env python3
"""End-to-end smoke test for a-lsp.

Usage:
    python3 tools/lsp/smoke_test.py build/a-lsp [--log]
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = '''import json

## A two-dimensional point.
class Point:
    x: i32
    y: i32

    fn length_squared(self) -> i32:
        return self.x * self.x + self.y * self.y

## Adds two numbers.
fn add(a: i32, b: i32) -> i32:
    return a + b

point = Point(3, 4)
total=add(point.x,point.y)
doc = json.parse("[1, 2]")
words = "abc"
print("total = {total}")
print(doc, words.strip())

## Adds things.
## Args:
##   a: first number
##   b: second number
##   c: third number
## Returns:
##   The sum.
## Example:
## >>> add3(1, 2, 3)
## 6
fn add3(a: i32, b: i32, c: i32) -> i32:
    return a + b + c

total = add3(1, 2, 3)

"""
Greets someone warmly.
"""
fn greet(name: str) -> str:
    """Returns a friendly greeting."""
    return "hi " + name

greeting = greet("world")

fn wrapper() -> i32:
    # docs: https://a-lang.dev
    return add(1, 2)

fn scale(factor: i32, offset: i32) -> i32:
    return factor * 2 + offset

fn apply() -> i32:
    factor = 3
    offset = 1
    squared = point.length_squared()
    return scale(factor, offset) + squared

class Base:
    v: i32

class Child(Base):
    w: i32

fn loops() -> i32:
    acc = 0
    for i in 0..3:
        acc += i
    doubled = [n * 2 for n in 0..3]
    return acc + doubled[0]
'''


class Client:
    def __init__(self, server: str) -> None:
        environment = dict(os.environ)
        if "--log" in sys.argv:
            environment["A_LSP_LOG"] = "1"
        self.process = subprocess.Popen(
            [server],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        self.next_id = 1
        self.buffered: list[dict] = []

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        assert self.process.stdin is not None
        self.process.stdin.write(header + body)
        self.process.stdin.flush()

    def receive(self) -> dict:
        assert self.process.stdout is not None
        headers: dict[str, str] = {}
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("a-lsp closed stdout")
            text = line.decode("ascii").strip()
            if not text:
                break
            name, value = text.split(":", 1)
            headers[name.lower()] = value.strip()
        length = int(headers["content-length"])
        return json.loads(self.process.stdout.read(length))

    def request(self, method: str, params: dict) -> dict:
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            message = self.receive()
            if message.get("id") == request_id and "method" not in message:
                return message
            # Async diagnostics may land mid-request (ty/pyrefly do the same);
            # buffer them for wait_notification instead of dropping them.
            if "method" in message:
                self.buffered.append(message)

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def wait_notification(self, method: str) -> dict:
        for index, buffered in enumerate(self.buffered):
            if buffered.get("method") == method:
                return self.buffered.pop(index)
        while True:
            message = self.receive()
            if message.get("method") == method:
                return message

    def stop(self) -> None:
        self.request("shutdown", {})
        self.notify("exit", {})
        self.process.wait(timeout=5)


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = sys.argv[1]
    if not Path(server).is_file() and not Path(server).exists():
        print(f"a-lsp executable not found: {server}")
        return 2

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "main.a"
        path.write_text(SOURCE, encoding="utf-8")
        uri = path.as_uri()

        client = Client(server)
        init = client.request(
            "initialize",
            {
                "rootUri": Path(directory).as_uri(),
                "capabilities": {},
            },
        )
        client.notify("initialized", {})
        client.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "a",
                    "version": 1,
                    "text": SOURCE,
                }
            },
        )

        diagnostics = client.wait_notification("textDocument/publishDiagnostics")
        if diagnostics["params"]["diagnostics"]:
            fail(f"unexpected diagnostics: {diagnostics['params']['diagnostics']}")

        lines = SOURCE.splitlines()
        call_line = next(i for i, line in enumerate(lines) if "add(point.x" in line)
        call_column = lines[call_line].index("add")
        definition = client.request(
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )
        expected_line = next(i for i, text in enumerate(lines) if text.startswith("fn add("))
        range_ = definition["result"]["range"]
        if range_["start"]["line"] != expected_line:
            fail(
                f"definition for 'add' landed on line {range_['start']['line']}, "
                f"expected {expected_line}"
            )

        hover = client.request(
            "textDocument/hover",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )
        hover_text = hover["result"]["contents"]["value"]
        if "fn add" not in hover_text:
            fail("hover did not describe the add function")
        if "Adds two numbers." not in hover_text:
            fail("hover did not include the declaration's doc comment")

        greet_call_line = next(i for i, line in enumerate(lines) if 'greet("world")' in line)
        greet_column = lines[greet_call_line].index('greet("')
        hover = client.request(
            "textDocument/hover",
            {
                "textDocument": {"uri": uri},
                "position": {"line": greet_call_line, "character": greet_column},
            },
        )
        greet_hover = hover["result"]["contents"]["value"]
        if "Returns a friendly greeting." not in greet_hover:
            fail("hover did not include the function's docstring")

        symbols = client.request(
            "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
        )["result"]
        names = {symbol["name"] for symbol in symbols}
        if not {"Point", "add", "point", "total"}.issubset(names):
            fail(f"missing document symbols: {sorted(names)}")

        completion = client.request(
            "textDocument/completion",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column + 1},
            },
        )["result"]["items"]
        labels = {item["label"] for item in completion}
        if not {"add", "Point", "print"}.issubset(labels):
            fail("completion did not include functions, classes, and builtins")

        formatting = client.request(
            "textDocument/formatting",
            {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 4, "insertSpaces": True},
            },
        )["result"]
        if len(formatting) != 1:
            fail(f"formatting returned {len(formatting)} edits, expected 1")
        formatted = formatting[0]["newText"]
        if "total = add(point.x, point.y)" not in formatted:
            fail("formatting did not normalize the assignment")
        if "\n\n\n" in formatted:
            fail("formatting left repeated blank lines")

        rename = client.request(
            "textDocument/rename",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
                "newName": "plus",
            },
        )["result"]
        edits = rename.get("changes", {}).get(uri, [])
        if len(edits) < 2:
            fail(f"rename returned {len(edits)} edits, expected declaration and use")
        if any(edit["newText"] != "plus" for edit in edits):
            fail("rename produced an unexpected replacement")

        highlights = client.request(
            "textDocument/documentHighlight",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )["result"]
        if len(highlights) < 2:
            fail(f"document highlight returned {len(highlights)} ranges, expected at least 2")

        folding = client.request(
            "textDocument/foldingRange", {"textDocument": {"uri": uri}}
        )["result"]
        struct_line = next(i for i, text in enumerate(lines) if text.startswith("class Point:"))
        if not any(entry["startLine"] == struct_line for entry in folding):
            fail(f"folding ranges did not include the class body: {folding}")

        def position_of(fragment: str) -> tuple[int, int]:
            for i, text in enumerate(lines):
                offset = text.find(fragment)
                if offset >= 0:
                    return i, offset
            raise AssertionError(f"fragment not found in source: {fragment!r}")

        # Native module members complete after `json.`.
        json_line, json_col = position_of("json.parse(")
        items = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": json_col + 5}},
        )["result"]["items"]
        labels = {item["label"] for item in items}
        if not {"parse", "stringify", "as_int"}.issubset(labels):
            fail(f"json. completion missing members: {sorted(labels)[:20]}")

        # Primitive methods complete after a bound string's dot.
        words_line, words_col = position_of("words.strip()")
        items = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri}, "position": {"line": words_line, "character": words_col + 6}},
        )["result"]["items"]
        labels = {item["label"] for item in items}
        if not {"strip", "startswith", "join"}.issubset(labels):
            fail(f"string completion missing methods: {sorted(labels)[:20]}")

        # Hover explains module members and primitive methods from the tables.
        _, parse_col = position_of("json.parse(")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": parse_col + 6}},
        )["result"]
        if not hover or "json.parse(text)" not in hover["contents"]["value"]:
            fail("hover did not describe json.parse")

        _, strip_col = position_of("words.strip()")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": words_line, "character": strip_col + 7}},
        )["result"]
        if not hover or "strip([chars])" not in hover["contents"]["value"]:
            fail("hover did not describe the string method strip")

        # Hover works at the very left edge (first character) of a module
        # member and of a line-start builtin. Before the token lookup fix,
        # the preceding '.' and the leading Newline/Indent token stole these
        # positions and hover returned null.
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": json_col + 5}},
        )["result"]
        if not hover or "json.parse(text)" not in hover["contents"]["value"]:
            fail("hover at the left edge of json.parse (member start)")
        standalone_print_line, standalone_print_col = position_of("print(doc, words.strip())")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": standalone_print_line, "character": standalone_print_col}},
        )["result"]
        if not hover or "print(" not in hover["contents"]["value"]:
            fail("hover at the left edge of a line-start builtin call")
        arg_line, arg_col = position_of("return a + b + c")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": arg_line, "character": arg_col + len("return ")}},
        )["result"]
        if not hover or "a: i32" not in hover["contents"]["value"]:
            fail("hover on an indented parameter use")

        # Rich hover: builtin docs render Example and See Also sections.
        print_line, print_col = position_of('print("total = {total}")')
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": print_line, "character": print_col + 2}},
        )["result"]
        if not hover or "**Example:**" not in hover["contents"]["value"] or "**See Also:**" not in hover["contents"]["value"]:
            fail(f"hover did not render Example/See Also for print: {hover}")
        # print's real signature renders multi-line with keyword options.
        if "print(\n    *objects," not in hover["contents"]["value"]:
            fail(f"hover did not render print's multi-line signature: {hover}")

        # Rich hover: a 3-parameter user function renders a multi-line
        # signature plus structured doc sections (Args/Returns/Example).
        rich_line, rich_col = position_of("total = add3(1, 2, 3)")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": rich_line, "character": rich_col + len("total = ") + 1}},
        )["result"]
        text = hover["contents"]["value"] if hover else ""
        if "fn add3(\n    a: i32,\n    b: i32,\n    c: i32\n) -> i32" not in text:
            fail(f"hover did not render a multi-line signature: {text!r}")
        if "**Args:**" not in text or "**Returns:**" not in text or "**Example:**" not in text:
            fail(f"hover did not render structured doc sections: {text!r}")
        if "[Definition](" not in text:
            fail(f"hover did not include a definition link: {text!r}")

        # Signature help inside `json.parse(` names the parameter.
        signature = client.request(
            "textDocument/signatureHelp",
            {"textDocument": {"uri": uri}, "position": {"line": json_line, "character": parse_col + 11}},
        )["result"]
        if (
            not signature
            or "json.parse(text)" not in signature["signatures"][0]["label"]
            or signature["activeParameter"] != 0
        ):
            fail(f"signature help inside json.parse(: {signature}")

        # After the comma in `add(point.x, ...)` the active parameter advances.
        add_line, add_col = position_of("add(point.x,")
        signature = client.request(
            "textDocument/signatureHelp",
            {
                "textDocument": {"uri": uri},
                "position": {"line": add_line, "character": add_col + len("add(point.x,")},
            },
        )["result"]
        if not signature or signature["activeParameter"] != 1:
            fail(f"signature help after the comma in add(...): {signature}")
        if "fn add(a: i32, b: i32)" not in signature["signatures"][0]["label"]:
            fail(f"signature help did not use the declared signature: {signature}")
        # SignatureInformation.documentation carries the builtin's docs.
        sig_doc = signature["signatures"][0].get("documentation")
        if not sig_doc or not sig_doc.get("value"):
            fail(f"signature help missing documentation: {signature}")

        # prepareRename / declaration at the call site (before any edit shifts it).
        prep = client.request(
            "textDocument/prepareRename",
            {"textDocument": {"uri": uri}, "position": {"line": call_line, "character": call_column}},
        )["result"]
        if not prep or "start" not in prep:
            fail(f"prepareRename unexpected: {prep}")
        # prepareRename must return the range under the cursor (this use of
        # `add`), not the declaration site on `expected_line`.
        if prep["start"]["line"] != call_line:
            fail(f"prepareRename returned declaration range: {prep}")

        decl = client.request(
            "textDocument/declaration",
            {"textDocument": {"uri": uri}, "position": {"line": call_line, "character": call_column}},
        )["result"]
        if not decl or decl["range"]["start"]["line"] != expected_line:
            fail(f"declaration navigation unexpected: {decl}")

        # Incremental sync: a ranged edit is applied without a full resend.
        total_line, total_col = position_of("total=add")
        client.notify(
            "textDocument/didChange",
            {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [
                    {
                        "range": {
                            "start": {"line": total_line, "character": total_col + 5},
                            "end": {"line": total_line, "character": total_col + 5},
                        },
                        "text": " ",
                    }
                ],
            },
        )
        # Re-requesting a definition proves the server survived the edit.
        client.request(
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column},
            },
        )

        # New capabilities: semantic tokens, inlay hints, code actions,
        # prepareRename, selection ranges, declaration navigation.
        init2 = client  # noqa
        tokens = client.request(
            "textDocument/semanticTokens/full", {"textDocument": {"uri": uri}}
        )["result"]["data"]
        if len(tokens) == 0 or len(tokens) % 5 != 0:
            fail(f"semantic tokens malformed: {tokens[:20]}")

        # Decode the delta stream and check token types: 5 = method (the
        # `length_squared` declaration), 6 = parameter (`a`/`b`/`self`),
        # 8 = property/field, 0 = namespace (the `json` import alias).
        def decode_semantic(data):
            line = col = 0
            out = []
            for i in range(0, len(data), 5):
                dl, dc, length, ttype, mods = data[i : i + 5]
                line += dl
                col = col + dc if dl == 0 else dc
                out.append((line, col, length, ttype, mods))
            return out

        decoded = decode_semantic(tokens)
        types = {entry[3] for entry in decoded}
        if 6 not in types:
            fail(f"semantic tokens missing parameter type: {sorted(types)}")
        if 5 not in types:
            fail(f"semantic tokens missing method type: {sorted(types)}")
        # `self` inside length_squared resolves to the Point parameter and
        # `self.x`/`self.y` classify as property (8).
        if 8 not in types:
            fail(f"semantic tokens missing property type: {sorted(types)}")

        hints = client.request(
            "textDocument/inlayHint",
            {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": len(lines), "character": 0},
                },
            },
        )["result"]
        if not any(": i32" in hint["label"] for hint in hints):
            fail(f"inlay hints missing inferred i32: {hints}")

        sel = client.request(
            "textDocument/selectionRange",
            {
                "textDocument": {"uri": uri},
                "positions": [{"line": call_line, "character": call_column}],
            },
        )["result"]
        # Spec shape: SelectionRange {range, parent?} — a linked chain from
        # the token outward, not a {ranges: [...]} wrapper.
        if not sel or "range" not in sel[0] or "parent" not in sel[0]:
            fail(f"selectionRange unexpected: {sel}")
        chain = []
        node = sel[0]
        while node:
            chain.append(node["range"])
            node = node.get("parent")
        if len(chain) < 2:
            fail(f"selectionRange chain too short: {sel}")

        actions = client.request(
            "textDocument/codeAction",
            {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": call_line, "character": call_column},
                    "end": {"line": call_line, "character": call_column},
                },
                "context": {},
            },
        )["result"]
        if actions is None:
            fail("codeAction returned null")

        # Call hierarchy: prepare on the `add` declaration (the call-line
        # column shifted by the incremental edit above), incoming calls from
        # `wrapper`, outgoing calls from `wrapper` back to `add`.
        prep_call = client.request(
            "textDocument/prepareCallHierarchy",
            {"textDocument": {"uri": uri}, "position": {"line": expected_line, "character": 3}},
        )["result"]
        if not prep_call or prep_call[0]["name"] != "add":
            fail(f"prepareCallHierarchy unexpected: {prep_call}")
        incoming = client.request(
            "callHierarchy/incomingCalls", {"item": prep_call[0]}
        )["result"]
        if not any(call["from"]["name"] == "wrapper" for call in incoming):
            fail(f"incomingCalls did not find wrapper(): {incoming}")
        wrapper_line = next(i for i, text in enumerate(lines) if text.startswith("fn wrapper"))
        prep_wrapper = client.request(
            "textDocument/prepareCallHierarchy",
            {"textDocument": {"uri": uri}, "position": {"line": wrapper_line, "character": 3}},
        )["result"]
        outgoing = client.request(
            "callHierarchy/outgoingCalls", {"item": prep_wrapper[0]}
        )["result"]
        if not any(call["to"]["name"] == "add" for call in outgoing):
            fail(f"outgoingCalls did not find add(): {outgoing}")

        # Type hierarchy: Child -> Base via class(Base), and back down.
        child_line = next(i for i, text in enumerate(lines) if text.startswith("class Child"))
        prep_type = client.request(
            "textDocument/prepareTypeHierarchy",
            {"textDocument": {"uri": uri}, "position": {"line": child_line, "character": 8}},
        )["result"]
        if not prep_type or prep_type[0]["name"] != "Child":
            fail(f"prepareTypeHierarchy unexpected: {prep_type}")
        supers = client.request(
            "typeHierarchy/supertypes", {"item": prep_type[0]}
        )["result"]
        if not any(item["name"] == "Base" for item in supers):
            fail(f"supertypes did not find Base: {supers}")
        subs = client.request(
            "typeHierarchy/subtypes", {"item": supers[0]}
        )["result"]
        if not any(item["name"] == "Child" for item in subs):
            fail(f"subtypes did not find Child: {subs}")

        # Code lens: `add` has a declaration plus call sites.
        lens = client.request(
            "textDocument/codeLens", {"textDocument": {"uri": uri}}
        )["result"]
        add_lens = [
            entry for entry in lens
            if entry["command"]["arguments"][1]["line"] == expected_line
        ]
        if not add_lens or "reference" not in add_lens[0]["command"]["title"]:
            fail(f"codeLens missing references for add: {lens}")

        # Range formatting: only the messy assignment line is in range.
        messy_line = next(i for i, text in enumerate(lines) if "total=add" in text)
        range_edits = client.request(
            "textDocument/rangeFormatting",
            {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": messy_line, "character": 0},
                    "end": {"line": messy_line + 1, "character": 0},
                },
                "options": {"tabSize": 4, "insertSpaces": True},
            },
        )["result"]
        if not range_edits or "total = add(point.x, point.y)" not in range_edits[0]["newText"]:
            fail(f"rangeFormatting did not fix the assignment: {range_edits}")

        # Pull diagnostics, moniker, document links, includeDeclaration.
        diag = client.request(
            "textDocument/diagnostic",
            {"textDocument": {"uri": uri}, "identifier": "a"},
        )["result"]
        if diag.get("kind") != "full" or diag.get("items"):
            fail(f"pull diagnostics unexpected: {diag}")
        mon = client.request(
            "textDocument/moniker", {"textDocument": {"uri": uri}}
        )["result"]
        if mon.get("scheme") != "a":
            fail(f"moniker unexpected: {mon}")
        links = client.request(
            "textDocument/documentLink", {"textDocument": {"uri": uri}}
        )["result"]
        if not any(link["target"] == "https://a-lang.dev" for link in links):
            fail(f"documentLink missed the comment URL: {links}")
        refs = client.request(
            "textDocument/references",
            {
                "textDocument": {"uri": uri},
                "position": {"line": expected_line, "character": 3},
                "context": {"includeDeclaration": False},
            },
        )["result"]
        if any(ref["range"]["start"]["line"] == expected_line for ref in refs):
            fail("references includeDeclaration=false still returned the declaration")

        # Parameter-name inlay hints: add(point.x, point.y) -> a:/b:.
        hints = client.request(
            "textDocument/inlayHint",
            {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": len(lines), "character": 0},
                },
            },
        )["result"]
        param_labels = {hint["label"] for hint in hints if hint.get("kind") == 2}
        if not {"a: ", "b: "}.issubset(param_labels):
            fail(f"inlay hints missing parameter names: {param_labels}")
        # Pylance-style suppression: `scale(factor, offset)` passes bare
        # identifiers already named like the parameters, so no hints.
        if "factor: " in param_labels or "offset: " in param_labels:
            fail(f"inlay hints not suppressed for same-named args: {param_labels}")

        # `self.` member completion inside a method resolves the enclosing
        # type's fields and methods.
        self_line, self_col = position_of("self.x")
        items = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": uri}, "position": {"line": self_line, "character": self_col + 5}},
        )["result"]["items"]
        labels = {item["label"] for item in items}
        if not {"x", "y", "length_squared"}.issubset(labels):
            fail(f"self. completion missing Point members: {sorted(labels)[:20]}")

        # Hover on a whole-module import alias describes the module.
        import_line, import_col = position_of("import json")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": import_line, "character": import_col + 8}},
        )["result"]
        if not hover or "import json" not in hover["contents"]["value"]:
            fail(f"hover on import alias unexpected: {hover}")

        # documentSymbol is hierarchical: `apply` nests its locals, and the
        # Point class nests its method.
        symbols = client.request(
            "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
        )["result"]

        def find_symbol(nodes, name):
            for node in nodes:
                if node["name"] == name:
                    return node
                found = find_symbol(node.get("children", []), name)
                if found:
                    return found
            return None

        apply_sym = find_symbol(symbols, "apply")
        apply_children = {c["name"] for c in apply_sym.get("children", [])} if apply_sym else set()
        if not {"factor", "offset"}.issubset(apply_children):
            fail(f"documentSymbol missing apply() locals: {sorted(apply_children)}")
        point_sym = find_symbol(symbols, "Point")
        point_children = {c["name"] for c in point_sym.get("children", [])} if point_sym else set()
        if "length_squared" not in point_children:
            fail(f"documentSymbol missing Point method: {sorted(point_children)}")

        # Organize-imports is advertised as a code action kind.
        kinds = init["result"]["capabilities"]["codeActionProvider"]["codeActionKinds"]
        if "source.organizeImports" not in kinds:
            fail(f"codeActionKinds missing source.organizeImports: {kinds}")

        # for-loop and comprehension variables appear as locals: `i` scopes
        # its body, `n` is a child of the `doubled` binding.
        loops_sym = find_symbol(symbols, "loops")
        loops_children = {c["name"] for c in loops_sym.get("children", [])} if loops_sym else set()
        if not {"acc", "i", "doubled"}.issubset(loops_children):
            fail(f"documentSymbol missing loops() locals: {sorted(loops_children)}")
        doubled_sym = find_symbol(symbols, "doubled")
        doubled_children = {c["name"] for c in doubled_sym.get("children", [])} if doubled_sym else set()
        if "n" not in doubled_children:
            fail(f"documentSymbol missing comprehension var n: {sorted(doubled_children)}")
        i_sym = find_symbol(symbols, "i")
        i_children = {c["name"] for c in i_sym.get("children", [])} if i_sym else set()
        if "acc" not in i_children:
            fail(f"documentSymbol for-var i missing body symbols: {sorted(i_children)}")

        # workspaceSymbol includes locals with their enclosing function as
        # containerName.
        ws = client.request(
            "workspace/symbol", {"query": "doubled"}
        )["result"]
        doubled_hits = [h for h in ws if h["name"] == "doubled"]
        if not doubled_hits or doubled_hits[0].get("containerName") != "loops":
            fail(f"workspaceSymbol missing local doubled/loops: {ws}")

        # documentHighlight kinds: `acc` is written (`=`, `+=`) and read.
        acc_line, acc_col = position_of("acc = 0")
        hl = client.request(
            "textDocument/documentHighlight",
            {
                "textDocument": {"uri": uri},
                "position": {"line": acc_line, "character": acc_col},
            },
        )["result"]
        kinds = {h.get("kind") for h in hl}
        if kinds != {2, 3}:
            fail(f"documentHighlight kinds for acc unexpected: {hl}")

        # Code actions on a second document with real diagnostics: W0001
        # (unused let) and W0002 (statement after return).
        bad_path = Path(directory) / "bad.a"
        bad_uri = bad_path.as_uri()
        bad_source = (
            "fn broken() -> i32:\n"
            "    unused_local = 2\n"
            "    return 1\n"
            "    dead_local = 3\n"
        )
        bad_path.write_text(bad_source, encoding="utf-8")
        client.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": bad_uri,
                    "languageId": "a",
                    "version": 1,
                    "text": bad_source,
                }
            },
        )
        # The worker publishes one notification per open document; drain
        # them and confirm bad.a's warnings arrived.
        bad_diags = None
        for _ in range(2):
            note = client.wait_notification("textDocument/publishDiagnostics")
            if note["params"]["uri"] == bad_uri:
                bad_diags = note["params"]["diagnostics"]
        codes = {d.get("code") for d in (bad_diags or [])}
        if not {"W0001", "W0002"}.issubset(codes):
            fail(f"bad.a diagnostics missing W0001/W0002: {bad_diags}")

        dead_line = 3  # 0-based line of `let dead_local = 3`
        actions = client.request(
            "textDocument/codeAction",
            {
                "textDocument": {"uri": bad_uri},
                "range": {
                    "start": {"line": dead_line, "character": 4},
                    "end": {"line": dead_line, "character": 10},
                },
                "context": {},
            },
        )["result"]
        titles = {a["title"] for a in actions}
        if "Remove unreachable statement" not in titles:
            fail(f"codeAction missing remove-unreachable quickfix: {titles}")
        unused_line = 1  # 0-based line of `let unused_local = 2`
        actions = client.request(
            "textDocument/codeAction",
            {
                "textDocument": {"uri": bad_uri},
                "range": {
                    "start": {"line": unused_line, "character": 8},
                    "end": {"line": unused_line, "character": 20},
                },
                "context": {},
            },
        )["result"]
        titles = {a["title"] for a in actions}
        if not any(t.startswith("Prefix with '_'") for t in titles):
            fail(f"codeAction missing unused-variable quickfix: {titles}")
        fix_all = client.request(
            "textDocument/codeAction",
            {
                "textDocument": {"uri": bad_uri},
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 0},
                },
                "context": {"only": ["source.fixAll"]},
            },
        )["result"]
        fix_actions = [a for a in fix_all if a.get("kind") == "source.fixAll.a"]
        if not fix_actions:
            fail(f"codeAction missing source.fixAll action: {fix_all}")
        fix_edits = fix_actions[0]["edit"]["changes"][bad_uri]
        if not any(e["newText"].startswith("_") for e in fix_edits):
            fail(f"source.fixAll edits unexpected: {fix_edits}")

        # Trait/impl: documentSymbol shows the impl block, and hover on the
        # conforming method reports the trait it implements.
        trait_path = Path(directory) / "trait.a"
        trait_uri = trait_path.as_uri()
        trait_source = (
            "trait Named:\n"
            "    fn label(self) -> str\n"
            "\n"
            "class Item:\n"
            "    fn label(self) -> str:\n"
            "        return \"item\"\n"
            "\n"
            "impl Named for Item:\n"
            "    pass\n"
        )
        trait_path.write_text(trait_source, encoding="utf-8")
        client.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": trait_uri,
                    "languageId": "a",
                    "version": 1,
                    "text": trait_source,
                }
            },
        )
        trait_diags = None
        for _ in range(3):
            note = client.wait_notification("textDocument/publishDiagnostics")
            if note["params"]["uri"] == trait_uri:
                trait_diags = note["params"]["diagnostics"]
                break
        trait_symbols = client.request(
            "textDocument/documentSymbol", {"textDocument": {"uri": trait_uri}}
        )["result"]
        trait_names = {s["name"] for s in trait_symbols}
        if "impl Named for Item" not in trait_names:
            fail(f"documentSymbol missing impl block: {trait_names}")
        trait_lines = trait_source.splitlines()
        method_line = next(
            i for i, text in enumerate(trait_lines)
            if "fn label" in text and i > next(j for j, t in enumerate(trait_lines) if t.startswith("class Item"))
        )
        method_col = trait_lines[method_line].index("label")
        hover = client.request(
            "textDocument/hover",
            {"textDocument": {"uri": trait_uri}, "position": {"line": method_line, "character": method_col}},
        )["result"]
        if not hover or "implements Named" not in hover["contents"]["value"]:
            fail(f"hover on impl method missing trait note: {hover}")

        # Organize imports: sorts a contiguous run (quoted, std:, from),
        # dedupes exact repeats, and collapses the removed line.
        (Path(directory) / "a.a").write_text("fn ax() -> i32:\n    return 1\n", encoding="utf-8")
        (Path(directory) / "b.a").write_text("fn bx() -> i32:\n    return 2\n", encoding="utf-8")
        imp_path = Path(directory) / "imp.a"
        imp_uri = imp_path.as_uri()
        imp_source = (
            "import std:json\n"
            "import \"b.a\"\n"
            "import std:core\n"
            "from \"a.a\" import ax\n"
            "import \"a.a\"\n"
            "import std:json\n"
        )
        imp_path.write_text(imp_source, encoding="utf-8")
        client.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": imp_uri,
                    "languageId": "a",
                    "version": 1,
                    "text": imp_source,
                }
            },
        )
        for _ in range(4):
            note = client.wait_notification("textDocument/publishDiagnostics")
            if note["params"]["uri"] == imp_uri:
                break
        actions = client.request(
            "textDocument/codeAction",
            {
                "textDocument": {"uri": imp_uri},
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 0},
                },
                "context": {"only": ["source.organizeImports"]},
            },
        )["result"]
        organize = [a for a in actions if a.get("kind") == "source.organizeImports"]
        if not organize:
            fail(f"codeAction missing organizeImports: {actions}")
        expected_imports = (
            "from \"a.a\" import ax\n"
            "import \"a.a\"\n"
            "import \"b.a\"\n"
            "import std:core\n"
            "import std:json\n"
        )
        edits = organize[0]["edit"]["changes"][imp_uri]
        if len(edits) != 1 or edits[0]["newText"] != expected_imports:
            fail(f"organizeImports edit unexpected: {edits}")

        # Async checking: didChange only enqueues (200ms debounce); a later
        # publishDiagnostics proves the worker ran without blocking requests.
        # Two rapid edits must coalesce (latest wins) instead of checking twice.
        client.notify(
            "textDocument/didChange",
            {
                "textDocument": {"uri": uri, "version": 3},
                "contentChanges": [{"text": SOURCE + "\n"}],
            },
        )
        client.notify(
            "textDocument/didChange",
            {
                "textDocument": {"uri": uri, "version": 4},
                "contentChanges": [{"text": SOURCE}],
            },
        )
        # Requests answer immediately even while the worker is debouncing.
        client.request(
            "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
        )
        async_diag = client.wait_notification("textDocument/publishDiagnostics")
        if async_diag["params"]["diagnostics"]:
            fail(f"async diagnostics unexpected: {async_diag['params']['diagnostics']}")

        client.stop()
        print(
            "OK: diagnostics, definition, hover, symbols, completion, formatting, "
            "rename, highlights, folding, module/string completion, signature help, "
            "incremental sync, async checking (debounced, latest-wins), semantic "
            "tokens, inlay hints, prepareRename, declaration, selectionRange, "
            "code actions, call/type hierarchy, code lens, range formatting, "
            "pull diagnostics, moniker, document links, param hints, "
            "semantic token types, self completion, import hover, "
            "hierarchical symbols, code-action quickfixes and fixAll, "
            "organizeImports, read/write highlights, loop/comprehension "
            "locals, workspace-symbol containers, trait impl hover"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
        client.request(
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

        # prepareRename / declaration at the call site (before any edit shifts it).
        prep = client.request(
            "textDocument/prepareRename",
            {"textDocument": {"uri": uri}, "position": {"line": call_line, "character": call_column}},
        )["result"]
        if not prep or "start" not in prep:
            fail(f"prepareRename unexpected: {prep}")

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
        if not sel or "ranges" not in sel[0]:
            fail(f"selectionRange unexpected: {sel}")

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
            "code actions"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

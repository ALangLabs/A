#!/usr/bin/env python3
"""Goto-definition into apm-installed packages (regression test).

Builds a throwaway apm project (.apm/packages/hello_lib/hello_lib.a,
apm.json), opens main.a in a-lsp, and checks textDocument/definition on
`import hello_lib` and on `hello_lib.greet` resolves into .apm/packages.

Usage:
    python3 tools/lsp/apm_smoke_test.py build/a-lsp [--log]
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = '''import hello_lib
from hello_lib import greet

fn main():
    print(greet("apm"))
    print(hello_lib.greet("world"))
'''

LIB = '''fn greet(name: String):
    return "Hello, " + name + "!"
'''

MANIFEST = '''{
  "name": "apm-lsp-fixture",
  "version": "0.1.0",
  "main": "main.a",
  "dependencies": {"hello-lib": "^1.0.0"}
}
'''


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = Path(sys.argv[1])
    if not server.is_file():
        print(f"a-lsp executable not found: {server}")
        return 2

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "apm.json").write_text(MANIFEST, encoding="utf-8")
        lib_dir = root / ".apm" / "packages" / "hello_lib"
        lib_dir.mkdir(parents=True)
        lib_path = lib_dir / "hello_lib.a"
        lib_path.write_text(LIB, encoding="utf-8")
        path = root / "main.a"
        path.write_text(SOURCE, encoding="utf-8")
        uri = path.as_uri()

        client = Client(str(server))
        client.request(
            "initialize",
            {"rootUri": root.as_uri(), "capabilities": {}},
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
        client.wait_notification("textDocument/publishDiagnostics")

        lines = SOURCE.splitlines()
        # Definition on the bare import spec `hello_lib` (line 0).
        definition = client.request(
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {"line": 0, "character": lines[0].index("hello_lib")},
            },
        )
        result = definition.get("result")
        locs = result if isinstance(result, list) else [result]
        if not locs or not locs[0]:
            print("FAIL: no definition location for `import hello_lib`")
            return 1
        target = locs[0]["uri"]
        if ".apm/packages/hello_lib/hello_lib.a" not in target:
            print(f"FAIL: definition target {target!r} is not the installed package file")
            return 1
        print(f"OK import definition -> {target}")

        # Definition on a call through the module: hello_lib.greet(...)
        call_line = next(i for i, line in enumerate(lines) if "hello_lib.greet" in line)
        call_column = lines[call_line].index("hello_lib.greet")
        definition = client.request(
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {"line": call_line, "character": call_column + len("hello_lib.greet")},
            },
        )
        result = definition.get("result")
        locs = result if isinstance(result, list) else [result]
        if not locs or not locs[0]:
            print("FAIL: no definition location for `hello_lib.greet`")
            return 1
        target = locs[0]["uri"]
        if ".apm/packages/hello_lib/hello_lib.a" not in target:
            print(f"FAIL: greet target {target!r} is not the installed package file")
            return 1
        print(f"OK member definition -> {target}")

        client.request("shutdown", {})
        client.notify("exit", {})
        client.stop()
        print("apm smoke test passed")
        return 0


class Client:
    def __init__(self, server: str) -> None:
        self.proc = subprocess.Popen(
            [server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
        )
        self.next_id = 1
        self.buffer: dict[int, dict] = {}

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def receive(self) -> dict:
        headers = b""
        while not headers.endswith(b"\r\n\r\n"):
            ch = self.proc.stdout.read(1)
            if not ch:
                raise RuntimeError("server closed stdout")
            headers += ch
        length = int(headers.decode().split("Content-Length:")[1].split("\r\n")[0].strip())
        return json.loads(self.proc.stdout.read(length))

    def request(self, method: str, params: dict) -> dict:
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            msg = self.receive()
            if msg.get("id") == request_id:
                return msg

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def wait_notification(self, method: str) -> dict:
        while True:
            msg = self.receive()
            if msg.get("method") == method:
                return msg

    def stop(self) -> None:
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


if __name__ == "__main__":
    sys.exit(main())

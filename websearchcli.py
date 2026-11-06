#!/usr/bin/env python3
"""Agent web-search CLI via the Exa MCP endpoint (no API key needed).

Standalone CLI adaptation of the vtx ``WebTool`` / ``WebSearchTool``:
POSTs a JSON-RPC ``tools/call`` request for ``web_search_exa`` to
``https://mcp.exa.ai/mcp`` and prints ranked titles, URLs, and snippets.

Stdlib only — no ``vtx``, ``pydantic``, or ``httpx`` required, so agents
can run it from any checkout:

    python3 websearchcli.py "A programming language ownership model"
    python3 websearchcli.py "latest C++20 features" -n 5 --type keyword
    python3 websearchcli.py "news about X" --livecrawl always --json

Exit codes: 0 on success, 1 on search/runtime failure, 2 on bad arguments.
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request

_MCP_URL = "https://mcp.exa.ai/mcp"
_DEFAULT_TIMEOUT = 25.0
_USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36"
)

_SEARCH_TYPES = ("auto", "neural", "keyword")
_LIVECRAWLS = ("fallback", "always", "never")


def _extract_text(data: dict) -> str | None:
    """Pull text out of a JSON-RPC tool result, joining all text parts."""
    content = data.get("result", {}).get("content", [])
    if isinstance(content, list) and content:
        parts = [c.get("text") for c in content if isinstance(c, dict)]
        parts = [p for p in parts if p]
        return "\n".join(parts) if parts else None
    return None


def _parse_response(body: str) -> str | None:
    """Parse an SSE-streamed or plain-JSON MCP response body."""
    # SSE-streamed response: find the first data line with content.
    for line in body.splitlines():
        if not line.startswith("data: "):
            continue
        try:
            data = json.loads(line[len("data: "):])
        except json.JSONDecodeError:
            continue
        text = _extract_text(data)
        if text:
            return text

    # Fallback: parse the whole body as a single JSON object.
    try:
        text = _extract_text(json.loads(body))
        if text:
            return text
    except (json.JSONDecodeError, TypeError, ValueError):
        pass

    return None


def search(
    query: str,
    num_results: int = 8,
    search_type: str = "auto",
    livecrawl: str = "fallback",
    timeout: float = _DEFAULT_TIMEOUT,
) -> str:
    """Run one Exa web search and return the raw result text.

    Raises RuntimeError with a human-readable message on failure.
    """
    if not query.strip():
        raise ValueError("query must not be empty")
    if not 1 <= num_results <= 20:
        raise ValueError("num_results must be between 1 and 20")
    if search_type not in _SEARCH_TYPES:
        raise ValueError(f"search_type must be one of {_SEARCH_TYPES}")
    if livecrawl not in _LIVECRAWLS:
        raise ValueError(f"livecrawl must be one of {_LIVECRAWLS}")

    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {
            "name": "web_search_exa",
            "arguments": {
                "query": query,
                "type": search_type,
                "numResults": num_results,
                "livecrawl": livecrawl,
            },
        },
    }
    req = urllib.request.Request(
        _MCP_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            "User-Agent": _USER_AGENT,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        # HTTPError subclasses URLError, so it must be caught first.
        raise RuntimeError(f"Exa API error {e.code}") from e
    except urllib.error.URLError as e:
        reason = getattr(e, "reason", e)
        if isinstance(reason, TimeoutError) or "timed out" in str(reason).lower():
            raise RuntimeError(f"Exa timed out after {timeout}s") from e
        raise RuntimeError(f"Exa request failed: {reason}") from e

    text = _parse_response(body)
    if not text:
        raise RuntimeError("No results returned from Exa API")
    return text


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="websearchcli.py",
        description=(
            "Search the web using neural semantic search (Exa MCP, no API key). "
            "Returns ranked page titles, URLs, and relevant text snippets. "
            "Requires internet access."
        ),
    )
    parser.add_argument("query", help="Search query string or question")
    parser.add_argument(
        "-n", "--num-results",
        type=int, default=8, choices=range(1, 21), metavar="1-20",
        help="Number of results (1-20, default: 8)",
    )
    parser.add_argument(
        "--type", "--search-type", dest="search_type",
        default="auto", choices=_SEARCH_TYPES,
        help="'auto' (hybrid), 'neural' (concept matching), or 'keyword' (exact text)",
    )
    parser.add_argument(
        "--livecrawl",
        default="fallback", choices=_LIVECRAWLS,
        help="'fallback' (cached/live), 'always' (fresh crawl), or 'never' (cached only)",
    )
    parser.add_argument(
        "--timeout",
        type=float, default=_DEFAULT_TIMEOUT,
        help=f"Request timeout in seconds (default: {_DEFAULT_TIMEOUT:g})",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit results as a JSON object {query, result} instead of plain text",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        text = search(
            args.query,
            num_results=args.num_results,
            search_type=args.search_type,
            livecrawl=args.livecrawl,
            timeout=args.timeout,
        )
    except (ValueError, RuntimeError) as e:
        print(f"websearchcli: {e}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps({"query": args.query, "result": text}, indent=2))
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# `std.net`

Standard library package `std.net` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** TCP/UDP networking and optional TLS.
- **Source path:** `stdlib/std/net/`

## Import

```a
import net
```

## What It Provides

- **Functions:** `resolve`, `tcp_listen`, `tcp_connect`, `accept`, `send`, `recv`, `close`, `local_port`, `set_timeout`, `set_nonblocking`, `poll`, `udp_socket`, `udp_bind`, `udp_send`, `udp_recv`, `tls_connect`, `tls_available`

## Example

```a
import net

# Inspect the available functions/constants in this build.
# Then call `net.<member>(...)` using the API overview above.
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (Astra backend paths) and host environment.
- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`

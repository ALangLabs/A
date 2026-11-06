# types

Port of CPython 3.14 `Lib/types.py` (345 lines) to A.

## Coverage vs Python

| Python | Status |
| ------ | ------ |
| `type_tag` / `type_name` | `type_tag(x) -> String` returns the Python-style `type(x).__name__` (`"NoneType"`, `"bool"`, `"int"`, `"float"`, `"str"`, `"list"`, `"set"`, `"tuple"`, `"dict"`, struct name, `"unknown"`). `type_name` is an alias. |
| Type-tag constants (`NoneType`, `FunctionType`, `LambdaType`, `BuiltinFunctionType`, `MethodType`, `ModuleType`, `CodeType`, `TracebackType`, `FrameType`, `GeneratorType`, `CoroutineType`, `AsyncGeneratorType`, `CellType`, `MappingProxyType`, `GenericAlias`, `UnionType`, `EllipsisType`, `NotImplementedType`) | Exposed as **string constants** — A has no runtime type objects, so they are vocabulary for `type_tag(x) == types.X` comparisons, not usable with `isinstance` (which does not exist). |
| `SimpleNamespace` | `SimpleNamespace(data)` / `simple_namespace()` struct with `.get/.get_or/.set/.has/.remove/.keys/.items/.len/.repr/.str`. Backed by a dynamic map (`ns.attr` syntax is impossible — no dynamic attribute access). |
| Everything else (`FunctionType` checks, `MappingProxyType`, `GenericAlias`, `UnionType`, `prepare_class`, `resolve_bases`, `new_class`, `get_original_bases`, `CellType`, `coroutine` helpers, `DynamicClassAttribute`, `MemberDescriptorType`, `GetSetDescriptorType`, `WrapperDescriptorType`, etc.) | **Not ported** — they require runtime type objects, `isinstance`/`issubclass`, the class-creation protocol, or C-level introspection A does not have. |

## Semantic deviations

- **No runtime type objects.** `type(x)` becomes `type_tag(x) -> String`.
  The `types.*Type` names are string constants, not classes.
- **No `isinstance`/`issubclass`/`getattr`/`hasattr`/`callable`.** Use
  `type_tag(x)` comparisons and direct field/method access.
- **`SimpleNamespace` attribute access** is via `.get(name)`/`.set(name, v)`/
  `.has(name)` instead of `ns.attr`; construction takes an optional map or
  starts empty (no `**kwargs`). `repr` renders `namespace(k=v, ...)` using
  `pprint.saferepr` for values.
- **Structs** report their struct name from `type_tag` (parsed from
  `str(x)`), matching `cls.__name__` for user classes.
- **Opaque values** (`fn`, ranges, `Ok`/`Err` results) report `"unknown"`;
  `Some(x)` reports the tag of `x` because `json.kind` unwraps optionals.
- Exceptions (`TypeError` etc.) have no A equivalent; contract violations
  raise runtime errors via `core.panic` where applicable.

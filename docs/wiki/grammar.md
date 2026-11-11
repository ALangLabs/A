# Grammar and syntax reference

This page is a concise, practical grammar-oriented summary of A's surface
syntax. It complements the detailed reference pages; when it disagrees with
`a run`/`a check` or the implementation (`compiler/astra/frontend/`), the
compiler is authoritative.

## Lexical elements

```ebnf
identifier   = ( letter | "_" ) , { letter | digit | "_" } ;
comment      = "#" , { any-char-except-newline } ;
newline      = "\n" ;                  (* significant outside brackets *)
indent/dedent = indentation changes after ":" introduce block suites
```

Keywords include `fn`, `if`, `elif`, `else`, `while`, `for`, `in`, `loop`,
`do`, `break`, `continue`, `return`, `defer`, `class`, `metaclass`, `enum`,
`trait`, `impl`, `type`, `where`, `comptime`, `import`, `from`, `as`,
`match`, `pass`, `self`, `and`, `or`, `not`. `None` and constructors like
`Some`/`Ok`/`Err` are capitalized builtins.

## Literals

```ebnf
int_lit      = decimal | "0x" hex | "0b" bin | "0o" oct ;
float_lit    = digits "." digits [ exponent ] | digits exponent ;
bool_lit     = "true" | "false" ;      (* lowercase only *)
char_lit     = "'" ascii-char "'" ;
string_lit   = '"' { char | escape | interpolation } '"' ;
raw_string   = '"""' { any-char } '"""' ;
none_lit     = "None" ;
```

Boolean literals are **`true`** and **`false`** (lowercase). `True`/`False`
are not keywords; used as names they are ordinary identifiers and must be
defined (otherwise name resolution reports `Undefined name`).

## Types (informal)

```ebnf
type         = simple_type | array_type | tuple_type | fn_type | optional
             | generic_type ;
simple_type  = identifier ;            (* bool, int, float, String, user types, ... *)
array_type   = "[" type "]" | type "[" int_lit { "," int_lit } "]" ;
optional     = type "?" ;
fn_type      = "fn" "(" [ types ] ")" [ "->" type ] ;
```

## Expressions (selected)

```ebnf
expr         = assignment-expr | logic-expr | ... ;
logic-expr   = comparison { ("and" | "or") comparison } | "not" logic-expr ;
comparison   = additive { ("=="|"!="|"<"|"<="|">"|">=") additive } ;
call         = primary "(" [ args ] ")" ;
index        = primary "[" expr | slice "]" ;
match_expr   = "match" expr ":" NEWLINE INDENT { arm } DEDENT ;
arm          = pattern [ "if" expr ] "=>" expr ;
```

String interpolation uses `{expr}` inside `"..."` strings.

## Statements and control flow

```ebnf
suite        = ":" NEWLINE INDENT { statement } DEDENT ;

if_stmt      = "if" expr suite { "elif" expr suite } [ "else" suite ] ;
while_stmt   = "while" expr suite ;
do_stmt      = "do" suite "while" expr ;
for_stmt     = "for" pattern "in" expr suite ;
loop_stmt    = "loop" suite ;
return_stmt  = "return" [ expr ] ;
defer_stmt   = "defer" expr ;
```

`if`/`elif`/`while`/`do`-`while` conditions are expressions. Nested control
flow is supported by indenting a suite inside another suite:

```python
if x > 5:
    if x > 8:
        print("nested")
    else:
        print("branch")
```

## Declarations (selected)

```ebnf
fn_decl      = "fn" identifier [ generics ] "(" [ params ] ")" [ "->" type ] suite ;
class_decl   = "class" identifier [ generics ] ":" NEWLINE INDENT { member } DEDENT ;
enum_decl    = "enum" identifier [ generics ] ":" ... ;
entry_decl   = "entry" identifier "(" ")" suite ;
import_stmt  = "import" module_path | "from" module_path "import" names ;
```

For full details, see [syntax and types](syntax-and-types.md),
[statements and expressions](statements-and-expressions.md),
[declarations](declarations.md), and [modules](modules.md).

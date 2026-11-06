# A — Primitive Type Methods

<!-- AUTO-GENERATED from the compiler doc tables by `tools/gen-docs/generate.py`. Do not edit by hand. -->

These methods live on primitive values and are available on any value of
the right type, without importing a module. They come from
`compiler/stages/type-checker/method_signatures.hpp` — the exact table the
type checker uses, so completions, hover, and this reference always agree.

## TOC

 * [Array methods](#array-methods)
 * [Map methods](#map-methods)
 * [Set methods](#set-methods)
 * [String methods](#string-methods)


## Array methods

| Method | Signature | Description |
|---|---|---|
| `append` | `append(v)` | Adds a single element to the end of the array. |
| `extend` | `extend(xs)` | Appends every element of another array. |
| `insert` | `insert(i, v)` | Inserts v before position i. |
| `remove_at` | `remove_at(i)` | Removes and returns the element at index i. |
| `pop` | `pop([i])` | Removes and returns the last element, or the one at index i. |
| `remove` | `remove(v)` | Removes the first occurrence of v. |
| `clear` | `clear()` | Removes every element. |
| `copy` | `copy()` | Returns a shallow copy of the array. |
| `count` | `count(v)` | Number of occurrences of v. |
| `index` | `index(v[, start[, end]])` | First index of v in the range; error when absent. |
| `contains` | `contains(v)` | True when the array holds v. |
| `sort` | `sort()` | Sorts the array in place. |
| `reverse` | `reverse()` | Reverses the array in place. |
| `first` | `first()` | The first element as an optional. |
| `last` | `last()` | The last element as an optional. |
| `join` | `join(sep)` | Concatenates the elements with a separator. |

## Map methods

| Method | Signature | Description |
|---|---|---|
| `get` | `get(k[, default])` | Value for key k; the default or None when absent. |
| `pop` | `pop(k[, default])` | Removes the key and returns its value or the default. |
| `contains` | `contains(k)` | True when the map holds the key. |
| `keys` | `keys()` | The keys as an array. |
| `values` | `values()` | The values as an array. |
| `items` | `items()` | Key/value pairs as an array of tuples. |
| `update` | `update(other)` | Copies every entry of another map into this one. |
| `copy` | `copy()` | Returns a shallow copy of the map. |
| `clear` | `clear()` | Removes every entry. |

## Set methods

| Method | Signature | Description |
|---|---|---|
| `add` | `add(v)` | Inserts a value; duplicates are ignored. |
| `remove` | `remove(v)` | Removes a value; error when it is absent. |
| `discard` | `discard(v)` | Removes a value if present. |
| `contains` | `contains(v)` | True when the set holds the value. |
| `union` | `union(s)` | Elements in either this set or another. |
| `intersection` | `intersection(s)` | Elements in both sets. |
| `difference` | `difference(s)` | Elements in this set but not the other. |
| `issubset` | `issubset(s)` | True when every element is in the other set. |
| `issuperset` | `issuperset(s)` | True when the other set is contained here. |
| `copy` | `copy()` | Returns a shallow copy of the set. |
| `clear` | `clear()` | Removes every element. |

## String methods

| Method | Signature | Description |
|---|---|---|
| `capitalize` | `capitalize()` | Upper-cases the first character. |
| `casefold` | `casefold()` | Aggressive lower-casing for comparisons. |
| `lower` | `lower()` | Lower-cases every character. |
| `upper` | `upper()` | Upper-cases every character. |
| `swapcase` | `swapcase()` | Switches the case of every character. |
| `title` | `title()` | Capitalizes each word. |
| `center` | `center(width[, fill])` | Pads both sides to the width. |
| `ljust` | `ljust(width[, fill])` | Pads on the right to the width. |
| `rjust` | `rjust(width[, fill])` | Pads on the left to the width. |
| `zfill` | `zfill(width)` | Left-pads with zeros to the width. |
| `count` | `count(sub[, start[, end]])` | Number of occurrences of a substring. |
| `find` | `find(sub[, start[, end]])` | First index of a substring, or -1. |
| `rfind` | `rfind(sub[, start[, end]])` | Last index of a substring, or -1. |
| `index` | `index(sub[, start[, end]])` | First index of a substring; error when absent. |
| `rindex` | `rindex(sub[, start[, end]])` | Last index of a substring; error when absent. |
| `startswith` | `startswith(prefix[, start[, end]])` | True when the slice begins with the prefix. |
| `endswith` | `endswith(suffix[, start[, end]])` | True when the slice ends with the suffix. |
| `expandtabs` | `expandtabs([tabsize])` | Replaces tabs with spaces. |
| `strip` | `strip([chars])` | Removes leading and trailing characters. |
| `lstrip` | `lstrip([chars])` | Removes leading characters. |
| `rstrip` | `rstrip([chars])` | Removes trailing characters. |
| `partition` | `partition(sep)` | Splits once at the separator into a three-tuple. |
| `rpartition` | `rpartition(sep)` | Splits once at the last separator into a three-tuple. |
| `removeprefix` | `removeprefix(affix)` | Drops the prefix when present. |
| `removesuffix` | `removesuffix(affix)` | Drops the suffix when present. |
| `replace` | `replace(old, new[, count])` | Replaces occurrences of a substring. |
| `split` | `split([sep[, maxsplit]])` | Splits into an array of pieces. |
| `rsplit` | `rsplit([sep[, maxsplit]])` | Splits from the right into an array. |
| `splitlines` | `splitlines([keepends])` | Splits at line boundaries. |
| `join` | `join(parts)` | Concatenates an array of strings with this string between. |
| `isalnum` | `isalnum()` | True when every character is a letter or digit. |
| `isalpha` | `isalpha()` | True when every character is a letter. |
| `isascii` | `isascii()` | True when every character is ASCII. |
| `isdecimal` | `isdecimal()` | True when every character is a decimal digit. |
| `isdigit` | `isdigit()` | True when every character is a digit. |
| `isidentifier` | `isidentifier()` | True when the text is a valid identifier. |
| `islower` | `islower()` | True when cased characters are lower-case. |
| `isnumeric` | `isnumeric()` | True when every character is numeric. |
| `isprintable` | `isprintable()` | True when every character is printable. |
| `isspace` | `isspace()` | True when every character is whitespace. |
| `istitle` | `istitle()` | True when each word is title-cased. |
| `isupper` | `isupper()` | True when cased characters are upper-case. |
| `contains` | `contains(sub)` | True when the substring occurs. |
| `repeat` | `repeat(n)` | Repeats the string n times. |
| `chars` | `chars()` | The characters as an array. |


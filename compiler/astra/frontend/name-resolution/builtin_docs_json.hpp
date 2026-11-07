#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addJsonDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // json (mirrors Python's json.dumps/loads; doctests below)
      {"json_parse", {"json_parse", "json.parse(text) -> Json",
                       "Parses one JSON document; malformed input is a runtime error.",
                       ">>> import std:json\n>>> doc = json.parse('[1, 2, 3]')\n>>> doc\n[1, 2, 3]",
                       "json.stringify"}},
      {"json_stringify", {"json_stringify", "json.stringify(value) -> String",
                           "Compact JSON text for any serializable A value.",
                           ">>> import std:json\n>>> json.stringify([1, 2, 3])\n'[1, 2, 3]'",
                           "json.parse"}},
      {"json_pretty", {"json_pretty", "json.pretty(value) -> String",
                        "Indented JSON text with two-space indent.",
                        ">>> json.pretty(doc)\n'[\n  1,\n  2\n]'",
                        "json.stringify"}},
      {"json_is_valid", {"json_is_valid", "json.is_valid(text) -> bool",
                          "Whether the text parses, without raising errors.",
                          ">>> json.is_valid('[1, 2]')\ntrue\n>>> json.is_valid('nope')\nfalse",
                          "json.parse"}},
      {"json_get", {"json_get", "json.get(doc, key) -> Json?",
                     "Value for an object key, or None.",
                     ">>> doc = json.parse('{\"a\": 1}')\n>>> json.get(doc, 'a')\n1\n>>> json.get(doc, 'b')\nNone",
                     "json.has"}},
      {"json_at", {"json_at", "json.at(doc, index) -> Json?",
                    "Value at an array index, or None.",
                    ">>> doc = json.parse('[1, 2, 3]')\n>>> json.at(doc, 1)\n2",
                    "json.len"}},
      {"json_len", {"json_len", "json.len(doc) -> i64",
                     "Members of an array or object, characters of a string."}},
      {"json_has", {"json_has", "json.has(doc, key) -> bool", "Key presence in an object."}},
      {"json_keys", {"json_keys", "json.keys(doc) -> [String]", "Object keys in document order."}},
      {"json_type", {"json_type", "json.kind(doc) -> String",
                      "One of null, bool, int, float, string, array, object."}},
      {"json_as_str", {"json_as_str", "json.as_str(doc) -> String?",
                        "The JSON string, or None when the kind differs."}},
      {"json_as_int", {"json_as_int", "json.as_int(doc) -> i64?",
                        "An integral JSON number as i64, or None."}},
      {"json_as_float", {"json_as_float", "json.as_float(doc) -> f64?",
                          "Any JSON number as f64, or None."}},
      {"json_as_bool", {"json_as_bool", "json.as_bool(doc) -> bool?", "A JSON bool, or None."}},
      {"json_as_array", {"json_as_array", "json.as_array(doc) -> [Json]?", "A JSON array, or None."}},
      {"json_as_map", {"json_as_map", "json.as_map(doc) -> {String: Json}?",
                        "A JSON object as a map, or None."}},

      // binary
      {"binary_encode", {"binary_encode", "binary.encode(value) -> [i64]",
                          "Self-describing bytes, one entry per byte (0-255)."}},
      {"binary_decode", {"binary_decode", "binary.decode(bytes) -> Json?",
                          "Decodes bytes; malformed or truncated input yields None."}},
  });
}

}  // namespace a

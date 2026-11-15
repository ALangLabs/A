#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addTraceDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // trace
      {"trace_start", {"trace_start", "trace.start(name) -> i64",
                        "Begins a span nested under the active one; returns its id."}},
      {"trace_end", {"trace_end", "trace.end(span)", "Finishes a span and pops it from the stack."}},
      {"trace_current", {"trace_current", "trace.current() -> i64",
                          "The active span id, or 0 when nothing is active."}},
      {"trace_set_attribute", {"trace_set_attribute", "trace.set_attribute(span, key, value)",
                                "Attaches an attribute; the same key overwrites."}},
      {"trace_add_event", {"trace_add_event", "trace.add_event(span, name)",
                            "Timestamped event on a span."}},
      {"trace_inject", {"trace_inject", "trace.inject() -> String",
                         "traceparent header for the active trace, or \"\"."}},
      {"trace_extract", {"trace_extract", "trace.extract(traceparent) -> i64",
                          "Continues a trace from a W3C traceparent header."}},
      {"trace_spans", {"trace_spans", "trace.spans() -> [Json]",
                        "One record per span in creation order."}},
      {"trace_reset", {"trace_reset", "trace.reset()", "Forgets every span."}},
  });
}

}  // namespace a

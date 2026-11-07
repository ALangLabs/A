#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addMetricsDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // metrics
      {"metrics_counter", {"metrics_counter", "metrics.counter(name, increment[, labels])",
                            "Adds a non-negative integer to a counter."}},
      {"metrics_gauge", {"metrics_gauge", "metrics.gauge(name, value[, labels])",
                          "Sets a gauge to the value."}},
      {"metrics_histogram", {"metrics_histogram", "metrics.histogram(name, value[, labels])",
                              "Records one observation in a histogram."}},
      {"metrics_render", {"metrics_render", "metrics.render() -> String",
                           "Prometheus text exposition format."}},
      {"metrics_reset", {"metrics_reset", "metrics.reset()", "Forgets every series."}},
  });
}

}  // namespace a

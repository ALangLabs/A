#pragma once

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"

namespace a {

inline void addLogDocs(StringMap<BuiltinDoc>& docs) {
  docs.insert({
      // log
      {"log_set_level", {"log_set_level", "log.set_level(level)",
                          "One of debug, info, warn, or error."}},
      {"log_set_json", {"log_set_json", "log.set_json(bool)",
                         "True writes one JSON object per line."}},
      {"log_debug", {"log_debug", "log.debug(message[, fields])",
                      "Debug-level record to stderr."}},
      {"log_info", {"log_info", "log.info(message[, fields])", "Info-level record to stderr."}},
      {"log_warn", {"log_warn", "log.warn(message[, fields])", "Warning record to stderr."}},
      {"log_error", {"log_error", "log.error(message[, fields])", "Error record to stderr."}},
  });
}

}  // namespace a

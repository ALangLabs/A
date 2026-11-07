#pragma once

#include <string>

#include "collections/transparent_map.hpp"
#include "name-resolution/builtin_doc.hpp"
#include "name-resolution/builtin_docs_math.hpp"
#include "name-resolution/builtin_docs_io.hpp"
#include "name-resolution/builtin_docs_collections.hpp"
#include "name-resolution/builtin_docs_unsafe.hpp"
#include "name-resolution/builtin_docs_py.hpp"
#include "name-resolution/builtin_docs_arena.hpp"
#include "name-resolution/builtin_docs_core.hpp"
#include "name-resolution/builtin_docs_convert.hpp"
#include "name-resolution/builtin_docs_json.hpp"
#include "name-resolution/builtin_docs_testing.hpp"
#include "name-resolution/builtin_docs_log.hpp"
#include "name-resolution/builtin_docs_metrics.hpp"
#include "name-resolution/builtin_docs_trace.hpp"
#include "name-resolution/builtin_docs_net.hpp"
#include "name-resolution/builtin_docs_kernels.hpp"
#include "name-resolution/builtin_docs_terminal.hpp"
#include "name-resolution/builtin_docs_intrinsics.hpp"

namespace a {

inline const StringMap<BuiltinDoc>& builtinDocs() {
  static const StringMap<BuiltinDoc> docs = [] {
    StringMap<BuiltinDoc> result;
    addMathDocs(result);
    addIoDocs(result);
    addCollectionsDocs(result);
    addUnsafeDocs(result);
    addPyDocs(result);
    addArenaDocs(result);
    addCoreDocs(result);
    addConvertDocs(result);
    addJsonDocs(result);
    addTestingDocs(result);
    addLogDocs(result);
    addMetricsDocs(result);
    addTraceDocs(result);
    addNetDocs(result);
    addKernelsDocs(result);
    addTerminalDocs(result);
    addIntrinsicsDocs(result);
    return result;
  }();
  return docs;
}

inline const BuiltinDoc* findBuiltinDoc(const std::string& name) {
  const auto found = builtinDocs().find(name);
  return found == builtinDocs().end() ? nullptr : &found->second;
}

}  // namespace a

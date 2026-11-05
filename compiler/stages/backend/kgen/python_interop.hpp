#pragma once
#ifdef A_HAVE_PYTHON
#include <Python.h>
#endif
#include <string>
#include <vector>
#include "memory/value.hpp"
#include "errors/runtime_error.hpp"
namespace a::python {
bool isAvailable();
bool ensureInitialized(const std::string& moduleHint = "");
Value importModule(const std::string& name, Span span);
Value getAttr(const Value& object, const std::string& attr, Span span);
Value getItem(const Value& object, const Value& key, Span span);
struct KeywordArg {
  std::string name;
  Value value;
};
Value callObject(const PythonObjectPtr& callable, const std::vector<Value>& args,
                 const std::vector<KeywordArg>& kwargs, Span span);
bool equals(const Value& lhs, const Value& rhs);
bool isPythonValue(const Value& v);
std::string repr(const Value& v);

// Operators that involve at least one Python object are evaluated by CPython
// itself (PyNumber_*/PyObject_RichCompareBool/PySequence_Contains), so things
// like `np_array * 2` or `x in torch_tensor` just work.
enum class PyBinOp {
  Add, Sub, Mul, Div, Mod, Pow,
  BitAnd, BitOr, BitXor, Shl, Shr,
  Eq, Ne, Lt, Gt, Le, Ge,
  In, NotIn,
  Invalid
};
Value binaryOp(PyBinOp op, const Value& lhs, const Value& rhs, Span span);
}

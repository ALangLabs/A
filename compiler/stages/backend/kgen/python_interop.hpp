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
void ensureInitialized();
Value importModule(const std::string& name, Span span);
Value getAttr(const Value& object, const std::string& attr, Span span);
Value getItem(const Value& object, const Value& key, Span span);
Value callObject(const PythonObjectPtr& callable, const std::vector<Value>& args, Span span);
bool equals(const Value& lhs, const Value& rhs);
bool isPythonValue(const Value& v);
std::string repr(const Value& v);
}

#include "backend/kgen/python_interop.hpp"
#include "strings/format.hpp"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace a::python {

#ifdef A_HAVE_PYTHON
PyObject* toPy(const Value& v, Span span);
#endif

namespace {

[[maybe_unused]] [[noreturn]] void unavailable(Span span, const std::string& what) {
  throw RuntimeError{span,
                     what + ": Python interop is not available in this build.",
                     "Rebuild with Python 3 development headers/libraries (python3-dev)."};
}

#ifdef A_HAVE_PYTHON

// Every entry point holds the GIL while touching the CPython API. The engine
// is single-threaded today, but PyGILState also keeps this correct if A ever
// runs Python calls off the main thread.
struct GilGuard {
  GilGuard() : state_(PyGILState_Ensure()) {}
  ~GilGuard() { PyGILState_Release(state_); }
  PyGILState_STATE state_;
};

void releasePy(void* handle) {
  PyObject* obj = static_cast<PyObject*>(handle);
  if (!obj) return;
  PyGILState_STATE state = PyGILState_Ensure();
  Py_DECREF(obj);
  PyGILState_Release(state);
}

// Takes ownership of `obj` (a new reference).
Value wrapPy(PyObject* obj) {
  if (!obj) return Value{};
  auto holder = std::make_shared<PythonObject>();
  holder->handle = obj;
  holder->release = &releasePy;
  Value v;
  v.data = PythonObjectPtr(std::move(holder));
  return v;
}

// `obj` is borrowed; the returned value keeps it alive.
Value wrapBorrowed(PyObject* obj) {
  Py_INCREF(obj);
  return wrapPy(obj);
}

PyObject* unwrap(const Value& v) {
  auto* holder = std::get_if<PythonObjectPtr>(&v.data);
  return holder ? static_cast<PyObject*>((*holder)->handle) : nullptr;
}

[[noreturn]] void failPy(Span span, const std::string& prefix) {
  std::string msg = prefix;
  if (PyErr_Occurred()) {
    PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    PyObject* s = value ? PyObject_Str(value) : nullptr;
    if (s) {
      const char* c = PyUnicode_AsUTF8(s);
      if (c) {
        msg += ": ";
        msg += c;
      }
      Py_DECREF(s);
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    PyErr_Clear();
  }
  throw RuntimeError{span, msg, ""};
}

std::string utf8FromPyUnicode(PyObject* obj) {
  Py_ssize_t size = 0;
  const char* data = PyUnicode_AsUTF8AndSize(obj, &size);
  return data ? std::string(data, static_cast<std::size_t>(size)) : std::string();
}

// Converts a CPython object into an A value. Immutable scalars and the
// builtin containers map onto native A values; everything else (modules,
// functions, class instances, ...) stays a PythonObject so attribute access
// and calls keep working through the C API.
Value fromPy(PyObject* obj) {
  if (!obj || obj == Py_None) return Value{};
  if (PyBool_Check(obj)) {
    Value v;
    v.data = (obj == Py_True);
    return v;
  }
  if (PyLong_Check(obj)) {
    int overflow = 0;
    long long number = PyLong_AsLongLongAndOverflow(obj, &overflow);
    if (!overflow) {
      Value v;
      v.data = static_cast<std::int64_t>(number);
      return v;
    }
    // Too large for i64: keep it as a Python float/object rather than failing.
    PyErr_Clear();
    double d = PyFloat_AsDouble(obj);
    if (!PyErr_Occurred()) {
      Value v;
      v.data = d;
      return v;
    }
    PyErr_Clear();
    return wrapBorrowed(obj);
  }
  if (PyFloat_Check(obj)) {
    Value v;
    v.data = PyFloat_AsDouble(obj);
    return v;
  }
  if (PyUnicode_Check(obj)) {
    Value v;
    v.data = utf8FromPyUnicode(obj);
    return v;
  }
  if (PyBytes_Check(obj)) {
    char* data = PyBytes_AsString(obj);
    Py_ssize_t size = PyBytes_Size(obj);
    Value v;
    v.data = data ? std::string(data, static_cast<std::size_t>(size)) : std::string();
    return v;
  }
  if (PyList_Check(obj) || PyTuple_Check(obj)) {
    const bool isList = PyList_Check(obj) != 0;
    Py_ssize_t n = isList ? PyList_Size(obj) : PyTuple_Size(obj);
    auto arr = std::make_shared<std::vector<Value>>();
    arr->reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
      PyObject* item = isList ? PyList_GetItem(obj, i) : PyTuple_GetItem(obj, i);
      arr->push_back(fromPy(item));
    }
    Value v;
    v.data = arr;
    return v;
  }
  if (PyDict_Check(obj)) {
    auto map = std::make_shared<MapObj>();
    PyObject *key = nullptr, *val = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &val)) {
      map->entries.emplace_back(fromPy(key), fromPy(val));
    }
    Value v;
    v.data = map;
    return v;
  }
  if (PySet_Check(obj) || PyFrozenSet_Check(obj)) {
    auto set = std::make_shared<SetObj>();
    PyObject* iterator = PyObject_GetIter(obj);
    if (iterator) {
      PyObject* item = nullptr;
      while ((item = PyIter_Next(iterator)) != nullptr) {
        set->elements.push_back(fromPy(item));
        Py_DECREF(item);
      }
      Py_DECREF(iterator);
    }
    Value v;
    v.data = set;
    return v;
  }
  return wrapBorrowed(obj);
}

PyObject* dictFromPairs(std::initializer_list<std::pair<const char*, PyObject*>> pairs) {
  PyObject* dict = PyDict_New();
  if (!dict) return nullptr;
  for (const auto& [key, value] : pairs) {
    PyObject* pyKey = PyUnicode_FromString(key);
    if (!pyKey || PyDict_SetItem(dict, pyKey, value) < 0) {
      Py_XDECREF(pyKey);
      Py_DECREF(dict);
      return nullptr;
    }
    Py_DECREF(pyKey);
  }
  return dict;
}

}  // namespace
#endif

bool isAvailable() {
#ifdef A_HAVE_PYTHON
  return true;
#else
  return false;
#endif
}

void ensureInitialized() {
#ifdef A_HAVE_PYTHON
  static bool inited = false;
  if (!inited) {
    Py_Initialize();
    // Let formatValue render PythonObject values through repr().
    setForeignValueRepr(&repr);
    inited = true;
  }
#endif
}

Value importModule(const std::string& name, Span span) {
#ifndef A_HAVE_PYTHON
  unavailable(span, "Cannot import '" + name + "'");
#else
  ensureInitialized();
  GilGuard gil;
  PyObject* mod = PyImport_ImportModule(name.c_str());
  if (!mod) failPy(span, "Failed to import Python module '" + name + "'");
  return wrapPy(mod);
#endif
}

Value getAttr(const Value& object, const std::string& attr, Span span) {
#ifndef A_HAVE_PYTHON
  unavailable(span, "Cannot access '" + attr + "'");
#else
  ensureInitialized();
  GilGuard gil;
  PyObject* target = unwrap(object);
  if (!target) {
    throw RuntimeError{span,
                       "Attribute access '" + attr + "' requires a Python object.",
                       "Only values produced by `import python:<pkg>` support this."};
  }
  PyObject* attrObj = PyObject_GetAttrString(target, attr.c_str());
  if (!attrObj) failPy(span, "Python object has no attribute '" + attr + "'");
  Value out = fromPy(attrObj);
  Py_DECREF(attrObj);
  return out;
#endif
}

Value getItem(const Value& object, const Value& key, Span span) {
#ifndef A_HAVE_PYTHON
  unavailable(span, "Cannot subscript a Python object");
#else
  ensureInitialized();
  GilGuard gil;
  PyObject* target = unwrap(object);
  if (!target) throw RuntimeError{span, "Subscript requires a Python object.", ""};
  PyObject* pyKey = toPy(key, span);
  if (!pyKey) failPy(span, "Failed to convert the index for Python subscript");
  PyObject* item = PyObject_GetItem(target, pyKey);
  Py_DECREF(pyKey);
  if (!item) failPy(span, "Python object is not subscriptable with that key");
  Value out = fromPy(item);
  Py_DECREF(item);
  return out;
#endif
}

Value callObject(const PythonObjectPtr& callable, const std::vector<Value>& args, Span span) {
#ifndef A_HAVE_PYTHON
  unavailable(span, "Cannot call a Python object");
#else
  ensureInitialized();
  GilGuard gil;
  PyObject* fn = callable ? static_cast<PyObject*>(callable->handle) : nullptr;
  if (!fn || !PyCallable_Check(fn)) {
    throw RuntimeError{span, "This Python object is not callable.",
                       "Only functions and other callables can be invoked."};
  }
  PyObject* tuple = PyTuple_New(static_cast<Py_ssize_t>(args.size()));
  if (!tuple) failPy(span, "Failed to build Python argument list");
  for (std::size_t i = 0; i < args.size(); ++i) {
    PyObject* arg = toPy(args[i], span);
    if (!arg) {
      Py_DECREF(tuple);
      failPy(span, "Failed to convert an argument for the Python call");
    }
    PyTuple_SET_ITEM(tuple, static_cast<Py_ssize_t>(i), arg);
  }
  PyObject* result = PyObject_Call(fn, tuple, nullptr);
  Py_DECREF(tuple);
  if (!result) failPy(span, "Python call failed");
  Value out = fromPy(result);
  Py_DECREF(result);
  return out;
#endif
}

bool equals(const Value& lhs, const Value& rhs) {
#ifndef A_HAVE_PYTHON
  (void)lhs;
  (void)rhs;
  return false;
#else
  ensureInitialized();
  GilGuard gil;
  PyObject* a = unwrap(lhs);
  PyObject* b = unwrap(rhs);
  if (!a || !b) return false;
  int result = PyObject_RichCompareBool(a, b, Py_EQ);
  if (result < 0) {
    PyErr_Clear();
    return false;
  }
  return result == 1;
#endif
}

#ifdef A_HAVE_PYTHON
PyObject* toPy(const Value& v, Span span) {
  if (std::holds_alternative<std::monostate>(v.data)) Py_RETURN_NONE;
  if (auto* b = std::get_if<bool>(&v.data)) {
    if (*b) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
  }
  if (auto* i = std::get_if<std::int64_t>(&v.data)) return PyLong_FromLongLong(*i);
  if (auto* d = std::get_if<double>(&v.data)) return PyFloat_FromDouble(*d);
  if (auto* c = std::get_if<char32_t>(&v.data)) {
    char buf[8];
    int n = 0;
    unsigned int cp = static_cast<unsigned int>(*c);
    if (cp < 0x80) {
      buf[0] = static_cast<char>(cp);
      n = 1;
    } else if (cp < 0x800) {
      buf[0] = static_cast<char>(0xC0 | (cp >> 6));
      buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
      n = 2;
    } else if (cp < 0x10000) {
      buf[0] = static_cast<char>(0xE0 | (cp >> 12));
      buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
      n = 3;
    } else {
      buf[0] = static_cast<char>(0xF0 | (cp >> 18));
      buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
      n = 4;
    }
    return PyUnicode_DecodeUTF8(buf, static_cast<Py_ssize_t>(n), "strict");
  }
  if (auto* s = std::get_if<std::string>(&v.data)) {
    return PyUnicode_DecodeUTF8(s->data(), static_cast<Py_ssize_t>(s->size()), "strict");
  }
  if (auto* a = std::get_if<ArrayPtr>(&v.data)) {
    PyObject* list = PyList_New(static_cast<Py_ssize_t>((*a)->size()));
    if (!list) return nullptr;
    for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>((*a)->size()); ++i) {
      PyObject* item = toPy((**a)[static_cast<std::size_t>(i)], span);
      if (!item) {
        Py_DECREF(list);
        return nullptr;
      }
      PyList_SET_ITEM(list, i, item);
    }
    return list;
  }
  if (auto* view = std::get_if<ArrayViewPtr>(&v.data)) {
    PyObject* list = PyList_New(static_cast<Py_ssize_t>((*view)->length));
    if (!list) return nullptr;
    const auto& storage = *(*view)->storage;
    for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>((*view)->length); ++i) {
      PyObject* item =
          toPy(storage[(*view)->offset + static_cast<std::size_t>(i)], span);
      if (!item) {
        Py_DECREF(list);
        return nullptr;
      }
      PyList_SET_ITEM(list, i, item);
    }
    return list;
  }
  if (auto* t = std::get_if<TuplePtr>(&v.data)) {
    PyObject* tup = PyTuple_New(static_cast<Py_ssize_t>((*t)->items.size()));
    if (!tup) return nullptr;
    for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>((*t)->items.size()); ++i) {
      PyObject* item = toPy((*t)->items[static_cast<std::size_t>(i)], span);
      if (!item) {
        Py_DECREF(tup);
        return nullptr;
      }
      PyTuple_SET_ITEM(tup, i, item);
    }
    return tup;
  }
  if (auto* m = std::get_if<MapPtr>(&v.data)) {
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;
    for (auto& [key, val] : (*m)->entries) {
      PyObject* pk = toPy(key, span);
      PyObject* pv = toPy(val, span);
      if (!pk || !pv || PyDict_SetItem(dict, pk, pv) < 0) {
        Py_XDECREF(pk);
        Py_XDECREF(pv);
        Py_DECREF(dict);
        return nullptr;
      }
      Py_DECREF(pk);
      Py_DECREF(pv);
    }
    return dict;
  }
  if (auto* set = std::get_if<SetPtr>(&v.data)) {
    PyObject* pySet = PySet_New(nullptr);
    if (!pySet) return nullptr;
    for (const Value& element : (*set)->elements) {
      PyObject* item = toPy(element, span);
      if (!item || PySet_Add(pySet, item) < 0) {
        Py_XDECREF(item);
        Py_DECREF(pySet);
        return nullptr;
      }
      Py_DECREF(item);
    }
    return pySet;
  }
  if (auto* o = std::get_if<OptionalPtr>(&v.data)) {
    if (!(*o)->hasValue) Py_RETURN_NONE;
    return toPy((*o)->value, span);
  }
  if (auto* r = std::get_if<ResultPtr>(&v.data)) {
    PyObject* ok = (*r)->ok ? Py_True : Py_False;
    PyObject* val = toPy((*r)->value, span);
    if (!val) return nullptr;
    PyObject* dict = dictFromPairs({{"ok", ok}, {"value", val}});
    Py_DECREF(val);
    return dict;
  }
  if (auto* s = std::get_if<StructPtr>(&v.data)) {
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;
    PyObject* typeName = PyUnicode_FromString((*s)->typeName.c_str());
    if (typeName && PyDict_SetItemString(dict, "__type__", typeName) == 0) {
      Py_DECREF(typeName);
      for (auto& field : (*s)->fields) {
        PyObject* key = PyUnicode_FromString(field.first.c_str());
        PyObject* val = toPy(field.second, span);
        if (!key || !val || PyDict_SetItem(dict, key, val) < 0) {
          Py_XDECREF(key);
          Py_XDECREF(val);
          Py_DECREF(dict);
          return nullptr;
        }
        Py_DECREF(key);
        Py_DECREF(val);
      }
      return dict;
    }
    Py_XDECREF(typeName);
    Py_DECREF(dict);
    return nullptr;
  }
  if (auto* e = std::get_if<EnumVariantPtr>(&v.data)) {
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;
    auto setStr = [&](const char* key, const std::string& value) {
      PyObject* pyKey = PyUnicode_FromString(key);
      PyObject* pyVal = PyUnicode_FromString(value.c_str());
      int rc = (pyKey && pyVal) ? PyDict_SetItem(dict, pyKey, pyVal) : -1;
      Py_XDECREF(pyKey);
      Py_XDECREF(pyVal);
      return rc;
    };
    if (setStr("enum", (*e)->enumName) < 0 || setStr("variant", (*e)->variant) < 0) {
      Py_DECREF(dict);
      return nullptr;
    }
    PyObject* tag = PyLong_FromLongLong((*e)->tag);
    if (!tag || PyDict_SetItemString(dict, "tag", tag) < 0) {
      Py_XDECREF(tag);
      Py_DECREF(dict);
      return nullptr;
    }
    Py_DECREF(tag);
    for (auto& field : (*e)->fields) {
      PyObject* key = PyUnicode_FromString(field.first.c_str());
      PyObject* val = toPy(field.second, span);
      if (!key || !val || PyDict_SetItem(dict, key, val) < 0) {
        Py_XDECREF(key);
        Py_XDECREF(val);
        Py_DECREF(dict);
        return nullptr;
      }
      Py_DECREF(key);
      Py_DECREF(val);
    }
    return dict;
  }
  if (auto* range = std::get_if<RangeValue>(&v.data)) {
    return PyObject_CallFunction((PyObject*)&PyRange_Type, "LLL",
                                 static_cast<long long>(range->start),
                                 static_cast<long long>(range->end),
                                 static_cast<long long>(range->step));
  }
  if (auto* py = std::get_if<PythonObjectPtr>(&v.data)) {
    PyObject* obj = static_cast<PyObject*>((*py)->handle);
    Py_XINCREF(obj);
    return obj;
  }
  throw RuntimeError{span,
                     "This A value cannot be passed to Python.",
                     "Only scalars, strings, arrays, maps, sets, tuples, optionals, "
                     "results, structs, enum variants, ranges, and Python objects "
                     "cross the boundary."};
}
#endif

bool isPythonValue(const Value& v) { return std::holds_alternative<PythonObjectPtr>(v.data); }

std::string repr(const Value& v) {
#ifdef A_HAVE_PYTHON
  if (!isPythonValue(v)) return {};
  try {
    ensureInitialized();
    GilGuard gil;
    PyObject* obj = unwrap(v);
    if (!obj) return {};
    PyObject* r = PyObject_Repr(obj);
    if (!r) {
      PyErr_Clear();
      return {};
    }
    std::string out = utf8FromPyUnicode(r);
    Py_DECREF(r);
    return out;
  } catch (...) {
    return {};
  }
#else
  (void)v;
  return {};
#endif
}

}  // namespace a::python

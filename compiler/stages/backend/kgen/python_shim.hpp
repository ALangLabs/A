#pragma once
// Redirects every Python C API entry point used in this translation unit through
// g_pySymbols, which python_loader.cpp fills via dlopen/dlsym. Include this
// AFTER Python.h (i.e. after python_interop.hpp). Only python_interop.cpp
// should include it — other TUs must see the real declarations.
#include "backend/kgen/python_symbols.hpp"

#ifdef A_HAVE_PYTHON

namespace a::python::detail {
inline void pyIncRef(PyObject* op) {
  if (op) g_pySymbols.sym_Py_IncRef(op);
}
inline void pyDecRef(PyObject* op) {
  if (op) g_pySymbols.sym_Py_DecRef(op);
}
}  // namespace a::python::detail

#define A_PY_G (::a::python::detail::g_pySymbols)

// Refcounting goes through Py_IncRef/Py_DecRef (real functions inside the
// loaded libpython) instead of the header's inline field access, so the
// compiled code does not depend on PyObject's exact layout.
#undef Py_INCREF
#undef Py_DECREF
#undef Py_XINCREF
#undef Py_XDECREF
#define Py_INCREF(op) ::a::python::detail::pyIncRef(op)
#define Py_DECREF(op) ::a::python::detail::pyDecRef(op)
#define Py_XINCREF(op) ::a::python::detail::pyIncRef(op)
#define Py_XDECREF(op) ::a::python::detail::pyDecRef(op)

// Functions.
#define Py_Initialize (A_PY_G.sym_Py_Initialize)
#define PyGILState_Ensure (A_PY_G.sym_PyGILState_Ensure)
#define PyGILState_Release (A_PY_G.sym_PyGILState_Release)
#define PyImport_ImportModule (A_PY_G.sym_PyImport_ImportModule)
#define PyObject_GetAttrString (A_PY_G.sym_PyObject_GetAttrString)
#define PyObject_GetItem (A_PY_G.sym_PyObject_GetItem)
#define PyObject_Call (A_PY_G.sym_PyObject_Call)
#define PyObject_CallFunction (A_PY_G.sym_PyObject_CallFunction)
#define PyObject_RichCompareBool (A_PY_G.sym_PyObject_RichCompareBool)
#define PyObject_Str (A_PY_G.sym_PyObject_Str)
#define PyObject_Repr (A_PY_G.sym_PyObject_Repr)
#define PyObject_GetIter (A_PY_G.sym_PyObject_GetIter)
#define PyIter_Next (A_PY_G.sym_PyIter_Next)
#define PyErr_Occurred (A_PY_G.sym_PyErr_Occurred)
#define PyErr_Clear (A_PY_G.sym_PyErr_Clear)
#define PyErr_Fetch (A_PY_G.sym_PyErr_Fetch)
#define PyErr_NormalizeException (A_PY_G.sym_PyErr_NormalizeException)
#define PySys_GetObject (A_PY_G.sym_PySys_GetObject)
#define PyUnicode_AsUTF8 (A_PY_G.sym_PyUnicode_AsUTF8)
#define PyUnicode_AsUTF8AndSize (A_PY_G.sym_PyUnicode_AsUTF8AndSize)
#define PyUnicode_DecodeUTF8 (A_PY_G.sym_PyUnicode_DecodeUTF8)
#define PyUnicode_DecodeFSDefault (A_PY_G.sym_PyUnicode_DecodeFSDefault)
#define PyUnicode_FromString (A_PY_G.sym_PyUnicode_FromString)
#define PyLong_AsLongLongAndOverflow (A_PY_G.sym_PyLong_AsLongLongAndOverflow)
#define PyLong_FromLongLong (A_PY_G.sym_PyLong_FromLongLong)
#define PyFloat_AsDouble (A_PY_G.sym_PyFloat_AsDouble)
#define PyFloat_FromDouble (A_PY_G.sym_PyFloat_FromDouble)
#define PyList_New (A_PY_G.sym_PyList_New)
#define PyList_Size (A_PY_G.sym_PyList_Size)
#define PyList_GetItem (A_PY_G.sym_PyList_GetItem)
#define PyList_Append (A_PY_G.sym_PyList_Append)
#define PyTuple_New (A_PY_G.sym_PyTuple_New)
#define PyTuple_Size (A_PY_G.sym_PyTuple_Size)
#define PyTuple_GetItem (A_PY_G.sym_PyTuple_GetItem)
#define PyDict_New (A_PY_G.sym_PyDict_New)
#define PyDict_SetItem (A_PY_G.sym_PyDict_SetItem)
#define PyDict_SetItemString (A_PY_G.sym_PyDict_SetItemString)
#define PyDict_Next (A_PY_G.sym_PyDict_Next)
#define PySet_New (A_PY_G.sym_PySet_New)
#define PySet_Add (A_PY_G.sym_PySet_Add)
#define PyBytes_AsString (A_PY_G.sym_PyBytes_AsString)
#define PyBytes_Size (A_PY_G.sym_PyBytes_Size)
#define PyType_IsSubtype (A_PY_G.sym_PyType_IsSubtype)
#define PyCallable_Check (A_PY_G.sym_PyCallable_Check)
#define PyNumber_Add (A_PY_G.sym_PyNumber_Add)
#define PyNumber_Subtract (A_PY_G.sym_PyNumber_Subtract)
#define PyNumber_Multiply (A_PY_G.sym_PyNumber_Multiply)
#define PyNumber_TrueDivide (A_PY_G.sym_PyNumber_TrueDivide)
#define PyNumber_Remainder (A_PY_G.sym_PyNumber_Remainder)
#define PyNumber_Power (A_PY_G.sym_PyNumber_Power)
#define PyNumber_And (A_PY_G.sym_PyNumber_And)
#define PyNumber_Or (A_PY_G.sym_PyNumber_Or)
#define PyNumber_Xor (A_PY_G.sym_PyNumber_Xor)
#define PyNumber_Lshift (A_PY_G.sym_PyNumber_Lshift)
#define PyNumber_Rshift (A_PY_G.sym_PyNumber_Rshift)
#define PySequence_Contains (A_PY_G.sym_PySequence_Contains)
#define PyNumber_Index (A_PY_G.sym_PyNumber_Index)
#define PyNumber_Float (A_PY_G.sym_PyNumber_Float)

// Singletons and type objects (addresses inside libpython).
#undef Py_None
#undef Py_True
#undef Py_False
#define Py_None (A_PY_G.sym__Py_NoneStruct)
#define Py_True (A_PY_G.sym__Py_TrueStruct)
#define Py_False (A_PY_G.sym__Py_FalseStruct)
#define PyBool_Type (*A_PY_G.sym_PyBool_Type)
#define PyLong_Type (*A_PY_G.sym_PyLong_Type)
#define PyFloat_Type (*A_PY_G.sym_PyFloat_Type)
#define PyUnicode_Type (*A_PY_G.sym_PyUnicode_Type)
#define PyBytes_Type (*A_PY_G.sym_PyBytes_Type)
#define PyList_Type (*A_PY_G.sym_PyList_Type)
#define PyTuple_Type (*A_PY_G.sym_PyTuple_Type)
#define PyDict_Type (*A_PY_G.sym_PyDict_Type)
#define PySet_Type (*A_PY_G.sym_PySet_Type)
#define PyFrozenSet_Type (*A_PY_G.sym_PyFrozenSet_Type)
#define PyRange_Type (*A_PY_G.sym_PyRange_Type)

// Type checks: the header's Py*_Check inlines were already parsed against the
// real symbols, so redirect the check macros themselves. Py_TYPE/Py_IS_TYPE
// are field accesses (no external symbols); subtype tests go through the
// dlsym'd PyType_IsSubtype and the loaded type objects above.
#undef PyObject_TypeCheck
#define PyObject_TypeCheck(ob, type) \
  ((ob) != nullptr && \
   (Py_TYPE(ob) == (type) || A_PY_G.sym_PyType_IsSubtype(Py_TYPE(ob), (type))))
#undef PyBool_Check
#define PyBool_Check(op) PyObject_TypeCheck((op), &PyBool_Type)
#undef PyLong_Check
#define PyLong_Check(op) PyObject_TypeCheck((op), &PyLong_Type)
#undef PyFloat_Check
#define PyFloat_Check(op) PyObject_TypeCheck((op), &PyFloat_Type)
#undef PyUnicode_Check
#define PyUnicode_Check(op) PyObject_TypeCheck((op), &PyUnicode_Type)
#undef PyBytes_Check
#define PyBytes_Check(op) PyObject_TypeCheck((op), &PyBytes_Type)
#undef PyList_Check
#define PyList_Check(op) PyObject_TypeCheck((op), &PyList_Type)
#undef PyTuple_Check
#define PyTuple_Check(op) PyObject_TypeCheck((op), &PyTuple_Type)
#undef PyDict_Check
#define PyDict_Check(op) PyObject_TypeCheck((op), &PyDict_Type)
#undef PySet_Check
#define PySet_Check(op) PyObject_TypeCheck((op), &PySet_Type)
#undef PyFrozenSet_Check
#define PyFrozenSet_Check(op) PyObject_TypeCheck((op), &PyFrozenSet_Type)
#undef PyCallable_Check
#define PyCallable_Check(op) (A_PY_G.sym_PyCallable_Check((op)))

// Py_SIZE is inline-only (no exported symbol) and its body asserts against
// PyLong_Type/PyBool_Type; read ob_size directly instead.
#undef Py_SIZE
#define Py_SIZE(ob) (_PyVarObject_CAST(ob)->ob_size)

#endif  // A_HAVE_PYTHON

#pragma once
// Symbol table for the CPython that python_loader.cpp discovers and dlopens
// at runtime. python_interop.cpp never links libpython directly; it calls
// through these pointers (see python_shim.hpp), so `a` embeds whichever
// interpreter it finds — uv-managed, venv, or system — like Mojo does.
#include <string>
#include <vector>

#ifdef A_HAVE_PYTHON
#include <Python.h>
#endif

namespace a::python::detail {

#ifdef A_HAVE_PYTHON
struct PySymbols {
  // Functions (resolved with dlsym).
  decltype(&Py_Initialize) sym_Py_Initialize = nullptr;
  decltype(&PyGILState_Ensure) sym_PyGILState_Ensure = nullptr;
  decltype(&PyGILState_Release) sym_PyGILState_Release = nullptr;
  decltype(&PyImport_ImportModule) sym_PyImport_ImportModule = nullptr;
  decltype(&PyObject_GetAttrString) sym_PyObject_GetAttrString = nullptr;
  decltype(&PyObject_GetItem) sym_PyObject_GetItem = nullptr;
  decltype(&PyObject_Call) sym_PyObject_Call = nullptr;
  decltype(&PyObject_CallFunction) sym_PyObject_CallFunction = nullptr;
  decltype(&PyObject_RichCompareBool) sym_PyObject_RichCompareBool = nullptr;
  decltype(&PyObject_Str) sym_PyObject_Str = nullptr;
  decltype(&PyObject_Repr) sym_PyObject_Repr = nullptr;
  decltype(&PyObject_GetIter) sym_PyObject_GetIter = nullptr;
  decltype(&PyIter_Next) sym_PyIter_Next = nullptr;
  decltype(&PyErr_Occurred) sym_PyErr_Occurred = nullptr;
  decltype(&PyErr_Clear) sym_PyErr_Clear = nullptr;
  decltype(&PyErr_Fetch) sym_PyErr_Fetch = nullptr;
  decltype(&PyErr_NormalizeException) sym_PyErr_NormalizeException = nullptr;
  decltype(&PySys_GetObject) sym_PySys_GetObject = nullptr;
  decltype(&PyUnicode_AsUTF8) sym_PyUnicode_AsUTF8 = nullptr;
  decltype(&PyUnicode_AsUTF8AndSize) sym_PyUnicode_AsUTF8AndSize = nullptr;
  decltype(&PyUnicode_DecodeUTF8) sym_PyUnicode_DecodeUTF8 = nullptr;
  decltype(&PyUnicode_DecodeFSDefault) sym_PyUnicode_DecodeFSDefault = nullptr;
  decltype(&PyUnicode_FromString) sym_PyUnicode_FromString = nullptr;
  decltype(&PyLong_AsLongLongAndOverflow) sym_PyLong_AsLongLongAndOverflow = nullptr;
  decltype(&PyLong_FromLongLong) sym_PyLong_FromLongLong = nullptr;
  decltype(&PyFloat_AsDouble) sym_PyFloat_AsDouble = nullptr;
  decltype(&PyFloat_FromDouble) sym_PyFloat_FromDouble = nullptr;
  decltype(&PyList_New) sym_PyList_New = nullptr;
  decltype(&PyList_Size) sym_PyList_Size = nullptr;
  decltype(&PyList_GetItem) sym_PyList_GetItem = nullptr;
  decltype(&PyList_Append) sym_PyList_Append = nullptr;
  decltype(&PyTuple_New) sym_PyTuple_New = nullptr;
  decltype(&PyTuple_Size) sym_PyTuple_Size = nullptr;
  decltype(&PyTuple_GetItem) sym_PyTuple_GetItem = nullptr;
  decltype(&PyDict_New) sym_PyDict_New = nullptr;
  decltype(&PyDict_SetItem) sym_PyDict_SetItem = nullptr;
  decltype(&PyDict_SetItemString) sym_PyDict_SetItemString = nullptr;
  decltype(&PyDict_Next) sym_PyDict_Next = nullptr;
  decltype(&PySet_New) sym_PySet_New = nullptr;
  decltype(&PySet_Add) sym_PySet_Add = nullptr;
  decltype(&PyBytes_AsString) sym_PyBytes_AsString = nullptr;
  decltype(&PyBytes_Size) sym_PyBytes_Size = nullptr;
  decltype(&PyType_IsSubtype) sym_PyType_IsSubtype = nullptr;
  decltype(&PyCallable_Check) sym_PyCallable_Check = nullptr;
  decltype(&PyNumber_Add) sym_PyNumber_Add = nullptr;
  decltype(&PyNumber_Subtract) sym_PyNumber_Subtract = nullptr;
  decltype(&PyNumber_Multiply) sym_PyNumber_Multiply = nullptr;
  decltype(&PyNumber_TrueDivide) sym_PyNumber_TrueDivide = nullptr;
  decltype(&PyNumber_Remainder) sym_PyNumber_Remainder = nullptr;
  decltype(&PyNumber_Power) sym_PyNumber_Power = nullptr;
  decltype(&PyNumber_And) sym_PyNumber_And = nullptr;
  decltype(&PyNumber_Or) sym_PyNumber_Or = nullptr;
  decltype(&PyNumber_Xor) sym_PyNumber_Xor = nullptr;
  decltype(&PyNumber_Lshift) sym_PyNumber_Lshift = nullptr;
  decltype(&PyNumber_Rshift) sym_PyNumber_Rshift = nullptr;
  decltype(&PySequence_Contains) sym_PySequence_Contains = nullptr;
  decltype(&PyNumber_Index) sym_PyNumber_Index = nullptr;
  decltype(&PyNumber_Float) sym_PyNumber_Float = nullptr;
  decltype(&Py_IncRef) sym_Py_IncRef = nullptr;
  decltype(&Py_DecRef) sym_Py_DecRef = nullptr;

  // Data objects: these hold the *address* of the real object inside
  // libpython (what the linker would have resolved &PyList_Type to).
  PyObject* sym__Py_NoneStruct = nullptr;
  PyObject* sym__Py_TrueStruct = nullptr;
  PyObject* sym__Py_FalseStruct = nullptr;
  PyTypeObject* sym_PyBool_Type = nullptr;
  PyTypeObject* sym_PyLong_Type = nullptr;
  PyTypeObject* sym_PyFloat_Type = nullptr;
  PyTypeObject* sym_PyUnicode_Type = nullptr;
  PyTypeObject* sym_PyBytes_Type = nullptr;
  PyTypeObject* sym_PyList_Type = nullptr;
  PyTypeObject* sym_PyTuple_Type = nullptr;
  PyTypeObject* sym_PyDict_Type = nullptr;
  PyTypeObject* sym_PySet_Type = nullptr;
  PyTypeObject* sym_PyFrozenSet_Type = nullptr;
  PyTypeObject* sym_PyRange_Type = nullptr;
};

extern PySymbols g_pySymbols;

// Result of the one-time interpreter discovery + dlopen.
struct PythonRuntimeInfo {
  int status = 0;  // 0 = not attempted, 1 = loaded, 2 = failed
  std::string description;      // e.g. "CPython 3.13.14 (/path/libpython3.13.so)"
  std::string error;            // why discovery failed, when status == 2
  std::string pythonHome;       // sys.base_prefix of the chosen interpreter
  std::vector<std::string> siteDirs;  // purelib/platlib/stdlib of chosen env
};

extern PythonRuntimeInfo g_pythonRuntime;

// Finds a usable libpython (honoring A_PYTHON / A_PYTHON_LIB, project venvs,
// VIRTUAL_ENV/uv/conda envs, PATH pythons, uv installs, system libs), dlopens
// it and resolves every symbol in g_pySymbols. When moduleHint is given,
// environments that actually contain that top-level module are preferred.
// Idempotent; the result is cached in g_pythonRuntime.
bool ensurePythonLoaded(const std::string& moduleHint = "");
const PythonRuntimeInfo& pythonRuntimeInfo();

#endif  // A_HAVE_PYTHON
}  // namespace a::python::detail
